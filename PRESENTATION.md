# L3S-LEO SciML Data Prep Engine — 5-Minute Technical Pitch

**Narrative arc:** this is not a visualization demo — it's infrastructure. A
production-shaped, three-process distributed system that turns raw,
multi-sensor, cloud-corrupted satellite telemetry into a bias-corrected,
locked-scale float32 tensor a SciML model can consume directly, with the
same failure-mode resilience a real ingestion pipeline needs and none of
the manual-QC bottleneck that normally sits in front of it.

Target: ~5:00 total, ~50s/slide. Timings below are a guide, not a script
you have to hit exactly.

---

## Slide 1 — "SST Data Doesn't Arrive Clean"

**Visual concept:** Dashboard's top-left 2×2 block only — the four `RAW
PASS` panels, full-size, dashboard paused/frozen on a frame with heavy
cloud coverage (lots of solid-black gaps, each panel's gap pattern
visibly different from the others). No fused panel visible yet — just
the raw mess. If live, this is the first thing on screen when
`run_demo.sh` finishes booting.

**Bullet points:**
- Source: NOAA ACSPO L3S-LEO (Jonasson, Gladkova, Ignatov & Kihai, 2021, *SPIE* 11752)
- 4 asynchronous overpasses/scene — different sensors, different times, different view geometry
- Every raw pass: independent cloud gaps + view-angle-dependent thermal bias
- This is what hits a SciML ingestion pipeline *before* any cleaning

**Speaker notes:**
> "Every panel you're looking at is one satellite overpass of the *same*
> patch of ocean, minutes to hours apart. Same geography, four different
> temperature fields. Black is cloud — genuinely missing data. But the
> colored pixels aren't clean either: they're carrying a systematic
> thermal bias from the viewing angle, which I'll show you in a second.
> This is the reimplementation of a real, peer-reviewed NOAA production
> algorithm — not a toy — and it's exactly the shape of problem that
> kills naive SciML data pipelines: async, multi-sensor, partially
> corrupted, and it has to get fused into one clean tensor before a model
> ever sees it."

---

## Slide 2 — "Three Processes, Two Independent ZeroMQ Channels"

**Visual concept:** The architecture diagram from `README.md`, rendered
large:
```
[ingestion_worker.py] --PUSH/PULL(tcp://*:5557)--> [l3s_engine --source live] --PUB/SUB(tcp://*:5556)--> [dashboard]
```
Split-terminal alternative: three panes showing `l3s_engine`, `ingestion_worker.py`,
and `subscriber.py` all logging simultaneously right after `./run_demo.sh` starts.

**Bullet points:**
- C++ engine: standalone binary, **zero Python dependency**, `--source sim|live`
- Inbound = ZMQ **PUSH/PULL**, bounded queue (`SNDHWM`/`RCVHWM`=4) — data-of-record, never silently dropped
- Outbound = ZMQ **PUB/SUB** — disposable telemetry, drops past HWM by design
- Engine **binds** both sockets; worker **connects** — stable core, disposable edge
- Wire protocols are versioned + byte-length-validated before every `memcpy`

**Speaker notes:**
> "This is three separate OS processes talking over two *different* ZeroMQ
> patterns, on purpose. The inbound channel — worker pushing scenes into
> the engine — is PUSH/PULL with a bounded queue, because that data is
> data-of-record: if the engine falls behind, we backpressure and log a
> drop, we never silently lose a granule. The *outbound* channel — engine
> to dashboard — is PUB/SUB, because telemetry is disposable: a stale
> frame should get dropped, not queued. That distinction is the kind of
> thing you'd expect from a real ingestion service, not a hackathon demo.
> And every frame on both channels is length-checked against its header
> before we ever touch it with a raw memcpy — a malformed payload gets
> rejected and logged, never reinterpreted."

---

## Slide 3 — "The Physics: Why You Can't Just Average These"

**Visual concept:** Eq. (1) rendered large and centered:
```
w_i ∝ exp(−S_i / S₀) · LCR_i²        S_i = sec(VZA_i) − 1        S₀ = 1.33
```
Underneath it, side-by-side: one RAW PASS panel with a visibly cold/blue
swath edge, next to its VZA field if you have it, or just point at the
panel directly.

**Bullet points:**
- `S_i = sec(VZA_i) − 1` — atmospheric slant-path length, grows with view angle
- Longer path ⇒ more atmospheric attenuation ⇒ systematically **colder** retrieved SST
- `LCR` — Local Clear-sky Ratio, 11×11 window mean of the clear/cloud mask, `∈[0,1]`
- Same physical pixel, 4 overpasses ⇒ 4 different `VZA` ⇒ 4 different biases

**Speaker notes:**
> "Here's the actual problem. At pixel [100,100], all four overpasses see
> the *same* patch of ocean, but each one's own swath geometry puts that
> pixel at a different view zenith angle. `sec(theta) minus one` is the
> slant-path term — it's small near nadir and blows up toward the swath
> edge, and a longer atmospheric path means more attenuation, which
> means the retrieved temperature comes back colder than truth. That's
> the blue edge you see on these panels — it's not noise, it's a
> systematic, angle-dependent bias. `LCR` handles the other failure
mode — cloud leakage — pixels that pass the cloud mask but sit right
> next to a cloud boundary, still contaminated. Eq. 1, straight from the
> paper, combines both into one weight per pixel per overpass."

---

## Slide 4 — "Continuous Weighting, Not Hard Selection"

**Visual concept:** Dashboard's right two panels only — `LEGACY FUSION //
LVZA Composite` next to `FUSED L3S SST // SciML-Ready Tensor` — same
frame, same locked color scale, side by side, so the jagged-stitching
vs. smooth contrast is visible in one glance. Optional: a code snippet of
`Engine.hpp`'s `computeWeights()` on screen behind you.

**Bullet points:**
- `SummedAreaTable`: O(1) box-filter query for **any** window size — 21×21 down to 7×7, same cost
- `LVZA` (legacy): hard `argmin(VZA)` pick per pixel → visible swath-edge stitching
- `L3S` (ours): `Σ(w_i · SST_i) / Σ w_i` — smooth, continuous blend, weighted toward nadir
- 3 debiasing iterations, shrinking windows 21×21 → 11×11 → 7×7, converging to final tensor

**Speaker notes:**
> "The legacy approach — LVZA — just picks whichever overpass had the
> lowest view angle at each pixel. Cheap, but you can see the seams
> where the source overpass changes underneath it. Our engine instead
> computes a *continuous* weighted average — every valid overpass
> contributes, weighted by Eq. 1, so the near-nadir view dominates
> without discarding the others outright. All of that per-pixel weight
> math, plus the local clear-sky ratio, runs through a summed-area table,
> so a window query costs the same O(1) whether it's 21-by-21 or 7-by-7
> — that's what makes three full debiasing iterations, at every pixel,
> tractable in milliseconds instead of seconds. Look at the difference:
> same underlying data, same color scale, and the fused tensor on the
> right is what actually goes downstream to a model."

---

## Slide 5 — "This Is Infrastructure, Not a Demo Script"

**Visual concept:** Terminal, live: kill the network (or point at a
`--source live` run) and let the log line print in real time:
`[l3s_engine] ingestion worker reports its own upstream fetch failed -- it is sending synthetic data`
— then keep talking while frames keep streaming uninterrupted.

**Bullet points:**
- Worker: `--source auto` — live ERDDAP fetch, ANY exception → synthetic fallback, logged not raised
- Engine: never trusts the worker — `--initial-grace-sec` / `--stale-ceiling-sec` → local `Simulator` fallback
- Between arrivals: engine **caches and reprocesses**, never stalls, never fabricates on every tick
- Result: `--source live` cannot hang or crash the pipeline, even with zero Python processes running

**Speaker notes:**
> "Watch this log line — the live NOAA fetch just failed, and the system
> didn't stop. The worker catches *any* exception from the network call
> and degrades to a synthetic scene automatically. But the engine doesn't
> even trust the worker to exist: if no scene ever arrives, or the feed
> goes stale, the engine falls back to its own local simulator — so this
> pipeline cannot hang or crash from a network problem, full stop. And
> between real granule arrivals — which in production terms means every
> few minutes, not every frame — the engine just keeps reprocessing the
> most recent real scene instead of stalling the dashboard or making
> something up. That's three independent fallback layers. That's the
> difference between a demo and something you'd actually trust to run
> unattended."

---

## Slide 6 — "One Command, Production-Shaped, SciML-Ready"

**Visual concept:** Full dashboard, live, all six panels populated, throughput
and latency strip charts actively scrolling with real numbers. Terminal
underneath showing the single command that launched everything:
`./run_demo.sh --region florida --width 800 --height 600`

**Bullet points:**
- `run_demo.sh`: 1 command → engine + worker + dashboard, port-bind-polled (not blind-sleep), trap-based teardown
- Output: bias-corrected, gap-minimized, **locked-scale** float32 tensor — not a picture, a tensor
- Live-measured throughput/latency on screen — not a slide claim
- This is the layer that should sit **in front of** every SciML model trained on this data, not after it

**Speaker notes:**
> "One command boots all three processes, waits for the ZeroMQ sockets to
> actually bind — not a guessed sleep — and tears everything down
> cleanly on Ctrl-C, no orphaned processes, no held ports. What you're
> watching update live is the real throughput and latency this machine
> is achieving right now, not a number I put in a slide. And the actual
> deliverable isn't this dashboard — it's that fused tensor, streaming
> out as raw float32, already bias-corrected, already gap-minimized,
> already on a locked scale. That's the piece that's supposed to sit
> *upstream* of a SciML model, not get bolted on as a cleanup step after
> the fact. Happy to open it up — let's run it live."

---

## Appendix — quick-reference numbers, if asked

- **Eq. (1):** `w_i ∝ exp(−S_i/S₀)·LCR_i²`, `S_i=sec(VZA_i)−1`, `S₀=1.33` (ACSPO V2.80 value)
- **LCR window:** 11×11; **debiasing windows:** 21×21 → 11×11 → 7×7 (3 iterations)
- **Outbound wire (Telemetry.hpp):** magic `"L3S3"`, 64-byte header, `4+numOverpasses` frames
- **Inbound wire (Ingestion.hpp):** magic `"L3S2"`, 32-byte `SceneHeader`, `3+3×numOverpasses` frames
- **Live source:** `jplMURSST41` (GHRSST L4 MUR SST) via NOAA CoastWatch ERDDAP — same product class (CMC/OSTIA/MUR/Reynolds) the paper itself uses as ACSPO's first-guess reference
- **4 overpasses = 4 independent real fetches**, verified 0.67–1.66 K mean pairwise difference (not clones)
