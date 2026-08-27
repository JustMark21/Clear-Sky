#include "l3s/Engine.hpp"
#include "l3s/Simulator.hpp"
#include "l3s/Telemetry.hpp"

#include <zmq.hpp>

#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

constexpr int kPublishIntervalMs = 500;

int main() {
    zmq::context_t ctx(1);
    zmq::socket_t pub(ctx, zmq::socket_type::pub);
    pub.bind("tcp://*:5556");

    // A PUB socket silently drops anything sent before a SUB has joined
    // and completed its connect handshake, so give one a moment to attach.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    l3s::SimulatorConfig cfg;
    l3s::Simulator sim(cfg);
    l3s::TelemetryPublisher telemetry(pub);

    double t = 0.0;
    uint32_t frameIndex = 0;
    while (true) {
        const auto tComputeStart = std::chrono::steady_clock::now();

        const auto overpasses = sim.generateOverpasses(t);

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
        std::this_thread::sleep_for(std::chrono::milliseconds(kPublishIntervalMs));
    }

    return 0;
}
