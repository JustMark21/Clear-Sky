// Ingestion.hpp
//
// Inbound wire protocol + ZeroMQ PULL receiver for live overpass data
// pushed in by an external ingestion worker (python/ingestion_worker.py).
// This is how l3s_engine can be fed real satellite scenes instead of (or
// alongside) the local Simulator, WITHOUT touching Engine.hpp or
// SummedAreaTable.hpp: this header's only job is to turn wire bytes into
// exactly the same std::vector<Overpass> that Simulator::generate()
// already produces, so L3SEngine::run() cannot tell the difference.
//
// Pipeline (two independent, differently-patterned ZMQ channels):
//
//   [ingestion_worker.py] --PUSH/PULL(tcp://*:5557)--> [l3s_engine] --PUB/SUB(tcp://*:5556)--> [dashboard]
//                  |                                        ^
//                  +-- live NOAA/GHRSST ERDDAP fetch,        |
//                      or synthetic fallback on any failure  +-- Telemetry.hpp, totally unchanged
//
// PUSH/PULL (not PUB/SUB) is used deliberately for the inbound side: this
// channel carries data-of-record, not disposable telemetry, so silently
// dropping a scene under load is the wrong default (unlike the outbound
// PUB feed, which *should* drop stale frames rather than block). The
// engine BINDs the PULL socket -- it is the long-lived, stable process,
// symmetric with how it already BINDs its outbound PUB socket; the worker
// CONNECTs as PUSH, since it is the more disposable, restart-anytime side.
//
// Wire format: one ZMQ multipart message per scene, exactly
// 3 + 3*numOverpasses frames, all little-endian and length-validated
// against the header before any memcpy (never trust the wire -- a wrong
// dtype, a stale/mismatched grid size, or a truncated frame is rejected
// and logged rather than reinterpreted):
//   [0] topic       : ASCII bytes "L3SIN"
//   [1] SceneHeader : fixed 32-byte binary struct (below)
//   [2] meta        : numOverpasses * 12-byte OverpassMeta records
//   [3..]           : per overpass, in order: float32 SST grid [K],
//                      float32 VZA grid [deg], uint8 validity mask
//                      (1 byte/pixel, not bit-packed) -- 3 frames/overpass,
//                      each exactly width*height elements
//
// SceneHeader (32 bytes; Python side: struct.pack("<6I d", magic, width,
// height, numOverpasses, dataSource, sceneId, timestampUnixS)):
//   uint32 magic            ('L','3','S','2' -> 0x3253334C)
//   uint32 width
//   uint32 height
//   uint32 numOverpasses
//   uint32 dataSource        (0 = synthetic fallback, 1 = live fetch)
//   uint32 sceneId           (monotonic counter, worker-assigned)
//   float64 timestampUnixS
//
// OverpassMeta (12 bytes each; Python side: struct.pack("<8sf", name, avgVZA)):
//   char[8] name             (ASCII, NUL-padded, e.g. b"A1")
//   float32 avgVZA           (informational only -- the engine recomputes
//                             per-pixel Eq.(1) weights from the VZA grid)
#pragma once

#include "Grid.hpp"
#include <zmq.hpp>
#include <vector>
#include <optional>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <chrono>

namespace l3s {

constexpr uint32_t kIngestMagic = 0x3253334Cu; // "L3S2"
// magic + width + height + numOverpasses + dataSource + sceneId (6 uint32
// fields) + timestampUnixS (1 double) = 6*4 + 8 = 32 bytes. Keep this in
// exact sync with parseHeader()'s field offsets below -- it's the first
// line of defense against a wire/struct mismatch.
constexpr size_t kSceneHeaderBytes = 6 * sizeof(uint32_t) + sizeof(double); // 32
constexpr size_t kOverpassMetaBytes = 8 + sizeof(float);                   // 12

struct SceneHeader {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t numOverpasses = 0;
    uint32_t dataSource = 0; // 0 = synthetic fallback, 1 = live fetch
    uint32_t sceneId = 0;
    double timestampUnixS = 0.0;
};

class IngestionReceiver {
public:
    explicit IngestionReceiver(const std::string& endpoint)
        : ctx_(1), socket_(ctx_, zmq::socket_type::pull) {
        socket_.set(zmq::sockopt::linger, 0);
        // Bounded: a live-data channel should never let an unconsumed
        // backlog grow without limit. If the engine falls behind, ZMQ
        // applies backpressure to the worker's PUSH socket (which itself
        // has a short SNDTIMEO and drops-with-a-log rather than blocking
        // forever -- see ingestion_worker.py) instead of this side's
        // memory growing unboundedly.
        socket_.set(zmq::sockopt::rcvhwm, 4);
        socket_.bind(endpoint);
    }

    // Polls for one complete scene for up to timeoutMs. Returns
    // std::nullopt on timeout, OR on any malformed / size-mismatched /
    // wrong-grid-shape frame -- every such case is logged to stderr and
    // the caller is expected to fall back to the local Simulator; this
    // function never partially-trusts a bad payload.
    std::optional<std::vector<Overpass>> receiveScene(int timeoutMs, uint32_t expectedWidth,
                                                        uint32_t expectedHeight,
                                                        bool* outIsLive = nullptr) {
        zmq::pollitem_t items[] = {{socket_.handle(), 0, ZMQ_POLLIN, 0}};
        zmq::poll(items, 1, std::chrono::milliseconds(timeoutMs));
        if (!(items[0].revents & ZMQ_POLLIN)) return std::nullopt;

        std::vector<zmq::message_t> parts;
        for (;;) {
            zmq::message_t msg;
            auto res = socket_.recv(msg, zmq::recv_flags::none);
            if (!res) return std::nullopt;
            const bool more = socket_.get(zmq::sockopt::rcvmore) != 0;
            parts.push_back(std::move(msg));
            if (!more) break;
        }

        if (parts.size() < 3) {
            warn("scene has too few frames (%zu, need >= 3)", parts.size());
            return std::nullopt;
        }
        if (parts[1].size() != kSceneHeaderBytes) {
            warn("SceneHeader wrong size (%zu bytes, expected %zu)", parts[1].size(), kSceneHeaderBytes);
            return std::nullopt;
        }

        const SceneHeader hdr = parseHeader(parts[1]);
        if (hdr.width == 0) { warn("SceneHeader magic mismatch -- dropping frame"); return std::nullopt; }
        if (hdr.height == 0 || hdr.numOverpasses == 0) {
            warn("SceneHeader has a zero dimension/overpass count");
            return std::nullopt;
        }
        if (hdr.width != expectedWidth || hdr.height != expectedHeight) {
            warn("SceneHeader grid %ux%u != engine grid %ux%u -- rejecting scene "
                 "(the ingestion worker's --width/--height must match l3s_engine's)",
                 hdr.width, hdr.height, expectedWidth, expectedHeight);
            return std::nullopt;
        }

        const size_t expectedFrames = 3 + 3 * static_cast<size_t>(hdr.numOverpasses);
        if (parts.size() != expectedFrames) {
            warn("expected %zu frames for %u overpasses, got %zu -- rejecting scene",
                 expectedFrames, hdr.numOverpasses, parts.size());
            return std::nullopt;
        }
        if (parts[2].size() != static_cast<size_t>(hdr.numOverpasses) * kOverpassMetaBytes) {
            warn("OverpassMeta block wrong size (%zu bytes, expected %zu)",
                 parts[2].size(), static_cast<size_t>(hdr.numOverpasses) * kOverpassMetaBytes);
            return std::nullopt;
        }

        // Fixing the expected byte length to EXACTLY width*height*sizeof(float)
        // is what catches a dtype mismatch (e.g. a float64 payload sent by
        // mistake) as a hard rejection rather than a silent reinterpret.
        const size_t floatBytes = static_cast<size_t>(hdr.width) * hdr.height * sizeof(float);
        const size_t maskBytes = static_cast<size_t>(hdr.width) * hdr.height * sizeof(uint8_t);

        std::vector<Overpass> overpasses;
        overpasses.reserve(hdr.numOverpasses);
        const uint8_t* metaPtr = static_cast<const uint8_t*>(parts[2].data());

        for (uint32_t i = 0; i < hdr.numOverpasses; ++i) {
            const size_t base = 3 + 3 * static_cast<size_t>(i);
            const zmq::message_t& sstMsg = parts[base + 0];
            const zmq::message_t& vzaMsg = parts[base + 1];
            const zmq::message_t& validMsg = parts[base + 2];
            if (sstMsg.size() != floatBytes || vzaMsg.size() != floatBytes || validMsg.size() != maskBytes) {
                warn("overpass %u array size mismatch (sst=%zu vza=%zu valid=%zu, expected %zu/%zu/%zu) "
                     "-- rejecting whole scene",
                     i, sstMsg.size(), vzaMsg.size(), validMsg.size(), floatBytes, floatBytes, maskBytes);
                return std::nullopt;
            }

            Overpass op;
            char nameBuf[9] = {0};
            std::memcpy(nameBuf, metaPtr + i * kOverpassMetaBytes, 8);
            op.name = std::string(nameBuf);
            float avgVza = 0.f;
            std::memcpy(&avgVza, metaPtr + i * kOverpassMetaBytes + 8, sizeof(float));
            op.avgVZA = avgVza;

            op.sst = Grid(static_cast<int>(hdr.width), static_cast<int>(hdr.height));
            op.vza = Grid(static_cast<int>(hdr.width), static_cast<int>(hdr.height));
            std::memcpy(op.sst.data.data(), sstMsg.data(), floatBytes);
            std::memcpy(op.vza.data.data(), vzaMsg.data(), floatBytes);
            std::memcpy(op.sst.valid.data(), validMsg.data(), maskBytes);
            overpasses.push_back(std::move(op));
        }

        if (outIsLive) *outIsLive = (hdr.dataSource == 1);
        return overpasses;
    }

private:
    static SceneHeader parseHeader(const zmq::message_t& m) {
        SceneHeader h;
        const uint8_t* p = static_cast<const uint8_t*>(m.data());
        uint32_t magic = 0;
        std::memcpy(&magic, p + 0, 4);
        if (magic != kIngestMagic) return h; // width stays 0 -> caller rejects
        std::memcpy(&h.width, p + 4, 4);
        std::memcpy(&h.height, p + 8, 4);
        std::memcpy(&h.numOverpasses, p + 12, 4);
        std::memcpy(&h.dataSource, p + 16, 4);
        std::memcpy(&h.sceneId, p + 20, 4);
        std::memcpy(&h.timestampUnixS, p + 24, 8);
        return h;
    }

    static void warn(const char* fmt, ...) {
        std::fprintf(stderr, "[ingestion] WARN: ");
        va_list args;
        va_start(args, fmt);
        std::vfprintf(stderr, fmt, args);
        va_end(args);
        std::fprintf(stderr, "\n");
    }

    zmq::context_t ctx_;
    zmq::socket_t socket_;
};

} // namespace l3s
