#pragma once

#include "l3s/Simulator.hpp"

#include <cmath>
#include <limits>
#include <vector>

namespace l3s {

// Naive lowest-view-zenith-angle composite: at each pixel, take the SST
// value from whichever overpass observed it at the smallest VZA among
// those where the pixel is valid. Pixels invalid in every overpass stay
// NODATA. This is a hard per-pixel selection, not a blend -- overpass
// boundaries can show up as visible seams in the composite.
inline std::vector<float> buildLvzaComposite(const std::vector<Overpass>& overpasses, int width, int height) {
    std::vector<float> out(static_cast<size_t>(width) * height, NODATA);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t idx = static_cast<size_t>(y) * width + x;

            float bestVza = std::numeric_limits<float>::infinity();
            float bestSst = NODATA;
            for (const auto& op : overpasses) {
                const float sst = op.sst[idx];
                if (std::isnan(sst)) continue;
                const float vza = op.vza[idx];
                if (vza < bestVza) {
                    bestVza = vza;
                    bestSst = sst;
                }
            }
            out[idx] = bestSst;
        }
    }
    return out;
}

} // namespace l3s
