// Telemetry.hpp
//
// Binary wire protocol + ZeroMQ PUB publisher for streaming processed
// SST matrices and live system-performance metrics to the Python
// "Infrastructure Control Room" dashboard.
//
// Wire format v2 (bumped from v1's "L3S1" to "L3S3" -- see kMagic -- because
// this is a breaking change: variable frame count and a wider header):
// a ZMQ multipart message with 4 + num_overpasses frames -- the 3-stage
// visual story, now with every individual raw overpass instead of just
// overpasses[0] (fragmented single sensors -> jagged legacy composite ->
// smooth fused tensor):
//   [0]              topic   : ASCII bytes "L3S"
//   [1]              header  : fixed 64-byte little-endian binary struct (below)
//   [2 .. 2+N-1]     raw_i   : float32[width*height], row-major, one frame
//                              per overpass i in [0, num_overpasses) -- a
//                              raw, uncollated satellite overpass, NaN gaps
//   [2+N]            lvza    : float32[width*height], row-major -- the legacy
//                              lowest-VZA composite reference, NaN = no data
//   [3+N]            clean   : float32[width*height], row-major -- the final
//                              fused L3S SST after all debiasing iterations
//
// Header layout (all little-endian, packed manually with memcpy so there
// is zero dependency on compiler struct-padding rules; the Python side
// unpacks with struct.unpack("<6I4d2f", ...) which must match exactly):
//   uint32 magic            ('L','3','S','3' as 0x3353334C)
//   uint32 width
//   uint32 height
//   uint32 frame_index
//   uint32 debias_iterations
//   uint32 num_overpasses    (how many raw_i frames follow the header)
//   float64 timestamp_unix_s
//   float64 exec_time_ms        (wall-clock time for the full pipeline)
//   float64 throughput_mb_s     (approx. matrix bytes touched / exec time)
//   float64 mem_footprint_mb    (process RSS at publish time)
//   float32 sst_min_k           (finite-value min across ALL raw + LVZA +
//                                clean grids -- one locked shared scale for
//                                every panel in the dashboard)
//   float32 sst_max_k           (finite-value max across the same union)
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <chrono>
#include <fstream>
#include <sstream>
#include <cmath>

#include <zmq.hpp>

namespace l3s {

constexpr uint32_t kMagic = 0x3353334Cu; // "L3S3" little-endian (v2 wire format)

struct FrameHeader {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frameIndex = 0;
    uint32_t debiasIterations = 0;
    uint32_t numOverpasses = 0;
    double timestampUnixS = 0.0;
    double execTimeMs = 0.0;
    double throughputMbS = 0.0;
    double memFootprintMb = 0.0;
    float sstMinK = 0.f;
    float sstMaxK = 0.f;
};

inline std::vector<uint8_t> serializeHeader(const FrameHeader& h) {
    std::vector<uint8_t> buf;
    buf.reserve(64);
    auto put = [&buf](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        buf.insert(buf.end(), b, b + n);
    };
    uint32_t magic = kMagic;
    put(&magic, 4);
    put(&h.width, 4);
    put(&h.height, 4);
    put(&h.frameIndex, 4);
    put(&h.debiasIterations, 4);
    put(&h.numOverpasses, 4);
    put(&h.timestampUnixS, 8);
    put(&h.execTimeMs, 8);
    put(&h.throughputMbS, 8);
    put(&h.memFootprintMb, 8);
    put(&h.sstMinK, 4);
    put(&h.sstMaxK, 4);
    return buf; // 6*4 + 4*8 + 2*4 = 64 bytes
}

// Reads current process resident set size in MB from /proc/self/status
// (Linux). Falls back to 0.0 if unavailable.
inline double currentRssMb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream iss(line.substr(6));
            double kb = 0.0;
            iss >> kb;
            return kb / 1024.0;
        }
    }
    return 0.0;
}

inline double nowUnixSeconds() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

class TelemetryPublisher {
public:
    explicit TelemetryPublisher(const std::string& endpoint)
        : ctx_(1), socket_(ctx_, zmq::socket_type::pub) {
        // A brief linger avoids blocking indefinitely on shutdown if a
        // subscriber is slow; small SNDHWM keeps the PUB socket from
        // silently piling up memory if no subscriber is connected yet
        // (ZMQ PUB drops rather than blocks once HWM is hit, which is the
        // right behavior for a live telemetry feed -- stale frames should
        // be dropped, not queued).
        socket_.set(zmq::sockopt::linger, 0);
        socket_.set(zmq::sockopt::sndhwm, 4);
        socket_.bind(endpoint);
    }

    // `rawOverpasses` holds one float32 grid per overpass -- header.numOverpasses
    // MUST equal rawOverpasses.size() (the caller is expected to set it from
    // the same vector before calling this).
    void publish(const FrameHeader& header, const std::vector<std::vector<float>>& rawOverpasses,
                 const std::vector<float>& lvza, const std::vector<float>& clean) {
        auto headerBytes = serializeHeader(header);

        zmq::message_t topic("L3S", 3);
        zmq::message_t hdr(headerBytes.data(), headerBytes.size());

        socket_.send(topic, zmq::send_flags::sndmore);
        socket_.send(hdr, zmq::send_flags::sndmore);
        for (const auto& raw : rawOverpasses) {
            zmq::message_t rawMsg(raw.data(), raw.size() * sizeof(float));
            socket_.send(rawMsg, zmq::send_flags::sndmore);
        }
        zmq::message_t lvzaMsg(lvza.data(), lvza.size() * sizeof(float));
        zmq::message_t cleanMsg(clean.data(), clean.size() * sizeof(float));
        socket_.send(lvzaMsg, zmq::send_flags::sndmore);
        socket_.send(cleanMsg, zmq::send_flags::none);
    }

private:
    zmq::context_t ctx_;
    zmq::socket_t socket_;
};

} // namespace l3s
