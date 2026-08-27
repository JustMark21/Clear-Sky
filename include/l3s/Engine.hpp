#pragma once

#include "l3s/SummedAreaTable.hpp"
#include "l3s/Simulator.hpp"

#include <cmath>
#include <limits>
#include <vector>

namespace l3s {

// S0: e-folding scale of the atmospheric slant-path attenuation term in
// Eq. (1) below.
constexpr double kS0 = 1.33;

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

// Eq. (1): w_i ∝ exp(-S_i / S0) * LCR_i^2, where S_i = sec(VZA_i) - 1 is
// the atmospheric slant-path length relative to nadir. Larger view angle
// -> longer path -> more attenuation -> lower weight; lower LCR -> more
// cloud contamination nearby -> lower weight (squared, so it penalizes
// harder than the VZA term). Pixels the overpass didn't observe get
// weight 0 outright, never a fabricated small positive weight.
inline std::vector<float> computeWeights(const Overpass& op, const std::vector<float>& lcr, double s0 = kS0) {
    std::vector<float> w(op.sst.size());
    for (size_t i = 0; i < w.size(); ++i) {
        if (std::isnan(op.sst[i])) {
            w[i] = 0.0f;
            continue;
        }
        const double vzaRad = op.vza[i] * M_PI / 180.0;
        const double s = 1.0 / std::cos(vzaRad) - 1.0; // sec(VZA) - 1
        const double lcr2 = static_cast<double>(lcr[i]) * lcr[i];
        w[i] = static_cast<float>(std::exp(-s / s0) * lcr2);
    }
    return w;
}

// Naive lowest-view-zenith-angle composite: at each pixel, take the SST
// value from whichever overpass observed it at the smallest VZA among
// those where the pixel is valid. Pixels invalid in every overpass stay
// NODATA. This is a hard per-pixel selection, not a blend -- overpass
// boundaries can show up as visible seams in the composite. Kept as a
// legacy reference to compare against the weighted composite below.
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

// Continuous Eq. (1) blend: out = sum(w_i * sst_i) / sum(w_i) over every
// overpass that observed the pixel, so every valid observation
// contributes in proportion to its weight instead of one overpass
// winning outright.
inline std::vector<float> buildWeightedComposite(const std::vector<Overpass>& overpasses,
                                                   const std::vector<std::vector<float>>& weights,
                                                   int width, int height) {
    std::vector<float> out(static_cast<size_t>(width) * height, NODATA);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t idx = static_cast<size_t>(y) * width + x;

            double wsum = 0.0, vsum = 0.0;
            for (size_t k = 0; k < overpasses.size(); ++k) {
                const float sst = overpasses[k].sst[idx];
                if (std::isnan(sst)) continue;
                const double w = weights[k][idx];
                wsum += w;
                vsum += w * sst;
            }
            if (wsum > 1e-9) {
                out[idx] = static_cast<float>(vsum / wsum);
            }
        }
    }
    return out;
}

} // namespace l3s
