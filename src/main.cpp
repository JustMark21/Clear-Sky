#include "l3s/Engine.hpp"
#include "l3s/Simulator.hpp"

#include <zmq.hpp>

#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

constexpr int kPublishIntervalMs = 500;

static void sendField(zmq::socket_t& pub, const std::vector<float>& field, bool more) {
    zmq::message_t msg(field.size() * sizeof(float));
    std::memcpy(msg.data(), field.data(), msg.size());
    pub.send(msg, more ? zmq::send_flags::sndmore : zmq::send_flags::none);
}

static double meanLcr(const std::vector<float>& lcr) {
    double sum = 0.0;
    for (float v : lcr) sum += v;
    return lcr.empty() ? 0.0 : sum / static_cast<double>(lcr.size());
}

int main() {
    zmq::context_t ctx(1);
    zmq::socket_t pub(ctx, zmq::socket_type::pub);
    pub.bind("tcp://*:5556");

    // A PUB socket silently drops anything sent before a SUB has joined
    // and completed its connect handshake, so give one a moment to attach.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    l3s::SimulatorConfig cfg;
    l3s::Simulator sim(cfg);

    double t = 0.0;
    while (true) {
        const auto overpasses = sim.generateOverpasses(t);
        const auto lvza = l3s::buildLvzaComposite(overpasses, cfg.width, cfg.height);

        // LCR is computed per overpass here so its behavior can be
        // sanity-checked against real cloud coverage before it feeds
        // into any fusion weighting.
        for (size_t k = 0; k < overpasses.size(); ++k) {
            const auto lcr = l3s::computeLCR(overpasses[k].sst, cfg.width, cfg.height);
            std::cout << "overpass " << k << " mean LCR=" << meanLcr(lcr) << "\n";
        }

        // Wire layout unchanged: one part per raw overpass, in order,
        // followed by the LVZA composite as the final part.
        for (size_t k = 0; k < overpasses.size(); ++k) {
            sendField(pub, overpasses[k].sst, true);
        }
        sendField(pub, lvza, false);

        t += 1.0;
        std::this_thread::sleep_for(std::chrono::milliseconds(kPublishIntervalMs));
    }

    return 0;
}
