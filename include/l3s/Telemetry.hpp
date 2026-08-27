#pragma once

#include <zmq.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

namespace l3s {

constexpr uint32_t kFrameMagic = 0x3353334C;   // "L3S3"
constexpr uint32_t kFrameVersion = 1;

// Fixed 48-byte header: 6x uint32_t (24 bytes) followed by 3x double (24
// bytes), each field written with its native little-endian layout (both
// ends of this pipeline run on x86_64). Every frame starts with this
// header as its own ZMQ part, ahead of the field data, so a receiver
// knows exactly how many parts and bytes to expect before touching any
// of them.
struct FrameHeader {
    uint32_t magic = kFrameMagic;
    uint32_t version = kFrameVersion;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t numOverpasses = 0;
    uint32_t frameIndex = 0;
    double timestampS = 0.0;
    double execMs = 0.0;
    double throughputMbS = 0.0;
};

constexpr size_t kFrameHeaderBytes = 6 * sizeof(uint32_t) + 3 * sizeof(double);

inline std::vector<uint8_t> serializeHeader(const FrameHeader& h) {
    std::vector<uint8_t> buf(kFrameHeaderBytes);
    size_t off = 0;
    auto putU32 = [&](uint32_t v) { std::memcpy(buf.data() + off, &v, sizeof(v)); off += sizeof(v); };
    auto putF64 = [&](double v) { std::memcpy(buf.data() + off, &v, sizeof(v)); off += sizeof(v); };
    putU32(h.magic);
    putU32(h.version);
    putU32(h.width);
    putU32(h.height);
    putU32(h.numOverpasses);
    putU32(h.frameIndex);
    putF64(h.timestampS);
    putF64(h.execMs);
    putF64(h.throughputMbS);
    return buf;
}

// Publishes one full frame as a multipart message: the header, then one
// part per raw overpass, then LVZA, then the fused composite. Every
// field part's byte length is exactly width*height*sizeof(float) by
// construction -- the header carries width/height/numOverpasses so a
// receiver can validate that before ever reinterpreting the bytes.
class TelemetryPublisher {
public:
    explicit TelemetryPublisher(zmq::socket_t& pub) : pub_(pub) {}

    void publish(const FrameHeader& header, const std::vector<std::vector<float>>& rawOverpasses,
                 const std::vector<float>& lvza, const std::vector<float>& fused) {
        const auto headerBytes = serializeHeader(header);
        sendBytes(headerBytes.data(), headerBytes.size(), true);
        for (const auto& op : rawOverpasses) {
            sendFloats(op, true);
        }
        sendFloats(lvza, true);
        sendFloats(fused, false);
    }

private:
    void sendBytes(const void* data, size_t n, bool more) {
        zmq::message_t msg(n);
        std::memcpy(msg.data(), data, n);
        pub_.send(msg, more ? zmq::send_flags::sndmore : zmq::send_flags::none);
    }

    void sendFloats(const std::vector<float>& v, bool more) { sendBytes(v.data(), v.size() * sizeof(float), more); }

    zmq::socket_t& pub_;
};

} // namespace l3s
