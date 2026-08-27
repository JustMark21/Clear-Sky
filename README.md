# L3S-LEO SciML Data Prep Engine

A distributed pipeline that reimplements the core math from
*"Algorithmic Improvements and Consistency Checks of the NOAA Global
Gridded Super-Collated SSTs from Low Earth Orbiting Satellites (L3S-LEO)"*
(Jonasson, Gladkova, Ignatov & Kihai, 2021, Proc. SPIE 11752, 1175202)
as a high-performance C++ backend, fed by either a local synthetic
simulator or a live/hybrid Python ingestion worker, streaming to a live
Python telemetry dashboard over ZeroMQ.

```
09respaper/
├── CMakeLists.txt              C++17 build (needs libzmq3-dev + cppzmq-dev)
├── include/l3s/
│   ├── SummedAreaTable.hpp      O(1) box filter (integral image)
│   ├── Grid.hpp                 row-major float tensor + validity mask
│   ├── Simulator.hpp            synthetic multi-overpass scene generator
│   ├── Engine.hpp               LCR, Eq.(1), iterative debiasing (untouched by ingestion)
│   ├── Telemetry.hpp            OUTBOUND binary protocol + ZMQ PUB publisher (engine -> dashboard)
│   └── Ingestion.hpp            INBOUND binary protocol + ZMQ PULL receiver (worker -> engine)
├── src/main.cpp                 engine entrypoint / streaming loop / --source sim|live
└── python/
    ├── subscriber.py            the "Infrastructure Control Room" dashboard
    ├── ingestion_worker.py      live-or-synthetic scene source for --source live
    ├── requirements.txt         dashboard deps (pyzmq, matplotlib, numpy)
    └── requirements-ingestion.txt  + requests, only needed for the worker's live fetch
```

## What the C++ engine actually computes

Straight out of Section 2 of the paper:

1. **Local Clear-sky Ratio (LCR)** — mean, in an 11×11 sliding window, of a
   binary clear(1)/cloudy(0) mask. Computed via a summed-area table, so
   every pixel's window mean is an O(1) lookup instead of an O(N²) scan.
2. **Eq. (1) weight** — `w_i ∝ exp(-S_i / S0) * LCR_i²`, with
   `S_i = sec(VZA_i) - 1` and `S0 = 1.33` (the paper's ACSPO V2.80 value).
   Used to build the **LVW** (LCR-VZA-weighted) initial L3S reference, and
   compared directly against the older **LVZA** (lowest-VZA composite)
   reference it replaced.
3. **Iterative debiasing** — 3 passes with shrinking windows (21×21 →
   11×11 → 7×7): each overpass is normalized against the current L3S
   reference by subtracting the local window mean of `(overpass -
   reference)`, then overpasses are recombined (LCR-VZA weighted) into the
   next reference, converging on the final L3S SST.

The synthetic **Simulator** stands in for real ACSPO L3U granules: it
generates several overpasses over a shared macro-scale "true" SST field
carrying a strong thermal front, each seen through a swath-geometry VZA
field (bias grows with VZA), with large organic "storm" cloud systems
that are *mostly* shared but slightly jittered per overpass — producing
both genuine multi-overpass data gaps and the large-scale cloud-leakage
rings that motivated the paper's Eq. (1) fix in the first place.

## Wire protocol

ZMQ PUB, topic `"L3S"`, wire format v2 (magic `"L3S3"`) — a variable-length
multipart message per scene, `4 + numOverpasses` frames:
`[topic][64-byte header][raw_0]...[raw_{N-1}][LVZA float32 grid][clean float32 grid]`,
one raw grid per individual overpass (not just `overpasses[0]`). The
header's `numOverpasses` field tells the dashboard how many `raw_i` frames
to expect, and its `sst_min_k`/`sst_max_k` are computed over the union of
*every* grid in the message — every panel in the dashboard shares one
locked color scale. See the header comment in `include/l3s/Telemetry.hpp`
for the exact byte layout — `python/subscriber.py`'s `decode_frame()`
unpacks it with `struct.unpack("<6I4d2f", ...)`.

## Hybrid live-data ingestion (`--source live`)

By default (`--source sim`, unchanged) the engine generates every scene
itself via `Simulator.hpp`. `--source live` instead feeds the engine from
an external Python worker over a **second, independent** ZMQ channel —
the existing engine → dashboard PUB/SUB telemetry channel is completely
untouched:

```
[ingestion_worker.py] --PUSH/PULL(tcp://*:5557)--> [l3s_engine --source live] --PUB/SUB(tcp://*:5556)--> [dashboard]
        |                                                    ^
        +-- live NOAA/GHRSST ERDDAP fetch,                   |
            or synthetic fallback on ANY failure              +-- Telemetry.hpp, byte-for-byte unchanged
```

**Why PUSH/PULL, not PUB/SUB, for the inbound side:** the outbound
telemetry feed carries disposable, latest-wins frames (dropping a stale
one is correct). The inbound feed carries data-of-record — silently
dropping a scene is the wrong default — so it uses PUSH/PULL with a
bounded queue on both ends (`SNDHWM`/`RCVHWM` = 4) instead: past that
bound, the worker's `send()` times out (`SNDTIMEO`) and drops-with-a-log
rather than blocking forever or growing memory unboundedly. The engine
**binds** the PULL socket (it's the long-lived, stable process, symmetric
with how it already binds its outbound PUB socket); the worker
**connects** as PUSH (the more disposable, restart-anytime side).

**Wire protocol** (`include/l3s/Ingestion.hpp`): one multipart message per
scene, `3 + 3*numOverpasses` frames — a 32-byte `SceneHeader`
(magic/width/height/numOverpasses/dataSource/sceneId/timestamp), a
12-byte-per-overpass metadata block (name + avgVZA), then per overpass a
float32 SST grid, float32 VZA grid, and uint8 validity mask. Every frame's
byte length is checked against the header **before** any `memcpy` — a
wrong dtype, truncated frame, or grid-size mismatch is rejected and
logged, never silently reinterpreted. `python/ingestion_worker.py` packs
the identical layout with `struct.pack("<6Id", ...)` / `struct.pack("<8sf",
...)`; a standalone round-trip check of both is in the worker's own
comments if you need to verify a change to either side.

**Fallback behavior is layered, not a single point of failure:**
1. The worker's own `--source auto` mode tries a live ERDDAP fetch every
   cycle and falls back to a synthetic scene on *any* exception (network,
   timeout, HTTP error, malformed response, all-NaN tile) — logged, never
   raised. `--source synthetic` skips the network entirely for a
   guaranteed-offline demo.
2. The engine itself never trusts the worker to be running at all: if no
   scene ever arrives within `--initial-grace-sec`, or the feed goes
   silent for longer than `--stale-ceiling-sec`, the engine falls back to
   its own local `Simulator` — so `--source live` can never hang or crash
   the pipeline even with zero Python processes running.
3. Between arrivals (real granules don't show up every 60ms), the engine
   caches and keeps reprocessing the most recent scene rather than either
   stalling or fabricating a new one each tick — mirroring how NRT ACSPO
   itself keeps serving its current best product between granule arrivals
   (paper: "typical latency of 3-6 hours").

**Live data source used by the worker's `--source auto` path:** NOAA/JPL
`jplMURSST41` (the GHRSST Level-4 MUR SST analysis — one of the exact
foundation-SST products, alongside CMC/OSTIA/RAMSSA/Reynolds, that the
paper itself lists as ACSPO's first-guess reference) via the public NOAA
CoastWatch ERDDAP (`coastwatch.pfeg.noaa.gov`), queried with a plain
`requests` call — confirmed reachable and returning real, live-dated
Agulhas Current temperatures at build time.

**Each overpass is its own independent fetch, not one tile reused.** By
default all `--overpasses` slots share the same AOI and dataset but each
pulls a genuinely different real calendar day (`today`, `today-1`,
`today-2`, `today-3`, via `--erddap-day-offsets 0,1,2,3`) — a faithful
analog of the paper's own A1/B1/A2/C1 scenario (near-identical geography,
different acquisition times), and independently verified: at one test
point, MUR's real SST measurably differed across 4 consecutive real days
(27.81 → 27.78 → 27.69 → 27.25 °C), and four full-tile fetches showed
0.67–1.66 K mean pairwise differences — genuinely distinct data, not
clones. (Note: ERDDAP's `last-N` relative-time syntax was tested first
and did **not** step backwards as its name implies — all of `last`,
`last-1`, `last-2`, `last-3` resolved to the same timestamp — so the
worker uses explicit ISO dates instead, which were verified to work.)
`--erddap-datasets ds1,ds2,ds3,ds4` (cycled if shorter than
`--overpasses`) additionally lets you point individual slots at different
real dataset IDs, if you have access to distinct per-instrument products.
Fetches happen sequentially and all-or-nothing per cycle: every overpass's
tile is fetched before any `OverpassArrays` are built, so one failed slot
among four raises and falls back the *whole* scene to `synth_scene()` —
never a partial real/fake mix.

**Caveat:** MUR is a gap-free L4 analysis, so it carries no native
per-pixel VZA or cloud mask; the worker uses MUR's real observed
temperatures as the physical SST field for each overpass and overlays a
synthetic (but physically-modeled, same swath-geometry math as
`Simulator.hpp`) per-overpass VZA field and cloud mask on top — real
temperatures, synthetic viewing geometry. If you have NOAA CoastWatch/
PO.DAAC access to genuine ACSPO L3U granules (paper ref [8]), point
`--erddap-datasets` at that source instead; `OverpassArrays` downstream is
unaffected either way. External endpoints and dataset IDs can change —
that's precisely why every layer above falls back automatically rather
than assuming this one stays valid forever.

```bash
# --- C++ backend, live mode ---
./build/l3s_engine --width 800 --height 600 --overpasses 4 --fps 4 \
                    --endpoint tcp://*:5556 \
                    --source live --ingest-endpoint tcp://*:5557

# --- ingestion worker (separate terminal) ---
.venv/bin/pip install -r python/requirements-ingestion.txt   # optional: only for live fetch
.venv/bin/python python/ingestion_worker.py \
    --engine-endpoint tcp://localhost:5557 --width 800 --height 600 --overpasses 4 \
    --source auto --lat0 23.0 --lat1 29.0 --lon0 -84.0 --lon1 -76.0

# --- dashboard, exactly as before, doesn't know or care which mode the engine is in ---
.venv/bin/python python/subscriber.py --endpoint tcp://localhost:5556
```

`--width`/`--height`/`--overpasses` must match between the engine and the
worker — the engine validates the incoming `SceneHeader`'s grid size and
rejects (falls back) on a mismatch rather than trying to reconcile it.

### CLI flags (`l3s_engine`, live-mode additions)

| flag | default | meaning |
|---|---|---|
| `--source` | `sim` | `sim` (unchanged) or `live` (pull from ingestion worker) |
| `--ingest-endpoint` | `tcp://*:5557` | PULL bind address for the ingestion worker |
| `--ingest-poll-ms` | 60 | per-tick poll budget for a new scene |
| `--initial-grace-sec` | 8 | wait this long for a first-ever scene before falling back |
| `--stale-ceiling-sec` | 300 | reprocess the cached scene up to this long before falling back |

### CLI flags (`ingestion_worker.py`)

| flag | default | meaning |
|---|---|---|
| `--engine-endpoint` | `tcp://localhost:5557` | PUSH-connects to the engine's `--ingest-endpoint` |
| `--width`, `--height`, `--overpasses` | 480, 320, 4 | must match `l3s_engine` |
| `--interval` | 20.0 s | time between scenes |
| `--source` | `auto` | `auto` (try live, auto-fallback) or `synthetic` (offline-only) |
| `--erddap-base`/`--erddap-dataset`/`--erddap-variable`/`--lat0..lon1` | see script | live-fetch AOI (default: Agulhas Current, matching paper Fig. 8/9) |
| `--erddap-datasets` | *(empty → repeats `--erddap-dataset`)* | comma-separated dataset ID per overpass slot, cycled |
| `--erddap-day-offsets` | `0,1,2,3` | comma-separated "days before today" per overpass slot, cycled — the default gives each overpass its own real calendar day |
| `--fetch-timeout` | 20.0 s | timeout **per independent fetch** (up to `--overpasses` fetches/cycle) |

## One-command demo (`run_demo.sh`)

For a live presentation, `./run_demo.sh` builds nothing but orchestrates
everything: it starts the engine (`--source live`), waits for its two ZMQ
ports to actually bind (polling `ss`, not a blind sleep), starts the
ingestion worker pointed at the engine, and finally launches the
dashboard in the foreground so its window opens immediately. Closing that
window or pressing Ctrl+C tears down the worker and engine cleanly (a
`trap` on `EXIT`/`INT`/`TERM`, SIGTERM first, SIGKILL after a grace period
for anything still alive) — no orphaned processes, no held ports.

```bash
./run_demo.sh                                    # Agulhas Current, live-or-fallback
./run_demo.sh --region florida --width 640 --height 480
./run_demo.sh --region florida --width 640 --height 480 --persistent-storm-probability 1.0 
./run_demo.sh --region 20.0,26.0,-158.0,-150.0    # custom bbox: lat0,lat1,lon0,lon1
./run_demo.sh --offline                           # zero network calls, guaranteed-safe rehearsal
./run_demo.sh --help
```

Region presets: `agulhas` (default, paper Fig. 8/9), `florida`, `gulfstream`,
`monterey` (paper Fig. 6/7). Requires the engine already built and the
`.venv` already created (see below) — it checks for both and fails fast
with the exact command to run if either is missing.

## Build & run

```bash
# --- C++ backend ---
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/l3s_engine --width 480 --height 320 --overpasses 4 --fps 4 \
                    --endpoint tcp://*:5556

# --- Python dashboard (separate terminal) ---
python3 -m venv .venv && .venv/bin/pip install -r python/requirements.txt
.venv/bin/python python/subscriber.py --endpoint tcp://localhost:5556
```

`l3s_engine` runs standalone — it has no Python dependency and will
happily publish telemetry with zero subscribers connected (the PUB
socket just drops frames past its small high-water mark rather than
blocking). Multiple dashboards can subscribe to the same engine
simultaneously.

### CLI flags (`l3s_engine`)

| flag | default | meaning |
|---|---|---|
| `--width`, `--height` | 480, 320 | grid size |
| `--overpasses` | 4 | simulated satellite passes per scene |
| `--fps` | 4.0 | target scenes/sec published |
| `--endpoint` | `tcp://*:5556` | ZMQ PUB bind address |
| `--seed` | 42 | RNG seed for the synthetic scene |

## Dashboard layout

Maps-first, nested-`GridSpec` layout — the visual pipeline dominates,
engine telemetry is a compact strip underneath:

- **Top block (~80% of height)**, itself a strict 2-row × 4-column grid:
  - **Columns 0–1, both rows** — a 2×2 block of up to 4 individual raw
    overpasses (`RAW PASS 1..4`), each fragmented with its own independent
    cloud-gap pattern.
  - **Column 2, spanning both rows** — the legacy LVZA composite (jagged
    stitching/leakage).
  - **Column 3, spanning both rows** — the final SciML-ready fused L3S SST
    after LCR-VZA weighting and 3 debiasing iterations (smooth).

  Every panel shares one **locked** `vmin`/`vmax` (computed by the engine
  over the union of *every* grid it sends), so the fragmented → jagged →
  smooth progression is an honest visual comparison, not an artifact of
  per-panel auto-scaling. The 2×2 block has exactly 4 fixed slots; if the
  engine is run with `--overpasses` ≠ 4, the dashboard shows
  `min(numOverpasses, 4)` slots, leaves unused slots visibly blank, and
  logs a one-time warning if any overpasses are hidden (> 4).
- **Bottom row (~20% of height), 2 columns** — compact throughput (MB/s)
  and processing latency (ms) strip charts with bright current-value
  readouts.

The canvas is only redrawn (`flush_events()`) once a whole frame — every
heatmap, both strip charts, and the status line — has been fully updated,
so the UI never shows a half-updated frame and never blocks. (This loop
is a plain `zmq.poll()` / `recv_multipart()` / `draw_idle()` cycle, not
`matplotlib.animation.FuncAnimation` — it was never `FuncAnimation`-based;
the manual loop already serves that role.)
