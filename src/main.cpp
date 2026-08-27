#include "l3s/Engine.hpp"
#include "l3s/Ingestion.hpp"
#include "l3s/Simulator.hpp"
#include "l3s/Telemetry.hpp"

#include <zmq.hpp>

#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

constexpr int kIngestPollTimeoutMs = 200;

struct Args {
    int width = 64;
    int height = 64;
    int overpasses = 4;
    double fps = 2.0;
    std::string endpoint = "tcp://*:5556";
    std::string source = "sim"; // "sim" | "live"
    std::string ingestEndpoint = "tcp://*:5557";
    double staleCeilingS = 15.0; // how long a live scene stays "current" before reverting to fallback
};

static Args parseArgs(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--width" && i + 1 < argc) {
            args.width = std::stoi(argv[++i]);
        } else if (flag == "--height" && i + 1 < argc) {
            args.height = std::stoi(argv[++i]);
        } else if (flag == "--overpasses" && i + 1 < argc) {
            args.overpasses = std::stoi(argv[++i]);
        } else if (flag == "--fps" && i + 1 < argc) {
            args.fps = std::stod(argv[++i]);
        } else if (flag == "--endpoint" && i + 1 < argc) {
            args.endpoint = argv[++i];
        } else if (flag == "--source" && i + 1 < argc) {
            args.source = argv[++i];
        } else if (flag == "--ingest-endpoint" && i + 1 < argc) {
            args.ingestEndpoint = argv[++i];
        } else if (flag == "--stale-ceiling-sec" && i + 1 < argc) {
            args.staleCeilingS = std::stod(argv[++i]);
        }
    }
    return args;
}

int main(int argc, char** argv) {
    const Args args = parseArgs(argc, argv);
    const int publishIntervalMs = args.fps > 0.0 ? static_cast<int>(1000.0 / args.fps) : 500;

    zmq::context_t ctx(1);
    zmq::socket_t pub(ctx, zmq::socket_type::pub);
    pub.bind(args.endpoint);

    // A PUB socket silently drops anything sent before a SUB has joined
    // and completed its connect handshake, so give one a moment to attach.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    l3s::SimulatorConfig cfg;
    cfg.width = args.width;
    cfg.height = args.height;
    cfg.numOverpasses = args.overpasses;
    l3s::Simulator sim(cfg);
    l3s::TelemetryPublisher telemetry(pub);

    std::optional<l3s::IngestionReceiver> receiver;
    if (args.source == "live") {
        receiver.emplace(ctx, args.ingestEndpoint);
        std::cout << "[l3s_engine] source=live, ingestion PULL bound at " << args.ingestEndpoint << "\n";
    } else {
        std::cout << "[l3s_engine] source=sim\n";
    }

    std::vector<l3s::Overpass> currentOverpasses;
    bool haveLiveScene = false;
    std::chrono::steady_clock::time_point lastLiveSceneTime{};
    std::optional<bool> usingFallback; // nullopt = not yet classified

    double t = 0.0;
    uint32_t frameIndex = 0;
    while (true) {
        if (args.source == "live") {
            const auto sceneOpt = receiver->receiveScene(kIngestPollTimeoutMs);
            const auto now = std::chrono::steady_clock::now();
            bool fellBack;

            if (sceneOpt && sceneOpt->width == static_cast<uint32_t>(cfg.width) &&
                sceneOpt->height == static_cast<uint32_t>(cfg.height) &&
                sceneOpt->numOverpasses == static_cast<uint32_t>(cfg.numOverpasses)) {
                currentOverpasses.clear();
                for (uint32_t k = 0; k < sceneOpt->numOverpasses; ++k) {
                    l3s::Overpass op;
                    op.sst = sceneOpt->sst[k];
                    op.vza = l3s::computeVzaField(cfg.width, cfg.height, static_cast<int>(k), cfg.numOverpasses, t,
                                                   cfg.maxVzaDeg);
                    currentOverpasses.push_back(std::move(op));
                }
                haveLiveScene = true;
                lastLiveSceneTime = now;
                fellBack = false;
            } else {
                // No new scene arrived this tick -- that's normal (the
                // worker's interval is far longer than the engine's poll
                // timeout) and not itself a fallback: keep reprocessing
                // the last live scene as long as it's within the
                // staleness ceiling. Only revert to the local simulator
                // if we've never gotten a live scene at all, or the one
                // we have has gone stale (the worker is presumed dead).
                const double staleS =
                    haveLiveScene ? std::chrono::duration<double>(now - lastLiveSceneTime).count() : -1.0;
                fellBack = !haveLiveScene || staleS > args.staleCeilingS;
                if (fellBack) {
                    currentOverpasses = sim.generateOverpasses(t);
                }
            }

            if (!usingFallback.has_value() || *usingFallback != fellBack) {
                std::cout << "[l3s_engine] "
                          << (fellBack ? "ingestion worker unavailable/stale -- using local fallback"
                                       : "receiving live scenes from ingestion worker")
                          << "\n";
                usingFallback = fellBack;
            }
        } else {
            currentOverpasses = sim.generateOverpasses(t);
        }

        const auto& overpasses = currentOverpasses;

        // Timed from here, not from the top of the loop: the live path's
        // receiveScene() above blocks on a ZMQ poll for up to
        // kIngestPollTimeoutMs, which is wait time, not compute time --
        // folding it into execMs would make the HUD's throughput number
        // describe how promptly the worker happens to publish, not how
        // fast the fusion pipeline itself runs.
        const auto tComputeStart = std::chrono::steady_clock::now();

        // LVZA is computed on the raw, undebiased overpasses -- it's the
        // legacy reference, and the legacy method never saw a debiasing
        // step. The weighted composite gets the benefit of debiasing.
        const auto lvza = l3s::buildLvzaComposite(overpasses, cfg.width, cfg.height);

        auto debiased = overpasses;
        l3s::debiasOverpasses(debiased, cfg.width, cfg.height);

        std::vector<std::vector<float>> weights;
        weights.reserve(debiased.size());
        for (const auto& op : debiased) {
            const auto lcr = l3s::computeLCR(op.sst, cfg.width, cfg.height);
            weights.push_back(l3s::computeWeights(op, lcr));
        }
        const auto fused = l3s::buildWeightedComposite(debiased, weights, cfg.width, cfg.height);

        const auto tComputeEnd = std::chrono::steady_clock::now();
        const double execMs = std::chrono::duration<double, std::milli>(tComputeEnd - tComputeStart).count();

        std::vector<std::vector<float>> rawOverpasses;
        rawOverpasses.reserve(overpasses.size());
        for (const auto& op : overpasses) rawOverpasses.push_back(op.sst);

        const size_t totalFieldBytes =
            (rawOverpasses.size() + 2) * static_cast<size_t>(cfg.width) * cfg.height * sizeof(float);
        const double throughputMbS = execMs > 0.0 ? (totalFieldBytes / (1024.0 * 1024.0)) / (execMs / 1000.0) : 0.0;

        l3s::FrameHeader header;
        header.width = static_cast<uint32_t>(cfg.width);
        header.height = static_cast<uint32_t>(cfg.height);
        header.numOverpasses = static_cast<uint32_t>(overpasses.size());
        header.frameIndex = frameIndex;
        header.timestampS = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        header.execMs = execMs;
        header.throughputMbS = throughputMbS;

        telemetry.publish(header, rawOverpasses, lvza, fused);

        std::cout << "published frame " << frameIndex << "  execMs=" << execMs
                  << "  throughputMbS=" << throughputMbS << "\n";

        ++frameIndex;
        t += 1.0;
        std::this_thread::sleep_for(std::chrono::milliseconds(publishIntervalMs));
    }

    return 0;
}
