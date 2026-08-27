// main.cpp — L3S-LEO SciML Data Prep Engine
//
// Standalone backend service: generates synthetic multi-sensor SST scenes
// (macro-scale thermal fronts + large cloud gaps/leakage), runs the
// ACSPO L3S-LEO collation math (LCR, Eq.(1) LVW weighting, iterative
// shrinking-window debiasing -- see include/l3s/Engine.hpp for the exact
// mapping to Section 2 of the paper), and streams the result plus live
// performance telemetry over a ZeroMQ PUB socket for the Python dashboard.
#include "l3s/Grid.hpp"
#include "l3s/Simulator.hpp"
#include "l3s/Engine.hpp"
#include "l3s/Telemetry.hpp"
#include "l3s/Ingestion.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#include <algorithm>
#include <limits>
#include <optional>
#include <vector>

namespace {
std::atomic<bool> g_running{true};
void handleSignal(int) { g_running.store(false); }
}

struct CliOptions {
    int width = 480;
    int height = 320;
    int numOverpasses = 4;
    double targetFps = 4.0;
    std::string endpoint = "tcp://*:5556";
    unsigned seed = 42;

    // Data source: "sim" (default, unchanged behavior) uses the local
    // Simulator every frame, exactly as before. "live" instead pulls
    // scenes pushed in by python/ingestion_worker.py over a separate
    // PUSH/PULL channel (see Ingestion.hpp), falling back to the local
    // Simulator if no live scene has ever arrived, or if the feed goes
    // stale -- so this flag can never make the engine hang or crash,
    // only change where frames come from.
    std::string source = "sim";
    std::string ingestEndpoint = "tcp://*:5557";
    int ingestPollMs = 60;          // per-tick poll budget for a new scene
    double initialGraceSec = 8.0;   // grace period before first-ever fallback
    double staleCeilingSec = 300.0; // reprocess cached scene up to this long before falling back
};

static CliOptions parseArgs(int argc, char** argv) {
    CliOptions o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* flag) -> const char* {
            if (i + 1 < argc) return argv[++i];
            std::fprintf(stderr, "missing value for %s\n", flag);
            std::exit(1);
        };
        if (a == "--width") o.width = std::atoi(next("--width"));
        else if (a == "--height") o.height = std::atoi(next("--height"));
        else if (a == "--overpasses") o.numOverpasses = std::atoi(next("--overpasses"));
        else if (a == "--fps") o.targetFps = std::atof(next("--fps"));
        else if (a == "--endpoint") o.endpoint = next("--endpoint");
        else if (a == "--seed") o.seed = static_cast<unsigned>(std::atoi(next("--seed")));
        else if (a == "--source") o.source = next("--source");
        else if (a == "--ingest-endpoint") o.ingestEndpoint = next("--ingest-endpoint");
        else if (a == "--ingest-poll-ms") o.ingestPollMs = std::atoi(next("--ingest-poll-ms"));
        else if (a == "--stale-ceiling-sec") o.staleCeilingSec = std::atof(next("--stale-ceiling-sec"));
        else if (a == "--initial-grace-sec") o.initialGraceSec = std::atof(next("--initial-grace-sec"));
        else if (a == "--help") {
            std::printf(
                "l3s_engine [--width W] [--height H] [--overpasses N] [--fps F]\n"
                "           [--endpoint tcp://*:5556] [--seed S]\n"
                "           [--source sim|live] [--ingest-endpoint tcp://*:5557]\n"
                "           [--ingest-poll-ms 60] [--initial-grace-sec 8] [--stale-ceiling-sec 300]\n"
                "\n"
                "  --source sim   (default) generate every scene locally via Simulator.\n"
                "  --source live  pull scenes from python/ingestion_worker.py over\n"
                "                 --ingest-endpoint; falls back to Simulator locally if\n"
                "                 no worker has ever connected, or its feed goes stale.\n");
            std::exit(0);
        }
        else {
            std::fprintf(stderr, "unrecognized flag: %s (see --help)\n", a.c_str());
            std::exit(1);
        }
    }
    return o;
}

// Accumulates finite-value min/max over `v` into the running (mn, mx)
// (NaN = no-data gap, excluded). Caller seeds mn=+inf, mx=-inf and calls
// this once per grid to get a single shared scale across several grids.
static void accumulateMinMax(const std::vector<float>& v, float& mn, float& mx) {
    for (float x : v) {
        if (!std::isfinite(x)) continue;
        mn = std::min(mn, x);
        mx = std::max(mx, x);
    }
}

// A single raw overpass still carries real (non-NaN) SST values at
// cloud-masked pixels -- the mask lives only in Grid::valid. For display
// (and for a shared min/max with the already-NaN-gapped LVZA/L3S grids)
// we materialize cloud-masked pixels as NaN here.
static std::vector<float> maskedView(const l3s::Grid& g) {
    std::vector<float> out(g.data.size());
    for (size_t i = 0; i < g.data.size(); ++i)
        out[i] = g.valid[i] ? g.data[i] : l3s::NODATA;
    return out;
}

int main(int argc, char** argv) {
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    CliOptions opt = parseArgs(argc, argv);
    if (opt.source != "sim" && opt.source != "live") {
        std::fprintf(stderr, "--source must be 'sim' or 'live', got '%s'\n", opt.source.c_str());
        return 1;
    }

    std::printf("[l3s_engine] %dx%d grid, %d overpasses, target %.1f fps, publishing on %s\n",
                opt.width, opt.height, opt.numOverpasses, opt.targetFps, opt.endpoint.c_str());
    if (opt.source == "live") {
        std::printf("[l3s_engine] source=live, pulling scenes on %s (Simulator remains the local fallback)\n",
                    opt.ingestEndpoint.c_str());
    } else {
        std::printf("[l3s_engine] source=sim (local Simulator)\n");
    }

    l3s::SimulatorConfig simCfg;
    simCfg.width = opt.width;
    simCfg.height = opt.height;
    simCfg.numOverpasses = opt.numOverpasses;
    simCfg.seed = opt.seed;
    l3s::Simulator simulator(simCfg);

    l3s::EngineConfig engCfg; // S0=1.33, LCR 11x11, debias 21/11/7 -- paper defaults
    l3s::L3SEngine engine(engCfg);

    l3s::TelemetryPublisher publisher(opt.endpoint);

    // Only constructed (and only binds the inbound port) in live mode --
    // `--source sim` behaves exactly as it always has, with zero extra
    // sockets or overhead.
    std::optional<l3s::IngestionReceiver> ingestion;
    if (opt.source == "live") ingestion.emplace(opt.ingestEndpoint);

    const double frameBudgetMs = 1000.0 / std::max(0.1, opt.targetFps);
    const size_t elemsPerGrid = static_cast<size_t>(opt.width) * opt.height;

    uint32_t frameIndex = 0;
    double simTime = 0.0;

    // Live-mode scene cache: a new scene arrives only when the ingestion
    // worker pushes one (real granules don't arrive every 60ms), so
    // between arrivals we keep reprocessing the most recent scene rather
    // than either stalling or fabricating a new one -- this mirrors how
    // NRT ACSPO itself keeps serving the current best product between
    // granule arrivals (paper: "typical latency of 3-6 hours").
    std::vector<l3s::Overpass> currentOverpasses;
    bool haveScene = false;
    std::optional<bool> usingFallback; // nullopt = not yet classified -- see noteFallbackState
    const auto processStart = std::chrono::steady_clock::now();
    double lastArrivalSec = 0.0;

    // Prints a transition message the FIRST time a state is reached (live
    // or fallback -- unlike a plain bool, std::optional's initial nullopt
    // guarantees the very first classification always logs once) and on
    // every subsequent change, so the operator can see live/fallback
    // status in the console without touching the untouched dashboard wire
    // format.
    auto noteFallbackState = [&](bool fb, const char* reason) {
        if (!usingFallback.has_value() || *usingFallback != fb) {
            std::printf("\n[l3s_engine] %s\n", reason);
            std::fflush(stdout); // don't let this sit behind the \r-repeated frame-progress line
        }
        usingFallback = fb;
    };

    while (g_running.load()) {
        const auto t0 = std::chrono::steady_clock::now();
        bool haveWorkThisTick = true;

        if (opt.source == "live") {
            bool isLive = false;
            auto scene = ingestion->receiveScene(opt.ingestPollMs, static_cast<uint32_t>(opt.width),
                                                  static_cast<uint32_t>(opt.height), &isLive);
            const double elapsedSec = std::chrono::duration<double>(t0 - processStart).count();

            if (scene) {
                currentOverpasses = std::move(*scene);
                haveScene = true;
                lastArrivalSec = elapsedSec;
                noteFallbackState(!isLive, isLive
                    ? "live ingestion feed active"
                    : "ingestion worker reports its own upstream fetch failed -- it is sending synthetic data");
            } else if (!haveScene) {
                if (elapsedSec < opt.initialGraceSec) {
                    haveWorkThisTick = false; // still warming up, nothing to (re)process yet
                } else {
                    noteFallbackState(true, "no ingestion worker seen -- using local Simulator fallback");
                    currentOverpasses = simulator.generate(simTime);
                    simTime += 1.0;
                    haveScene = true; // now behaves like a normal (locally-sourced) cached scene
                    lastArrivalSec = elapsedSec;
                }
            } else if (elapsedSec - lastArrivalSec > opt.staleCeilingSec) {
                noteFallbackState(true, "ingestion feed stale -- switching to local Simulator fallback");
                currentOverpasses = simulator.generate(simTime);
                simTime += 1.0;
                lastArrivalSec = elapsedSec;
            }
            // else: no new scene this tick, but the cached one is still
            // fresh -- fall through and simply reprocess it.
        } else {
            currentOverpasses = simulator.generate(simTime);
            simTime += 1.0;
        }

        if (!haveWorkThisTick) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // 2. Run the full LCR / Eq.(1) / iterative-debiasing pipeline --
        //    identical call whether currentOverpasses came from the local
        //    Simulator or a live ingestion scene; Engine.hpp cannot tell
        //    the difference, by design.
        //
        //    Timed from HERE, not from t0: in --source live, t0 precedes
        //    receiveScene()'s poll, which legitimately blocks for up to
        //    --ingest-poll-ms waiting on the network -- that wait is
        //    real wall-clock time but it is not compute, and folding it
        //    into execMs/throughputMbS would silently understate the
        //    engine's actual throughput (badly: at the default 60ms
        //    poll budget it would dwarf the ~5-30ms the math itself
        //    takes on typical grid sizes). t0 is still used below, for
        //    the frame-pacing sleep, where the full tick duration
        //    (including any poll wait) is exactly what should count.
        const auto tComputeStart = std::chrono::steady_clock::now();
        l3s::EngineResult result = engine.run(currentOverpasses);

        const auto t1 = std::chrono::steady_clock::now();
        const double execMs = std::chrono::duration<double, std::milli>(t1 - tComputeStart).count();

        // Approximate matrix data volume touched by the pipeline this frame:
        // LCR + weight passes over every overpass, the LVZA/LVW composites,
        // and (diff + debiased-output) passes for every debiasing iteration.
        const size_t overpassBytes = currentOverpasses.size() * elemsPerGrid * sizeof(float);
        const size_t iterBytes = engCfg.debiasRadii.size() *
            (2 * overpassBytes + elemsPerGrid * sizeof(float));
        const size_t totalBytes = 2 * overpassBytes + 2 * elemsPerGrid * sizeof(float) + iterBytes;
        const double throughputMbS = execMs > 0.0
            ? (totalBytes / (1024.0 * 1024.0)) / (execMs / 1000.0)
            : 0.0;

        // 3. Package telemetry for the visual story: EVERY raw uncollated
        //    overpass (not just overpasses[0]) -> the legacy LVZA composite
        //    (stitching artifacts) -> the final fused L3S SST (smooth,
        //    SciML-ready). One raw grid per overpass, in overpass order.
        const auto& clean = result.finalL3S();
        std::vector<std::vector<float>> rawOverpasses;
        rawOverpasses.reserve(currentOverpasses.size());
        for (const auto& op : currentOverpasses) rawOverpasses.push_back(maskedView(op.sst));

        // Shared min/max across EVERY grid we're about to send (all raw
        // overpasses + LVZA + clean), so the color scale is locked
        // identically across the whole dashboard and the fragmented ->
        // jagged -> smooth progression is visually honest rather than an
        // artifact of per-panel auto-scaling.
        float sstMin = std::numeric_limits<float>::infinity();
        float sstMax = -std::numeric_limits<float>::infinity();
        for (const auto& raw : rawOverpasses) accumulateMinMax(raw, sstMin, sstMax);
        accumulateMinMax(result.lvzaReference.data, sstMin, sstMax);
        accumulateMinMax(clean.data, sstMin, sstMax);
        if (!std::isfinite(sstMin)) { sstMin = 0.f; sstMax = 0.f; }

        l3s::FrameHeader header;
        header.width = static_cast<uint32_t>(opt.width);
        header.height = static_cast<uint32_t>(opt.height);
        header.frameIndex = frameIndex;
        header.debiasIterations = static_cast<uint32_t>(engCfg.debiasRadii.size());
        header.numOverpasses = static_cast<uint32_t>(rawOverpasses.size());
        header.timestampUnixS = l3s::nowUnixSeconds();
        header.execTimeMs = execMs;
        header.throughputMbS = throughputMbS;
        header.memFootprintMb = l3s::currentRssMb();
        header.sstMinK = sstMin;
        header.sstMaxK = sstMax;

        publisher.publish(header, rawOverpasses, result.lvzaReference.data, clean.data);

        std::printf("\r[l3s_engine] frame %6u | exec %7.2f ms | %7.1f MB/s | RSS %6.1f MB   ",
                    frameIndex, execMs, throughputMbS, header.memFootprintMb);
        std::fflush(stdout);

        ++frameIndex;

        const auto t2 = std::chrono::steady_clock::now();
        const double elapsedMs = std::chrono::duration<double, std::milli>(t2 - t0).count();
        const double sleepMs = frameBudgetMs - elapsedMs;
        if (sleepMs > 0) {
            std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(sleepMs));
        }
    }

    std::printf("\n[l3s_engine] shutting down.\n");
    return 0;
}
