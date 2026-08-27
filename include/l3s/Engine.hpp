#pragma once

#include "l3s/SummedAreaTable.hpp"
#include "l3s/Simulator.hpp"

#include <cmath>
#include <limits>
#include <vector>

namespace l3s {

// Local Clear-sky Ratio: the fraction of clear (non-NODATA) pixels within
// an (2*windowRadius+1) x (2*windowRadius+1) window centered on each
// pixel, in [0, 1]. A pixel can pass the cloud mask itself yet still sit
// on a cloud boundary -- LCR captures that by scoring the neighborhood,
// not just the pixel. windowRadius=5 gives the paper's 11x11 window.
inline std::vector<float> computeLCR(const std::vector<float>& sst, int width, int height, int windowRadius = 5) {
    std::vector<float> clearMask(static_cast<size_t>(width) * height);
    for (size_t i = 0; i < clearMask.size(); ++i) {
        clearMask[i] = std::isnan(sst[i]) ? 0.0f : 1.0f;
    }

    const SummedAreaTable sat(clearMask, width, height);
    std::vector<float> lcr(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            lcr[static_cast<size_t>(y) * width + x] = static_cast<float>(sat.boxMean(x, y, windowRadius));
        }
    }
    return lcr;
}

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
