#include "l3s/Simulator.hpp"

#include <zmq.hpp>

#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

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

    double t = 0.0;
    while (true) {
        const auto field = sim.generate(t);

        zmq::message_t msg(field.size() * sizeof(float));
        std::memcpy(msg.data(), field.data(), msg.size());
        pub.send(msg, zmq::send_flags::none);

        std::cout << "published " << cfg.width << "x" << cfg.height << " field  t=" << t << "\n";

        t += 1.0;
        std::this_thread::sleep_for(std::chrono::milliseconds(kPublishIntervalMs));
    }

    return 0;
}
