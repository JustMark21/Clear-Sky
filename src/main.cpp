#include "l3s/Engine.hpp"
#include "l3s/Simulator.hpp"

#include <zmq.hpp>

#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

constexpr int kPublishIntervalMs = 500;

static void sendField(zmq::socket_t& pub, const std::vector<float>& field, bool more) {
    zmq::message_t msg(field.size() * sizeof(float));
    std::memcpy(msg.data(), field.data(), msg.size());
    pub.send(msg, more ? zmq::send_flags::sndmore : zmq::send_flags::none);
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

        std::vector<std::vector<float>> weights;
        weights.reserve(overpasses.size());
        for (const auto& op : overpasses) {
            const auto lcr = l3s::computeLCR(op.sst, cfg.width, cfg.height);
            weights.push_back(l3s::computeWeights(op, lcr));
        }

        const auto lvza = l3s::buildLvzaComposite(overpasses, cfg.width, cfg.height);
        const auto fused = l3s::buildWeightedComposite(overpasses, weights, cfg.width, cfg.height);

        // Wire layout: one part per raw overpass, then LVZA, then the
        // Eq. (1) weighted composite -- LVZA kept as a legacy reference
        // alongside the primary fused output.
        for (size_t k = 0; k < overpasses.size(); ++k) {
            sendField(pub, overpasses[k].sst, true);
        }
        sendField(pub, lvza, true);
        sendField(pub, fused, false);

        std::cout << "published " << overpasses.size() << " overpasses + lvza + fused  t=" << t << "\n";

        t += 1.0;
        std::this_thread::sleep_for(std::chrono::milliseconds(kPublishIntervalMs));
    }

    return 0;
}
