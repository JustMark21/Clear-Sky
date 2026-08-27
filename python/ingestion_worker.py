#!/usr/bin/env python3
"""
L3S-LEO Ingestion Worker
=========================

The data-source half of the hybrid pipeline. This process is the ONLY
thing that talks to the outside world; the C++ engine (`l3s_engine
--source live`) never makes a network call, and the existing
engine -> dashboard PUB/SUB telemetry channel is completely untouched --
this worker only feeds the engine's *inbound* PULL socket (see
include/l3s/Ingestion.hpp for the exact wire format both sides share):

    [this worker] --PUSH/PULL(tcp://*:5557)--> [l3s_engine --source live] --PUB/SUB(5556)--> [dashboard]

Each cycle it tries to build one scene (a `--overpasses`-length list of
SST/VZA/validity grids) from a real public GHRSST source, and falls back
to a clean, physically-motivated synthetic scene on ANY failure --
network error, timeout, HTTP error, malformed response, all-NaN tile.
That fallback is not a rare corner case here, it is a first-class, always
-exercised code path: pass --source synthetic to use it exclusively (zero
network calls, fully deterministic modulo --seed) for a guaranteed-safe
hackathon demo, or leave --source auto (default) to prefer live data and
degrade automatically when it is not available.

Live data source (verified reachable from this environment at write time):
NOAA/JPL "jplMURSST41" -- the GHRSST Level-4 MUR SST analysis -- served
from the public NOAA CoastWatch ERDDAP at coastwatch.pfeg.noaa.gov, via a
plain ERDDAP griddap .csv query (no extra SDK, just `requests`). Default
AOI is the Agulhas Current, South Africa -- the same dynamic-thermal-front
region used in the paper's own Fig. 8/9 debiasing demonstration.

FOUR INDEPENDENT FETCHES PER SCENE: each overpass (A1, B1, A2, C1, ...)
is built from its OWN real ERDDAP query, not one fetch reused four times.
By default that means the same AOI and dataset but four different real
calendar days (anchor, anchor-1, anchor-2, anchor-3 -- see --erddap-day-
offsets), which is both a faithful analog of the paper's actual A1/B1/A2/
C1 scenario (near-identical geography, genuinely different acquisition
times within the collation window) and something this worker can verify
end to end: MUR's daily SST values at a fixed point measurably differ
day to day (confirmed live: 27.81 / 27.78 / 27.69 / 27.25 degC across 4
consecutive real days at one test point), so each overpass carries an
authentic, non-cloned spatial distribution rather than the same tile
repeated with only a synthetic overlay on top. --erddap-datasets lets you
point individual slots at different real dataset IDs too (e.g. if you
have access to distinct per-instrument products), cycling through the
list if you give fewer than --overpasses entries.

"anchor" above is the most recent day _find_latest_published_grid()
actually confirmed is published, NOT necessarily today: JPL's MUR SST
processing lag is not perfectly constant, so a granule that was available
one cycle can 404 the next. The search starts --erddap-lookback-start-
days back (2, to clear typical processing time) and, only on an HTTP 404,
steps one day further back at a time up to --erddap-lookback-max-days (7)
until it finds one that's actually there -- a transient 404 degrades
gracefully to trying an older day, not to a synthetic fallback for the
whole scene.

NOTE on ERDDAP time syntax: "last-N" relative-time arithmetic was tested
against jplMURSST41 and did NOT step backwards as its own name implies
(all of last, last-1, last-2, last-3 resolved to the identical timestamp)
-- so this worker deliberately uses explicit ISO calendar dates instead
(e.g. "2026-08-19"), which ERDDAP snaps to the nearest valid grid time on
that date and which was verified to return genuinely distinct real data.

IMPORTANT CAVEAT: MUR is a gap-free L4 *analysis* (one of the exact
foundation-SST products the paper itself lists as ACSPO's first-guess
reference -- CMC, OSTIA, GPB, MUR, RAMSSA, Reynolds), not a per-sensor L2/
L3U swath product, so it carries no native per-pixel view-zenith-angle or
cloud mask. This worker uses MUR's real, live-observed temperatures as
the physical SST field for each overpass, then overlays a synthetic-but-
physically-modeled swath VZA geometry and cloud mask per overpass to
emulate what per-sensor L3U granules would look like -- documented at
every point below. If your NOAA CoastWatch / PO.DAAC credentials give you
access to genuine ACSPO L3U granules (the paper's own ref [8]), point
--erddap-datasets at that source instead; the OverpassArrays contract
downstream is unaffected either way.

Usage:
    python3 ingestion_worker.py --source synthetic                   # offline, always safe
    python3 ingestion_worker.py --source auto                        # try live, auto-fallback
    python3 ingestion_worker.py --source auto --engine-endpoint tcp://localhost:5557
"""
from __future__ import annotations

import argparse
import logging
import math
import sys
import time
from dataclasses import dataclass
from datetime import date, timedelta

import numpy as np
import zmq

try:
    import requests
    _HAVE_REQUESTS = True
except ImportError:  # pragma: no cover - degrade gracefully, see fetch_live_grid()
    _HAVE_REQUESTS = False

import struct

# ---------------------------------------------------------------------------
# Wire protocol -- must match include/l3s/Ingestion.hpp EXACTLY (field
# order, byte widths, and the little-endian '<' prefix on every format).
# ---------------------------------------------------------------------------
MAGIC = 0x3253334C  # "L3S2"
SCENE_HEADER_FMT = "<6Id"   # magic,width,height,numOverpasses,dataSource,sceneId | timestampUnixS
SCENE_HEADER_SIZE = struct.calcsize(SCENE_HEADER_FMT)   # 32 bytes
OVERPASS_META_FMT = "<8sf"  # name (8 bytes, NUL-padded ASCII) | avgVZA
OVERPASS_META_SIZE = struct.calcsize(OVERPASS_META_FMT)  # 12 bytes

DATA_SOURCE_SYNTHETIC = 0
DATA_SOURCE_LIVE = 1

NAMES = ["A1", "B1", "A2", "C1", "D1", "E1"]


def pack_scene_header(width: int, height: int, num_overpasses: int, data_source: int,
                       scene_id: int, timestamp: float) -> bytes:
    return struct.pack(SCENE_HEADER_FMT, MAGIC, width, height, num_overpasses,
                        data_source, scene_id, timestamp)


def pack_overpass_meta(name: str, avg_vza: float) -> bytes:
    name_bytes = name.encode("ascii", "replace")[:8].ljust(8, b"\0")
    return struct.pack(OVERPASS_META_FMT, name_bytes, avg_vza)


@dataclass
class OverpassArrays:
    name: str
    avg_vza: float
    sst: np.ndarray    # float32 (height, width), Kelvin, ALWAYS finite (see note below)
    vza: np.ndarray    # float32 (height, width), degrees
    valid: np.ndarray  # uint8   (height, width), 1 = has a retrieval, 0 = cloud/no-data


def send_scene(socket: "zmq.Socket", width: int, height: int, overpasses: list[OverpassArrays],
               data_source: int, scene_id: int) -> bool:
    """Serializes and PUSHes one scene. Returns False (after logging) rather
    than raising or blocking forever if the engine isn't draining fast
    enough -- the bounded SNDHWM/SNDTIMEO set in run_worker() are what
    actually enforce that; this function just reports the outcome so the
    caller can log a clean "scene N sent/dropped" line."""
    parts = [b"L3SIN", pack_scene_header(width, height, len(overpasses), data_source, scene_id, time.time())]
    parts.append(b"".join(pack_overpass_meta(op.name, op.avg_vza) for op in overpasses))

    for op in overpasses:
        # Defensive shape/dtype/finiteness checks BEFORE anything goes on
        # the wire -- exactly the class of bug ("floating-point mismatch",
        # buffer size mismatch) the receiving C++ side is also guarding
        # against independently. Fail loudly and locally rather than
        # shipping a payload the engine would have to reject anyway.
        assert op.sst.shape == (height, width), f"sst shape {op.sst.shape} != {(height, width)}"
        assert op.vza.shape == (height, width), f"vza shape {op.vza.shape} != {(height, width)}"
        assert op.valid.shape == (height, width), f"valid shape {op.valid.shape} != {(height, width)}"
        sst32 = np.ascontiguousarray(op.sst, dtype=np.float32)
        vza32 = np.ascontiguousarray(op.vza, dtype=np.float32)
        valid8 = np.ascontiguousarray(op.valid, dtype=np.uint8)
        if not np.isfinite(sst32).all():
            raise ValueError(f"overpass {op.name}: sst contains non-finite values under valid=1 pixels")
        parts.append(sst32.tobytes())
        parts.append(vza32.tobytes())
        parts.append(valid8.tobytes())

    try:
        socket.send_multipart(parts, flags=0)
        return True
    except zmq.Again:
        logging.warning("engine not draining fast enough (PUSH send timed out) -- dropping scene %d", scene_id)
        return False
    except zmq.ZMQError as e:
        logging.warning("ZMQ send failed (%s) -- dropping scene %d", e, scene_id)
        return False


# ---------------------------------------------------------------------------
# Synthetic scene generator -- a lightweight numpy port of the same
# physical model as include/l3s/Simulator.hpp (smooth macro thermal front
# + swath-geometry VZA + organic cloud gaps/leakage). It does not need to
# be bit-identical to the C++ simulator; it only needs to be a clean,
# always-available, physically-plausible scene so the demo never stalls.
# ---------------------------------------------------------------------------
def _true_field(xs: np.ndarray, ys: np.ndarray, width: int, height: int, t: float) -> np.ndarray:
    nx = xs / width
    ny = ys / height
    base = 288.0 + 6.0 * ny
    front_x = 0.45 + 0.06 * np.sin(ny * 6.0 + t * 0.03) + 0.02 * math.sin(t * 0.017)
    front = 1.8 * np.tanh((nx - front_x) * 14.0)
    eddy = 0.6 * np.sin(nx * 9.0 + t * 0.02) * np.cos(ny * 7.0 - t * 0.015)
    return base + front + eddy


def _swath_vza(xs: np.ndarray, width: int, k: int, num_overpasses: int, t: float) -> np.ndarray:
    phase = k / num_overpasses
    swath_center_x = width * (0.15 + 0.7 * phase)
    max_vza = 20.0 + 55.0 * (0.3 + 0.7 * abs(math.sin(k * 1.7 + t * 0.05)))
    return np.minimum(85.0, max_vza * np.abs(xs - swath_center_x) / (width * 0.5)).astype(np.float32)


def _apply_cloud_overlay(sst: np.ndarray, valid: np.ndarray, xs: np.ndarray, ys: np.ndarray,
                          width: int, height: int, rng: np.random.Generator, n_blobs: int = 6,
                          forced_blobs: list[tuple[float, float, float]] | None = None) -> None:
    """In-place: punches organic cloud gaps into `valid` and applies a
    cold cloud-leakage bias in the boundary band around each gap, mirroring
    the exact failure mode Sec. 2.1 of the paper describes.

    `n_blobs` random blobs are drawn independently EVERY call -- i.e.
    independently per overpass, since this is called once per overpass in
    synth_scene()/live_scene() -- so their overlap across overpasses is
    incidental, not guaranteed (verified: 0.00% all-4-clouded pixels
    across 5 seeds at defaults). `forced_blobs`, in contrast, is a list of
    explicit (cx, cy, r) tuples the CALLER draws ONCE per scene and passes
    identically into every overpass's call -- that is what guarantees (not
    merely allows) the same coordinates end up masked in every overpass.
    """
    base_r = 0.09 * min(width, height)
    blobs = [(rng.uniform(0, width), rng.uniform(0, height), base_r * rng.uniform(0.7, 1.6))
              for _ in range(n_blobs)]
    if forced_blobs:
        blobs.extend(forced_blobs)
    for cx, cy, r in blobs:
        dist = np.sqrt((xs - cx) ** 2 + (ys - cy) ** 2) - r
        valid[dist < 0] = 0
        leak_band = (dist >= 0) & (dist < r * 0.5)
        proximity = np.clip(1.0 - dist / (r * 0.5), 0.0, 1.0)
        sst -= np.where(leak_band, (1.0 + 2.0 * proximity) * np.clip(1.0 - proximity * 0.3, 0.0, None), 0.0)


def _maybe_persistent_storm(width: int, height: int, rng: np.random.Generator,
                             probability: float) -> list[tuple[float, float, float]] | None:
    """Drawn ONCE per scene (not per overpass, unlike the random blobs in
    _apply_cloud_overlay). With `probability`, returns a single deliberately
    large (cx, cy, r) blob to be forced identically into every overpass's
    cloud overlay -- the only way to GUARANTEE (not just statistically hope
    for) pixels that are 100% obscured in all overpasses simultaneously,
    proving Engine.hpp's all-invalid-pixel fallback (buildWeightedComposite's
    wsum==0 branch, buildLvzaComposite's found==false branch) actually
    executes, not just that it's theoretically reachable."""
    if probability <= 0.0 or rng.uniform(0.0, 1.0) >= probability:
        return None
    cx = rng.uniform(width * 0.3, width * 0.7)
    cy = rng.uniform(height * 0.3, height * 0.7)
    r = 0.22 * min(width, height)  # deliberately "massive" relative to the 0.09x random blobs
    return [(cx, cy, r)]


def synth_scene(width: int, height: int, num_overpasses: int, t: float,
                 rng: np.random.Generator, persistent_storm_probability: float = 0.0) -> list[OverpassArrays]:
    xs, ys = np.meshgrid(np.arange(width, dtype=np.float64), np.arange(height, dtype=np.float64))
    true_sst = _true_field(xs, ys, width, height, t)
    forced_blobs = _maybe_persistent_storm(width, height, rng, persistent_storm_probability)

    overpasses = []
    for k in range(num_overpasses):
        vza = _swath_vza(xs, width, k, num_overpasses, t)
        sec_theta = 1.0 / np.cos(np.deg2rad(vza))
        atm_bias = -0.35 * (sec_theta - 1.0)
        sst = (true_sst + atm_bias).astype(np.float64)
        valid = np.ones((height, width), dtype=np.uint8)

        _apply_cloud_overlay(sst, valid, xs, ys, width, height, rng, forced_blobs=forced_blobs)

        # sst must stay finite everywhere (see OverpassArrays docstring) --
        # under cloud-masked pixels the value is simply unused downstream
        # (every math path in Engine.hpp checks `valid` first), but a
        # placeholder finite value keeps the wire payload unambiguous.
        sst = np.nan_to_num(sst, nan=288.0, posinf=310.0, neginf=270.0).astype(np.float32)
        avg_vza = float(vza[valid == 1].mean()) if (valid == 1).any() else float(vza.mean())
        overpasses.append(OverpassArrays(NAMES[k % len(NAMES)], avg_vza, sst, vza, valid))
    return overpasses


# ---------------------------------------------------------------------------
# Live data source: NOAA CoastWatch ERDDAP griddap (see module docstring).
# ---------------------------------------------------------------------------
def fetch_live_grid(width: int, height: int, base_url: str, dataset: str, variable: str,
                     lat0: float, lat1: float, lon0: float, lon1: float,
                     timeout_s: float, time_selector: str = "last") -> np.ndarray:
    """Fetches ONE real SST tile (Kelvin, float32, shape (height, width))
    from a public ERDDAP griddap endpoint. Raises on ANY failure --
    network, HTTP status, empty/malformed CSV, all-NaN tile -- so callers
    can catch-and-fallback uniformly; this never returns a partially-bad
    grid.

    `time_selector` is either the literal string "last" (most recent
    available step) or an explicit ISO date "YYYY-MM-DD" (ERDDAP snaps to
    the nearest valid grid time on that date). Deliberately NOT
    "last-N" -- tested against jplMURSST41 and it did not step backwards
    (see module docstring); explicit dates are the verified-working way
    to fetch a genuinely different real day per overpass."""
    if not _HAVE_REQUESTS:
        raise RuntimeError("the 'requests' package is not installed (pip install requests)")

    time_expr = "last" if time_selector == "last" else time_selector
    ctx = f"{dataset}@{time_expr}"  # identifies which of several independent fetches failed, in logs
    url = (f"{base_url}/{dataset}.csv?{variable}[({time_expr})]"
           f"[({lat0}):({lat1})][({lon0}):({lon1})]")
    resp = requests.get(url, timeout=timeout_s)
    resp.raise_for_status()

    # ERDDAP .csv layout: row0 = column names, row1 = units, then data
    # rows "time,latitude,longitude,<variable>".
    lines = resp.text.strip().splitlines()
    if len(lines) < 3:
        raise ValueError(f"[{ctx}] ERDDAP returned no data rows ({len(lines)} lines total)")

    lats: list[float] = []
    lons: list[float] = []
    vals: list[float] = []
    for line in lines[2:]:
        cols = line.split(",")
        if len(cols) != 4:
            continue
        lats.append(float(cols[1]))
        lons.append(float(cols[2]))
        vals.append(float(cols[3]) if cols[3] else math.nan)
    if not vals:
        raise ValueError(f"[{ctx}] ERDDAP response had no parseable data rows")

    lat_u = sorted(set(lats))
    lon_u = sorted(set(lons))
    lat_index = {v: i for i, v in enumerate(lat_u)}
    lon_index = {v: i for i, v in enumerate(lon_u)}
    grid = np.full((len(lat_u), len(lon_u)), np.nan, dtype=np.float64)
    for la, lo, v in zip(lats, lons, vals):
        grid[lat_index[la], lon_index[lo]] = v

    # DO NOT flip rows here. `lat_u` is ascending, so row 0 is already the
    # southernmost latitude and the last row is the northernmost -- i.e.
    # row index already increases northward, which is exactly what
    # subscriber.py's `ax.imshow(..., origin="lower")` expects (row 0
    # drawn at the bottom of the axes). A [::-1] flip here would put the
    # northernmost row at index 0, which origin="lower" would then draw
    # at the *bottom* -- upside-down geography. Just convert degC -> Kelvin.
    grid = grid + 273.15

    if not np.isfinite(grid).any():
        raise ValueError(f"[{ctx}] fetched tile is entirely NaN (land/ice mask, or AOI too small/misplaced)")

    # Crop -- NEVER resample/interpolate, every retained pixel must stay a
    # real, unmodified observation -- to exactly (height, width), anchored
    # at the NW corner. If the AOI/resolution combination under-fills the
    # requested grid, pad with NaN (which becomes valid=0 downstream)
    # rather than fabricating values.
    out = np.full((height, width), np.nan, dtype=np.float64)
    h = min(height, grid.shape[0])
    w = min(width, grid.shape[1])
    out[:h, :w] = grid[:h, :w]
    return out


def _find_latest_published_grid(width: int, height: int, base_url: str, dataset: str, variable: str,
                                 lat0: float, lat1: float, lon0: float, lon1: float, timeout_s: float,
                                 start_offset_days: int = 2, max_offset_days: int = 7):
    """Searches backward, one day at a time, for the most recent day
    NOAA CoastWatch ERDDAP has actually finished publishing for `dataset`,
    instead of assuming any fixed offset (e.g. "yesterday") is always
    safe -- JPL's processing lag is not perfectly constant, so an offset
    that worked last cycle can 404 the next.

    Starts at start_offset_days (2, to clear typical processing time)
    and, ONLY on an HTTP 404 (the specific "not published yet" signal --
    anything else, a timeout/connection error/malformed CSV/all-NaN
    tile/etc., is a different failure mode and is NOT retried here; it
    propagates immediately exactly like a single fetch_live_grid call
    would), steps the offset forward one day at a time up to
    max_offset_days.

    Returns (grid, resolved_date, offset_days) for the first day that
    succeeds -- the caller gets the fetched tile back too, so it never
    has to re-fetch the same day a second time. Raises the last exception
    seen if nothing in [start_offset_days, max_offset_days] is published;
    this is a plain exception, not a crash -- run_worker()'s existing
    live-fetch try/except is what turns that into a same-cycle fallback
    to synth_scene(), so a fully-exhausted search degrades the pipeline
    for one cycle instead of taking it down."""
    last_exc: Exception | None = None
    for offset in range(start_offset_days, max_offset_days + 1):
        resolved_date = date.today() - timedelta(days=offset)
        try:
            grid = fetch_live_grid(width, height, base_url, dataset, variable, lat0, lat1, lon0, lon1,
                                    timeout_s, time_selector=resolved_date.isoformat())
            return grid, resolved_date, offset
        except Exception as e:  # noqa: BLE001 - narrowed to "was this specifically a 404" immediately below
            status = getattr(getattr(e, "response", None), "status_code", None)
            if _HAVE_REQUESTS and isinstance(e, requests.exceptions.HTTPError) and status == 404:
                logging.debug("[%s] %s not published yet (404) -- trying %d day(s) back next",
                              dataset, resolved_date.isoformat(), offset + 1)
                last_exc = e
                continue
            raise  # a non-404 failure is a different problem; don't mask it by trying more days
    raise last_exc or RuntimeError(
        f"no published {dataset} granule found {start_offset_days}-{max_offset_days} day(s) back")


@dataclass
class OverpassSource:
    """Where one overpass's real SST tile comes from -- its own dataset ID
    and its own time selector, so four overpasses built from this are four
    independent ERDDAP queries, not one fetch reused four times."""
    dataset: str
    time_selector: str  # an explicit "YYYY-MM-DD", relative to a confirmed-published anchor day


def _resolve_overpass_sources(args: argparse.Namespace, num_overpasses: int,
                               anchor_date: date) -> list[OverpassSource]:
    """Builds one OverpassSource per overpass slot from --erddap-datasets
    (cycled if shorter than num_overpasses; defaults to --erddap-dataset
    repeated) and --erddap-day-offsets (cycled the same way; each offset N
    means "N real calendar days before anchor_date", giving a genuinely
    different real day -- and therefore a genuinely different real SST
    field -- per overpass by default).

    anchor_date must be a day _find_latest_published_grid() has already
    CONFIRMED is published (see live_scene()) -- offsets are relative to
    that verified day, not to date.today(). Anchoring to an unverified
    wall-clock date is exactly what let a 404 through before this fix:
    JPL's processing lag means "today" (offset 0) is frequently not
    published yet."""
    datasets = [d.strip() for d in args.erddap_datasets.split(",") if d.strip()] or [args.erddap_dataset]
    offsets = [int(o.strip()) for o in args.erddap_day_offsets.split(",") if o.strip() != ""] or [0]

    sources = []
    for k in range(num_overpasses):
        ds = datasets[k % len(datasets)]
        offset = offsets[k % len(offsets)]
        sources.append(OverpassSource(ds, (anchor_date - timedelta(days=offset)).isoformat()))
    return sources


def live_scene(width: int, height: int, num_overpasses: int, t: float,
                rng: np.random.Generator, args: argparse.Namespace) -> list[OverpassArrays]:
    """Builds a scene from FOUR (or --overpasses-many) INDEPENDENT real
    ERDDAP fetches -- see _resolve_overpass_sources -- each overlaid with
    its own synthetic per-overpass swath-VZA + cloud-gap pattern (see
    module docstring for why the overlay is necessary on top of a gap-
    free L4 analysis product). If ANY of the independent fetches raises,
    this function raises too (no partial/mixed scene is ever built or
    returned) -- run_worker()'s existing try/except is what turns that
    into a full fallback to synth_scene() for the whole cycle."""
    # Anchor-day discovery FIRST: which real day is actually published is
    # not assumed, it's confirmed (see _find_latest_published_grid) using
    # the first configured dataset (every slot's relative offset in
    # _resolve_overpass_sources is then computed from this verified day,
    # on the assumption every configured dataset shares roughly the same
    # publication cadence -- reasonable for slots that are all MUR SST by
    # default, less so if --erddap-datasets points at wildly different
    # products).
    anchor_dataset = ([d.strip() for d in args.erddap_datasets.split(",") if d.strip()] or [args.erddap_dataset])[0]
    anchor_grid, anchor_date, anchor_offset = _find_latest_published_grid(
        width, height, args.erddap_base, anchor_dataset, args.erddap_variable,
        args.lat0, args.lat1, args.lon0, args.lon1, args.fetch_timeout,
        start_offset_days=args.erddap_lookback_start_days, max_offset_days=args.erddap_lookback_max_days)
    logging.debug("anchor day for this scene: %s (%d day(s) back, dataset=%s)",
                  anchor_date.isoformat(), anchor_offset, anchor_dataset)

    sources = _resolve_overpass_sources(args, num_overpasses, anchor_date)
    xs, ys = np.meshgrid(np.arange(width, dtype=np.float64), np.arange(height, dtype=np.float64))
    forced_blobs = _maybe_persistent_storm(width, height, rng,
                                            getattr(args, "persistent_storm_probability", 0.0))

    # Fetch every remaining overpass's own tile, so a failure on fetch #3
    # (say) never leaves overpasses 0-2 partially built into the outgoing
    # scene -- either all N real tiles are in hand, or none of them are
    # used at all. Whichever slot resolved to the exact anchor day+dataset
    # reuses the grid the discovery search above already fetched, instead
    # of querying ERDDAP for the identical tile a second time.
    tiles: list[np.ndarray] = []
    for src in sources:
        if src.dataset == anchor_dataset and src.time_selector == anchor_date.isoformat():
            tiles.append(anchor_grid)
            continue
        tile = fetch_live_grid(width, height, args.erddap_base, src.dataset, args.erddap_variable,
                                args.lat0, args.lat1, args.lon0, args.lon1,
                                args.fetch_timeout, time_selector=src.time_selector)
        tiles.append(tile)

    overpasses = []
    for k in range(num_overpasses):
        base_sst = tiles[k]
        vza = _swath_vza(xs, width, k, num_overpasses, t)
        sec_theta = 1.0 / np.cos(np.deg2rad(vza))
        atm_bias = -0.35 * (sec_theta - 1.0)
        sst = base_sst + atm_bias

        # This overpass's own real tile's NaNs (land / ice / no MUR
        # retrieval on ITS day) are genuine no-data, exactly like a cloud
        # gap downstream.
        valid = np.isfinite(base_sst).astype(np.uint8)

        _apply_cloud_overlay(sst, valid, xs, ys, width, height, rng, forced_blobs=forced_blobs)

        sst = np.nan_to_num(sst, nan=288.0, posinf=310.0, neginf=270.0).astype(np.float32)
        avg_vza = float(vza[valid == 1].mean()) if (valid == 1).any() else float(vza.mean())
        overpasses.append(OverpassArrays(NAMES[k % len(NAMES)], avg_vza, sst, vza, valid))
    return overpasses


# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------
def run_worker(args: argparse.Namespace) -> None:
    ctx = zmq.Context.instance()
    sock = ctx.socket(zmq.PUSH)
    # Bounded queue + bounded send timeout is what actually prevents
    # unbounded memory growth if l3s_engine is slow, not yet started, or
    # down: past SNDHWM, send() blocks only up to SNDTIMEO, then raises
    # zmq.Again, which send_scene() turns into a clean drop-and-log
    # instead of an unbounded backlog or a hang.
    sock.setsockopt(zmq.SNDHWM, 4)
    sock.setsockopt(zmq.SNDTIMEO, 3000)  # ms
    sock.setsockopt(zmq.LINGER, 0)
    sock.connect(args.engine_endpoint)

    rng = np.random.default_rng(args.seed)
    scene_id = 0
    t = 0.0
    consecutive_live_failures = 0

    logging.info("ingestion worker: source=%s -> %s  (grid %dx%d, %d overpasses/scene, every %.1fs)",
                 args.source, args.engine_endpoint, args.width, args.height, args.overpasses, args.interval)
    if args.source == "synthetic":
        logging.info("network fetch disabled (--source synthetic) -- fully offline, demo-safe mode")

    while True:
        iter_start = time.time()
        try:
            overpasses = None
            data_source = DATA_SOURCE_SYNTHETIC

            if args.source == "auto":
                try:
                    overpasses = live_scene(args.width, args.height, args.overpasses, t, rng, args)
                    data_source = DATA_SOURCE_LIVE
                    if consecutive_live_failures > 0:
                        logging.info("live fetch recovered after %d failed attempt(s)", consecutive_live_failures)
                    consecutive_live_failures = 0
                except Exception as e:  # noqa: BLE001 - deliberately broad: ANY live-path failure falls back
                    consecutive_live_failures += 1
                    logging.warning("live fetch failed (%s: %s) -- sending synthetic data this cycle "
                                     "[%d consecutive failure(s)]",
                                     type(e).__name__, e, consecutive_live_failures)

            if overpasses is None:
                overpasses = synth_scene(args.width, args.height, args.overpasses, t, rng,
                                          persistent_storm_probability=args.persistent_storm_probability)
                data_source = DATA_SOURCE_SYNTHETIC

            sent = send_scene(sock, args.width, args.height, overpasses, data_source, scene_id)
            if sent:
                logging.info("scene %d sent (%s)", scene_id,
                             "LIVE" if data_source == DATA_SOURCE_LIVE else "synthetic")
            scene_id += 1
            t += 1.0

        except KeyboardInterrupt:
            raise
        except Exception as e:  # noqa: BLE001 - last-resort: one bad cycle must never kill the worker
            logging.error("unexpected error in worker loop (%s) -- continuing", e, exc_info=True)

        elapsed = time.time() - iter_start
        time.sleep(max(0.1, args.interval - elapsed))


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="L3S-LEO ingestion worker: live-or-synthetic scene source for l3s_engine --source live")
    p.add_argument("--engine-endpoint", default="tcp://localhost:5557",
                    help="l3s_engine's --ingest-endpoint to CONNECT to as PUSH (default: tcp://localhost:5557)")
    p.add_argument("--width", type=int, default=480, help="must match l3s_engine's --width")
    p.add_argument("--height", type=int, default=320, help="must match l3s_engine's --height")
    p.add_argument("--overpasses", type=int, default=4, help="must match l3s_engine's --overpasses")
    p.add_argument("--interval", type=float, default=20.0,
                    help="seconds between scenes (default: 20 -- real granules don't arrive every frame; "
                         "the engine keeps reprocessing the latest cached scene in between, see main.cpp)")
    p.add_argument("--source", choices=["auto", "synthetic"], default="auto",
                    help="'auto' (default) tries a live fetch each cycle and falls back to synthetic on any "
                         "failure; 'synthetic' never touches the network (guaranteed-safe demo mode)")
    p.add_argument("--seed", type=int, default=42, help="numpy RNG seed (synthetic fields + cloud overlay)")
    p.add_argument("--persistent-storm-probability", type=float, default=0.0,
                    help="probability [0,1] per scene of forcing one large cloud blob to identical "
                         "coordinates across ALL overpasses (default: 0.0, i.e. off -- independent random "
                         "blobs, verified 0.00%% all-overpasses-clouded at defaults). Set >0 (e.g. 0.5) to "
                         "reliably exercise the engine's all-invalid-pixel fallback for testing.")

    live = p.add_argument_group("live fetch (NOAA CoastWatch ERDDAP griddap)")
    live.add_argument("--erddap-base", default="https://coastwatch.pfeg.noaa.gov/erddap/griddap")
    live.add_argument("--erddap-dataset", default="jplMURSST41",
                       help="GHRSST L4 MUR SST analysis; default/fallback dataset ID (see --erddap-datasets)")
    live.add_argument("--erddap-datasets", default="",
                       help="comma-separated dataset ID(s), one per overpass slot, cycled if shorter than "
                            "--overpasses (default: empty -> repeats --erddap-dataset for every slot). "
                            "Point different slots at different real instrument products here if you have access.")
    live.add_argument("--erddap-day-offsets", default="0,1,2,3",
                       help="comma-separated integers, one per overpass slot, cycled if shorter than "
                            "--overpasses. Each N means 'N real calendar days before the confirmed-published "
                            "anchor day' (see --erddap-lookback-start-days/--erddap-lookback-max-days), giving "
                            "each overpass a genuinely different real day's data (verified: MUR SST at a fixed "
                            "point measurably differs day to day) instead of one fetch reused for all slots.")
    live.add_argument("--erddap-lookback-start-days", type=int, default=2,
                       help="first offset (days before today) to try when discovering the most recent PUBLISHED "
                            "day -- 2 clears typical JPL processing time (default: 2)")
    live.add_argument("--erddap-lookback-max-days", type=int, default=7,
                       help="last offset to try before giving up on the live fetch for this cycle and falling "
                            "back to synthetic (default: 7)")
    live.add_argument("--erddap-variable", default="analysed_sst")
    live.add_argument("--lat0", type=float, default=-38.0)
    live.add_argument("--lat1", type=float, default=-34.5)
    live.add_argument("--lon0", type=float, default=18.5)
    live.add_argument("--lon1", type=float, default=23.5)
    live.add_argument("--fetch-timeout", type=float, default=20.0,
                       help="seconds, PER independent fetch (up to --overpasses+1 fetches happen per cycle, "
                            "including the anchor-day discovery search)")

    p.add_argument("-v", "--verbose", action="store_true")
    return p


def main() -> None:
    args = build_arg_parser().parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                         format="%(asctime)s [ingestion] %(levelname)s %(message)s",
                         datefmt="%H:%M:%S")
    try:
        run_worker(args)
    except KeyboardInterrupt:
        logging.info("shutting down.")


if __name__ == "__main__":
    sys.exit(main())
