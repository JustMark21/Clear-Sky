// Engine.hpp
//
// Implements the ACSPO L3S-LEO super-collation math from Section 2 of
// Jonasson, Gladkova, Ignatov & Kihai (2021), Proc. SPIE 11752, 1175202:
//
//   1. Local Clear-sky Ratio (LCR): mean, within an 11x11 sliding window,
//      of a binary clear(1)/cloudy(0) matrix -> LCR in [0,1]. (Sec. 2.1,
//      "The local clear-sky ratio (LCR) is defined as a mean within an
//      11x11 pixel sliding window ... of a matrix whose elements are 1
//      for clear-sky and 0 for cloudy conditions.")
//
//   2. Eq. (1) LCR-VZA weight:  w_i ∝ exp(-S_i / S0) * LCR_i^2,
//      S_i = sec(theta_i) - 1, S0 = 1.33 (empirically tuned in ACSPO
//      V2.80, used verbatim here).
//
//   3. Initial L3S reference ("LVW SST") = the Eq.(1)-weighted average of
//      all overpasses, replacing the earlier lowest-VZA (LVZA) composite,
//      which is also computed here for direct comparison (Fig. 3).
//
//   4. Iterative debiasing (Sec. 2.2): for 3 iterations with progressively
//      smaller windows (21x21 -> 11x11 -> 7x7), each overpass is debiased
//      against the current L3S reference by subtracting the local mean of
//      (overpass - reference) over the window, then overpasses are
//      recombined (LCR-VZA weighted) into the next L3S reference,
//      converging on the final L3S SST.
//
// All window statistics are computed via SummedAreaTable so every query,
// regardless of window size (21x21 down to 7x7), costs O(1).
#pragma once

#include "Grid.hpp"
#include "SummedAreaTable.hpp"
#include <vector>
#include <cmath>
#include <algorithm>
#include <limits>

namespace l3s {

struct EngineConfig {
    double S0 = 1.33;              // Eq. (1) VZA-preference constant (ACSPO V2.80 value)
    int lcrWindowRadius = 5;       // 11x11 window (paper, Sec. 2.1)
    std::vector<int> debiasRadii = {10, 5, 3}; // 21x21, 11x11, 7x7 (paper, Sec. 2.2)
};

struct EngineResult {
    Grid lvzaReference;          // legacy lowest-VZA composite (Fig. 3a)
    Grid lvwReference;           // Eq.(1) LCR-VZA weighted composite (Fig. 3b)
    std::vector<Grid> iterations; // L3S SST after each debiasing iteration (Fig. 8/9)
    const Grid& finalL3S() const { return iterations.back(); }
};

class L3SEngine {
public:
    explicit L3SEngine(EngineConfig cfg = {}) : cfg_(std::move(cfg)) {}

    EngineResult run(std::vector<Overpass>& overpasses) const {
        const int w = overpasses.front().sst.width;
        const int h = overpasses.front().sst.height;

        computeLCR(overpasses);
        computeWeights(overpasses);

        EngineResult result;
        result.lvzaReference = buildLvzaComposite(overpasses, w, h);
        result.lvwReference = buildWeightedComposite(overpasses, w, h, /*useSst=*/nullptr);

        Grid reference = result.lvwReference;
        result.iterations.reserve(cfg_.debiasRadii.size());
        for (int radius : cfg_.debiasRadii) {
            std::vector<Grid> debiased = debiasOverpasses(overpasses, reference, radius);
            Grid next = buildWeightedComposite(overpasses, w, h, &debiased);
            fillGaps(next, reference); // keep prior estimate where no data this iteration
            result.iterations.push_back(next);
            reference = next;
        }
        return result;
    }

private:
    EngineConfig cfg_;

    // --- Step: LCR, Eq.(1) -----------------------------------------------

    void computeLCR(std::vector<Overpass>& overpasses) const {
        for (auto& op : overpasses) {
            const int w = op.sst.width, h = op.sst.height;
            std::vector<float> clearMask(static_cast<size_t>(w) * h);
            for (size_t i = 0; i < clearMask.size(); ++i)
                clearMask[i] = op.sst.valid[i] ? 1.f : 0.f;

            SummedAreaTable sat;
            sat.build(clearMask, w, h); // no gating: denominator = full window area
            op.lcr = Grid(w, h);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    op.lcr.at(x, y) = static_cast<float>(sat.windowMean(x, y, cfg_.lcrWindowRadius, 0.0));
        }
    }

    void computeWeights(std::vector<Overpass>& overpasses) const {
        for (auto& op : overpasses) {
            const int w = op.sst.width, h = op.sst.height;
            op.weight = Grid(w, h);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    if (!op.sst.v(x, y)) { op.weight.at(x, y) = 0.f; continue; }
                    const double thetaRad = op.vza.at(x, y) * M_PI / 180.0;
                    const double S = 1.0 / std::cos(thetaRad) - 1.0; // sec(theta) - 1
                    const double lcr = op.lcr.at(x, y);
                    const double wgt = std::exp(-S / cfg_.S0) * lcr * lcr; // Eq. (1)
                    op.weight.at(x, y) = static_cast<float>(wgt);
                }
            }
        }
    }

    // --- Composites --------------------------------------------------------

    Grid buildLvzaComposite(const std::vector<Overpass>& overpasses, int w, int h) const {
        Grid out(w, h);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                double bestVza = std::numeric_limits<double>::infinity();
                float bestSst = NODATA;
                bool found = false;
                for (auto& op : overpasses) {
                    if (!op.sst.v(x, y)) continue;
                    const double vza = op.vza.at(x, y);
                    if (vza < bestVza) { bestVza = vza; bestSst = op.sst.at(x, y); found = true; }
                }
                out.at(x, y) = bestSst;
                out.v(x, y) = found ? 1 : 0;
            }
        }
        return out;
    }

    // Weighted average across overpasses using each overpass's Eq.(1) weight.
    // If `sstOverride` is provided (one grid per overpass, e.g. debiased
    // SSTs), those values are averaged instead of the raw op.sst values,
    // while weights/validity still come from the original overpass.
    Grid buildWeightedComposite(const std::vector<Overpass>& overpasses, int w, int h,
                                 const std::vector<Grid>* sstOverride) const {
        Grid out(w, h);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                double wsum = 0.0, vsum = 0.0;
                double simpleSum = 0.0; int simpleN = 0;
                for (size_t k = 0; k < overpasses.size(); ++k) {
                    const auto& op = overpasses[k];
                    if (!op.sst.v(x, y)) continue;
                    const float sst = sstOverride ? (*sstOverride)[k].at(x, y) : op.sst.at(x, y);
                    const double wgt = op.weight.at(x, y);
                    wsum += wgt;
                    vsum += wgt * sst;
                    simpleSum += sst; simpleN++;
                }
                if (wsum > 1e-9) {
                    out.at(x, y) = static_cast<float>(vsum / wsum);
                    out.v(x, y) = 1;
                } else if (simpleN > 0) {
                    // all weights collapsed (e.g. LCR=0 everywhere valid) -> plain mean fallback
                    out.at(x, y) = static_cast<float>(simpleSum / simpleN);
                    out.v(x, y) = 1;
                } else {
                    out.at(x, y) = NODATA;
                    out.v(x, y) = 0;
                }
            }
        }
        return out;
    }

    // --- Iterative debiasing (Sec. 2.2) ------------------------------------

    std::vector<Grid> debiasOverpasses(const std::vector<Overpass>& overpasses,
                                        const Grid& reference, int radius) const {
        const int w = reference.width, h = reference.height;
        std::vector<Grid> out;
        out.reserve(overpasses.size());

        for (auto& op : overpasses) {
            std::vector<float> diff(static_cast<size_t>(w) * h, 0.f);
            std::vector<uint8_t> ok(static_cast<size_t>(w) * h, 0);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = static_cast<size_t>(y) * w + x;
                    if (op.sst.v(x, y) && reference.v(x, y)) {
                        diff[i] = op.sst.at(x, y) - reference.at(x, y);
                        ok[i] = 1;
                    }
                }
            }
            SummedAreaTable sat;
            sat.build(diff, w, h, &ok); // gated: mean over valid (overpass & reference) pixels only

            Grid debiased(w, h);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    if (!op.sst.v(x, y)) { debiased.v(x, y) = 0; debiased.at(x, y) = NODATA; continue; }
                    const double localBias = sat.windowMean(x, y, radius, 0.0);
                    debiased.at(x, y) = static_cast<float>(op.sst.at(x, y) - localBias);
                    debiased.v(x, y) = 1;
                }
            }
            out.push_back(std::move(debiased));
        }
        return out;
    }

    static void fillGaps(Grid& g, const Grid& fallback) {
        for (size_t i = 0; i < g.data.size(); ++i) {
            if (!g.valid[i] && fallback.valid[i]) {
                g.data[i] = fallback.data[i];
                g.valid[i] = 1;
            }
        }
    }
};

} // namespace l3s
