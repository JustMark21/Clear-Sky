// Grid.hpp — a minimal row-major float tensor with an optional validity mask.
#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <string>

namespace l3s {

constexpr float NODATA = std::numeric_limits<float>::quiet_NaN();

struct Grid {
    int width = 0;
    int height = 0;
    std::vector<float> data;   // SST [K], row-major
    std::vector<uint8_t> valid; // 1 = has a retrieval (clear-sky), 0 = cloud/gap

    Grid() = default;
    Grid(int w, int h) : width(w), height(h), data(static_cast<size_t>(w) * h, 0.f),
                          valid(static_cast<size_t>(w) * h, 0) {}

    inline size_t idx(int x, int y) const { return static_cast<size_t>(y) * width + x; }
    inline float& at(int x, int y) { return data[idx(x, y)]; }
    inline float at(int x, int y) const { return data[idx(x, y)]; }
    inline uint8_t& v(int x, int y) { return valid[idx(x, y)]; }
    inline uint8_t v(int x, int y) const { return valid[idx(x, y)]; }
};

// One simulated LEO overpass: SST field + VZA field + validity/cloud mask.
struct Overpass {
    std::string name;
    double avgVZA = 0.0;
    Grid sst;
    Grid vza;      // view zenith angle, degrees, per-pixel
    Grid lcr;      // local clear-sky ratio, computed by the engine
    Grid weight;   // Eq. (1) weight, computed by the engine
};

} // namespace l3s
