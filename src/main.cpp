#include <zmq.hpp>

#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

constexpr int kWidth = 64;
constexpr int kHeight = 64;
constexpr int kPublishIntervalMs = 500;

int main() {
    zmq::context_t ctx(1);
    zmq::socket_t pub(ctx, zmq::socket_type::pub);
    pub.bind("tcp://*:5556");

    // A PUB socket silently drops anything sent before a SUB has joined
    // and completed its connect handshake, so give one a moment to attach.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::vector<float> grid(static_cast<size_t>(kWidth) * kHeight, 0.0f);

    while (true) {
        zmq::message_t msg(grid.size() * sizeof(float));
        std::memcpy(msg.data(), grid.data(), msg.size());
        pub.send(msg, zmq::send_flags::none);

        std::cout << "published " << kWidth << "x" << kHeight << " grid\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(kPublishIntervalMs));
    }

    return 0;
}
