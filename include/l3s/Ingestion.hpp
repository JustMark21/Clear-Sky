#pragma once

#include <zmq.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace l3s {

constexpr uint32_t kSceneMagic = 0x3253334C;   // "L3S2"
constexpr uint32_t kSceneVersion = 1;

// Fixed 32-byte header: 6x uint32_t (24 bytes) + 1x double (8 bytes).
struct SceneHeader {
    uint32_t magic = kSceneMagic;
    uint32_t version = kSceneVersion;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t numOverpasses = 0;
    uint32_t reserved = 0;
    double timestampS = 0.0;
};
constexpr size_t kSceneHeaderBytes = 6 * sizeof(uint32_t) + sizeof(double);

// Fixed 12-byte per-overpass diagnostic record: which upstream source
// fed this pass, and a quick summary of what it contained -- not
// consumed by the fusion math (VZA is synthesized separately, on the
// engine side, since it's purely geometric), just carried through for
// logging.
struct OverpassMeta {
    uint32_t sourceId = 0;
    float meanSstK = 0.0f;
    float validFraction = 0.0f;
};
constexpr size_t kOverpassMetaBytes = sizeof(uint32_t) + 2 * sizeof(float);

struct Scene {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t numOverpasses = 0;
    double timestampS = 0.0;
    std::vector<OverpassMeta> meta;
    std::vector<std::vector<float>> sst;
};

inline bool decodeSceneHeader(const void* bytes, size_t len, SceneHeader& out) {
    if (len != kSceneHeaderBytes) return false;
    const auto* p = static_cast<const uint8_t*>(bytes);
    size_t off = 0;
    auto getU32 = [&] {
        uint32_t v;
        std::memcpy(&v, p + off, sizeof(v));
        off += sizeof(v);
        return v;
    };
    auto getF64 = [&] {
        double v;
        std::memcpy(&v, p + off, sizeof(v));
        off += sizeof(v);
        return v;
    };

    SceneHeader h;
    h.magic = getU32();
    if (h.magic != kSceneMagic) return false;
    h.version = getU32();
    if (h.version != kSceneVersion) return false;
    h.width = getU32();
    h.height = getU32();
    h.numOverpasses = getU32();
    h.reserved = getU32();
    h.timestampS = getF64();
    out = h;
    return true;
}

inline bool decodeOverpassMeta(const void* bytes, size_t len, OverpassMeta& out) {
    if (len != kOverpassMetaBytes) return false;
    const auto* p = static_cast<const uint8_t*>(bytes);
    size_t off = 0;
    std::memcpy(&out.sourceId, p + off, sizeof(out.sourceId));
    off += sizeof(out.sourceId);
    std::memcpy(&out.meanSstK, p + off, sizeof(out.meanSstK));
    off += sizeof(out.meanSstK);
    std::memcpy(&out.validFraction, p + off, sizeof(out.validFraction));
    return true;
}

// Inbound side of the ingestion channel: the engine binds a PULL socket
// and the ingestion worker connects and pushes scenes -- a bounded,
// backpressured, data-of-record channel, the opposite of the disposable
// PUB/SUB telemetry the engine publishes outward.
class IngestionReceiver {
public:
    explicit IngestionReceiver(zmq::context_t& ctx, const std::string& endpoint)
        : pull_(ctx, zmq::socket_type::pull) {
        pull_.bind(endpoint);
    }

    // Waits up to timeoutMs for one multipart scene. Returns nullopt if
    // nothing arrived within the timeout, or if what arrived fails
    // validation at any point -- every part's byte length is checked
    // against what the header (or the field size it implies) requires
    // before that part is ever reinterpreted as anything else.
    std::optional<Scene> receiveScene(int timeoutMs) {
        zmq::pollitem_t items[] = {{pull_.handle(), 0, ZMQ_POLLIN, 0}};
        zmq::poll(items, 1, std::chrono::milliseconds(timeoutMs));
        if (!(items[0].revents & ZMQ_POLLIN)) return std::nullopt;

        std::vector<zmq::message_t> parts;
        while (true) {
            zmq::message_t msg;
            const auto res = pull_.recv(msg, zmq::recv_flags::none);
            if (!res) return std::nullopt;
            const bool more = pull_.get(zmq::sockopt::rcvmore) != 0;
            parts.push_back(std::move(msg));
            if (!more) break;
        }
        return decode(parts);
    }

private:
    static std::optional<Scene> decode(const std::vector<zmq::message_t>& parts) {
        if (parts.empty()) return std::nullopt;

        SceneHeader header;
        if (!decodeSceneHeader(parts[0].data(), parts[0].size(), header)) return std::nullopt;

        const size_t expectedParts = 1 + static_cast<size_t>(header.numOverpasses) * 2;
        if (parts.size() != expectedParts) return std::nullopt;

        const size_t expectedFieldBytes = static_cast<size_t>(header.width) * header.height * sizeof(float);

        Scene scene;
        scene.width = header.width;
        scene.height = header.height;
        scene.numOverpasses = header.numOverpasses;
        scene.timestampS = header.timestampS;
        scene.meta.reserve(header.numOverpasses);
        scene.sst.reserve(header.numOverpasses);

        for (uint32_t k = 0; k < header.numOverpasses; ++k) {
            const auto& metaPart = parts[1 + 2 * k];
            const auto& dataPart = parts[2 + 2 * k];

            OverpassMeta meta;
            if (!decodeOverpassMeta(metaPart.data(), metaPart.size(), meta)) return std::nullopt;
            if (dataPart.size() != expectedFieldBytes) return std::nullopt;

            std::vector<float> field(header.width * static_cast<size_t>(header.height));
            std::memcpy(field.data(), dataPart.data(), expectedFieldBytes);

            scene.meta.push_back(meta);
            scene.sst.push_back(std::move(field));
        }

        return scene;
    }

    zmq::socket_t pull_;
};

} // namespace l3s
