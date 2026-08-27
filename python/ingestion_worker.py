import argparse
import csv
import io
import struct
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone

import numpy as np
import requests
import zmq

SCENE_MAGIC = 0x3253334C  # "L3S2"
SCENE_VERSION = 1
# 6x uint32 + 1x double, little-endian -- matches Ingestion.hpp's
# SceneHeader byte-for-byte on an x86_64 host.
SCENE_HEADER_STRUCT = struct.Struct("<6Id")
# 1x uint32 + 2x float -- matches Ingestion.hpp's OverpassMeta.
OVERPASS_META_STRUCT = struct.Struct("<Iff")

ERDDAP_URL = "https://coastwatch.pfeg.noaa.gov/erddap/griddap/jplMURSST41.csv"


@dataclass
class OverpassSource:
    source_id: int
    time_selector: str  # interior of ERDDAP's time constraint, e.g. "last" or an ISO8601 timestamp
    day_offset: int


def _resolve_latest_available_date(lat0: float, lon0: float, timeout_s: float = 10.0) -> datetime:
    """Probes a single pixel at ERDDAP's own "last" time selector and
    reads back the actual timestamp it resolved to. jplMURSST41 is a
    near-real-time product that typically lags a day or more behind wall
    clock, so assuming "today" has been published is not safe -- ask
    ERDDAP what its actual latest day is instead of guessing."""
    query = f"analysed_sst%5B(last)%5D%5B({lat0}):({lat0 + 0.02})%5D%5B({lon0}):({lon0 + 0.02})%5D"
    resp = requests.get(f"{ERDDAP_URL}?{query}", timeout=timeout_s)
    resp.raise_for_status()
    rows = list(csv.reader(io.StringIO(resp.text)))
    if len(rows) < 3:
        raise RuntimeError("ERDDAP 'last' probe returned no data row")
    return datetime.strptime(rows[2][0], "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)


def _resolve_overpass_sources(num_overpasses: int, lat0: float, lon0: float) -> list:
    """One real calendar day per overpass, most recent first. jplMURSST41
    publishes a single daily analysis rather than per-swath granules, so
    distinct days stand in for distinct, independently-sourced passes --
    each fetched separately below, each with genuinely different SST and
    cloud coverage from the others."""
    try:
        base = _resolve_latest_available_date(lat0, lon0)
    except Exception as exc:  # noqa: BLE001 -- discovery failing just degrades day-anchoring, not the whole worker
        print(f"[ingestion_worker] could not resolve ERDDAP's latest published date ({exc}) -- "
              f"assuming 2 days of publication latency behind wall-clock", file=sys.stderr)
        base = datetime.now(timezone.utc).replace(hour=9, minute=0, second=0, microsecond=0) - timedelta(days=2)

    sources = []
    for k in range(num_overpasses):
        ts = base - timedelta(days=k)
        sources.append(OverpassSource(source_id=k, time_selector=ts.strftime("%Y-%m-%dT%H:%M:%SZ"), day_offset=k))
    return sources


def _resample_nearest(grid: np.ndarray, out_h: int, out_w: int) -> np.ndarray:
    in_h, in_w = grid.shape
    row_idx = np.linspace(0, in_h - 1, out_h).round().astype(int)
    col_idx = np.linspace(0, in_w - 1, out_w).round().astype(int)
    return grid[np.ix_(row_idx, col_idx)]


def fetch_live_grid(lat0: float, lat1: float, lon0: float, lon1: float, width: int, height: int,
                     time_selector: str = "last", timeout_s: float = 10.0) -> np.ndarray:
    """Fetches one jplMURSST41 (GHRSST L4 MUR SST) grid over the given
    bounding box, at the given ERDDAP time constraint, from NOAA
    CoastWatch ERDDAP, and resamples it to (height, width). Raises on any
    network, HTTP, or parsing failure -- callers are expected to catch
    and fall back."""
    query = f"analysed_sst%5B({time_selector})%5D%5B({lat0}):({lat1})%5D%5B({lon0}):({lon1})%5D"
    resp = requests.get(f"{ERDDAP_URL}?{query}", timeout=timeout_s)
    resp.raise_for_status()

    rows = list(csv.reader(io.StringIO(resp.text)))
    data_rows = rows[2:]  # ERDDAP CSV starts with a column-name row and a units row
    if not data_rows:
        raise RuntimeError("ERDDAP returned no data rows for the requested bbox")

    lats, lons, vals = [], [], []
    for row in data_rows:
        if len(row) < 4:
            continue
        _time_s, lat_s, lon_s, sst_s = row[0], row[1], row[2], row[3]
        sst_k = float("nan") if sst_s == "" else float(sst_s) + 273.15
        lats.append(float(lat_s))
        lons.append(float(lon_s))
        vals.append(sst_k)

    if not lats:
        raise RuntimeError("ERDDAP response had no parseable rows")

    uniq_lats = sorted(set(lats))
    uniq_lons = sorted(set(lons))
    lat_index = {v: i for i, v in enumerate(uniq_lats)}
    lon_index = {v: i for i, v in enumerate(uniq_lons)}

    grid = np.full((len(uniq_lats), len(uniq_lons)), np.nan, dtype=np.float32)
    for lat, lon, v in zip(lats, lons, vals):
        grid[lat_index[lat], lon_index[lon]] = v

    grid = _resample_nearest(grid, height, width)

    # uniq_lats is sorted ascending (south to north), so row 0 of `grid`
    # is already the southern edge -- exactly what origin="lower"
    # expects for row 0 to sit at the bottom of the image. No flip
    # needed: south-to-north data plus a bottom-up axis already agree.
    return grid


def synth_grid(width: int, height: int, rng: np.random.Generator) -> np.ndarray:
    """Local fallback scene: a flat Kelvin baseline with one gaussian
    warm core plus a handful of circular cloud gaps, used whenever a
    given overpass's live fetch fails for any reason."""
    baseline_k = 288.15
    amplitude_k = 6.0
    sigma_px = 0.2 * min(width, height)

    yy, xx = np.mgrid[0:height, 0:width]
    cx = rng.uniform(width * 0.3, width * 0.7)
    cy = rng.uniform(height * 0.3, height * 0.7)
    grid = baseline_k + amplitude_k * np.exp(-((xx - cx) ** 2 + (yy - cy) ** 2) / (2.0 * sigma_px ** 2))
    grid = grid.astype(np.float32)

    for _ in range(4):
        bx = rng.uniform(0, width)
        by = rng.uniform(0, height)
        br = rng.uniform(3.0, 0.08 * min(width, height) + 3.0)
        grid[(xx - bx) ** 2 + (yy - by) ** 2 <= br ** 2] = np.nan

    return grid


def _fetch_one_overpass(source: OverpassSource, lat0: float, lat1: float, lon0: float, lon1: float, width: int,
                         height: int) -> tuple:
    """Runs in a worker thread, one per overpass, so the 4 fetches are in
    flight concurrently. Each overpass's failure is independent -- one
    source timing out or erroring falls that overpass back to synthetic
    without affecting the other, successfully-fetched overpasses."""
    try:
        grid = fetch_live_grid(lat0, lat1, lon0, lon1, width, height, time_selector=source.time_selector)
        return grid, "live"
    except Exception as exc:  # noqa: BLE001 -- any failure degrades this one overpass, never the whole scene
        print(f"[ingestion_worker] overpass {source.source_id} (day_offset={source.day_offset}) "
              f"live fetch failed ({exc}) -- falling back to synthetic", file=sys.stderr)
        return synth_grid(width, height, np.random.default_rng()), "synthetic"


def run_worker(args: argparse.Namespace) -> None:
    ctx = zmq.Context()
    push = ctx.socket(zmq.PUSH)
    push.connect(args.engine_endpoint)

    with ThreadPoolExecutor(max_workers=args.overpasses) as pool:
        while True:
            sources = _resolve_overpass_sources(args.overpasses, args.lat0, args.lon0)
            futures = [
                pool.submit(_fetch_one_overpass, s, args.lat0, args.lat1, args.lon0, args.lon1, args.width,
                            args.height)
                for s in sources
            ]
            results = [f.result() for f in futures]

            overpass_grids = [grid for grid, _source in results]
            source_ids = [s.source_id for s in sources]
            live_count = sum(1 for _grid, kind in results if kind == "live")

            parts = pack_scene(args.width, args.height, overpass_grids, source_ids, time.time())
            push.send_multipart(parts)
            print(f"[ingestion_worker] sent scene ({live_count}/{args.overpasses} overpasses live, "
                  f"{args.overpasses - live_count} synthetic)")

            time.sleep(args.interval)


def pack_scene(width: int, height: int, overpass_grids: list, source_ids: list, timestamp_s: float) -> list:
    header = SCENE_HEADER_STRUCT.pack(
        SCENE_MAGIC, SCENE_VERSION, width, height, len(overpass_grids), 0, timestamp_s
    )
    parts = [header]
    for grid, source_id in zip(overpass_grids, source_ids):
        valid = ~np.isnan(grid)
        mean_sst_k = float(np.nanmean(grid)) if valid.any() else 0.0
        valid_fraction = float(valid.mean())
        parts.append(OVERPASS_META_STRUCT.pack(source_id, mean_sst_k, valid_fraction))
        parts.append(np.ascontiguousarray(grid, dtype=np.float32).tobytes())
    return parts


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="L3S-LEO ingestion worker: real or synthetic scenes over ZMQ PUSH.")
    p.add_argument("--engine-endpoint", default="tcp://localhost:5557")
    p.add_argument("--width", type=int, default=64)
    p.add_argument("--height", type=int, default=64)
    p.add_argument("--overpasses", type=int, default=4)
    p.add_argument("--interval", type=float, default=20.0, help="seconds between scenes")
    p.add_argument("--lat0", type=float, default=-37.0)
    p.add_argument("--lat1", type=float, default=-35.0)
    p.add_argument("--lon0", type=float, default=20.0)
    p.add_argument("--lon1", type=float, default=22.0)
    return p


if __name__ == "__main__":
    run_worker(build_arg_parser().parse_args())
