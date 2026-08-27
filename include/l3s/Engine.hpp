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

// Debiasing window radii, largest first: 21x21 -> 11x11 -> 7x7. Starting
// wide catches broad, slowly-varying bias; each subsequent, smaller pass
// refines the correction locally without re-introducing noise from a
// window too small to average over.
constexpr int kDebiasWindowRadii[3] = {10, 5, 3};

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

// Windowed mean of `diff` restricted to pixels where `validMask` is 1,
// via two summed-area tables: one over the (zeroed-where-invalid) diff
// values, one over the validity mask itself, so the denominator is the
// true count of contributing pixels rather than the raw window area.
inline std::vector<float> windowedMaskedMean(const std::vector<float>& diff, const std::vector<float>& validMask,
                                              int width, int height, int windowRadius) {
    const SummedAreaTable diffSat(diff, width, height);
    const SummedAreaTable countSat(validMask, width, height);

    std::vector<float> mean(static_cast<size_t>(width) * height, 0.0f);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int x0 = x - windowRadius, x1 = x + windowRadius + 1;
            const int y0 = y - windowRadius, y1 = y + windowRadius + 1;
            const double count = countSat.boxSum(x0, y0, x1, y1);
            const size_t idx = static_cast<size_t>(y) * width + x;
            mean[idx] = count > 0.5 ? static_cast<float>(diffSat.boxSum(x0, y0, x1, y1) / count) : 0.0f;
        }
    }
    return mean;
}

// Iterative debiasing: over 3 passes with shrinking windows (21x21 ->
// 11x11 -> 7x7), each overpass's SST is compared against an unweighted
// per-pixel consensus of every overpass valid at that pixel, and its
// local windowed mean offset from that consensus is subtracted out --
// suppressing the systematic, slowly-varying VZA-driven cold bias beyond
// what Eq. (1)'s per-pixel weighting alone corrects for. Modifies
// `overpasses` in place; a pixel invalid in an overpass is left alone.
inline void debiasOverpasses(std::vector<Overpass>& overpasses, int width, int height) {
    const size_t n = static_cast<size_t>(width) * height;

    for (int windowRadius : kDebiasWindowRadii) {
        std::vector<float> refSum(n, 0.0f), refCount(n, 0.0f);
        for (const auto& op : overpasses) {
            for (size_t i = 0; i < n; ++i) {
                if (std::isnan(op.sst[i])) continue;
                refSum[i] += op.sst[i];
                refCount[i] += 1.0f;
            }
        }
        std::vector<float> reference(n, NODATA);
        for (size_t i = 0; i < n; ++i) {
            if (refCount[i] > 0.0f) reference[i] = refSum[i] / refCount[i];
        }

        for (auto& op : overpasses) {
            std::vector<float> diff(n, 0.0f), validMask(n, 0.0f);
            for (size_t i = 0; i < n; ++i) {
                if (std::isnan(op.sst[i]) || std::isnan(reference[i])) continue;
                diff[i] = op.sst[i] - reference[i];
                validMask[i] = 1.0f;
            }

            const auto bias = windowedMaskedMean(diff, validMask, width, height, windowRadius);
            for (size_t i = 0; i < n; ++i) {
                if (std::isnan(op.sst[i])) continue;
                op.sst[i] -= bias[i];
            }
        }
    }
}

// Naive lowest-view-zenith-angle composite: at each pixel, take the SST
// value from whichever overpass observed it at the smallest VZA among
// those where the pixel is valid. Pixels invalid in every overpass stay
// NODATA -- `bestSst` is only ever overwritten by an observed value, so
// there is no division and no way to leak a garbage number here. Kept as
// a legacy reference to compare against the weighted composite below.
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
// overpass that observed the pixel. Hardened against the case where a
// pixel is technically observed but every weight collapses near zero
// (extreme VZA everywhere it was seen): falls back to a plain unweighted
// mean of the valid observations rather than dividing by a near-zero
// sum. Only a pixel with zero valid observations in every overpass stays
// NODATA -- never a divide-by-zero, never a fabricated value.
inline std::vector<float> buildWeightedComposite(const std::vector<Overpass>& overpasses,
                                                   const std::vector<std::vector<float>>& weights,
                                                   int width, int height) {
    std::vector<float> out(static_cast<size_t>(width) * height, NODATA);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t idx = static_cast<size_t>(y) * width + x;

            double wsum = 0.0, vsum = 0.0;
            double simpleSum = 0.0;
            int simpleN = 0;
            for (size_t k = 0; k < overpasses.size(); ++k) {
                const float sst = overpasses[k].sst[idx];
                if (std::isnan(sst)) continue;
                const double w = weights[k][idx];
                wsum += w;
                vsum += w * sst;
                simpleSum += sst;
                simpleN++;
            }

            if (wsum > 1e-9) {
                out[idx] = static_cast<float>(vsum / wsum);
            } else if (simpleN > 0) {
                out[idx] = static_cast<float>(simpleSum / simpleN);
            }
        }
    }
    return out;
}

} // namespace l3s
