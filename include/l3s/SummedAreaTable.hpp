// SummedAreaTable.hpp
//
// Integral-image (summed-area table) box filter, giving O(1) mean/sum
// queries over an arbitrary rectangular window after an O(W*H) build pass.
//
// This is the workhorse used everywhere the paper calls for a local
// spatial statistic over an NxN sliding window:
//   - Local Clear-sky Ratio (LCR), Sec. 2.1: mean of a binary clear/cloud
//     mask over an 11x11 window.
//   - Iterative debiasing local-bias windows (21x21 -> 11x11 -> 7x7),
//     Sec. 2.2: mean of (overpass - reference) over a shrinking window.
//
// Without a SAT, an NxN box filter over a WxH grid costs O(W*H*N^2).
// With a SAT, the build is O(W*H) and every subsequent window query is
// O(1) regardless of N, which is what lets this engine treat 21x21 and
// 7x7 windows identically cheaply at full-grid scale.
#pragma once

#include <cstddef>
#include <vector>
#include <algorithm>
#include <cmath>

namespace l3s {

class SummedAreaTable {
public:
    SummedAreaTable() = default;

    // Build the integral image of `data` (row-major, width x height).
    // `valid` (optional, same shape) masks which pixels contribute; pixels
    // with valid==false contribute 0 to both the value-sum and the
    // count-sum, which lets windowMean() correctly average only over
    // valid samples (e.g. only clear-sky, or only pixels with data).
    void build(const std::vector<float>& data, int width, int height,
               const std::vector<uint8_t>* valid = nullptr) {
        width_ = width;
        height_ = height;
        // (W+1) x (H+1) padded integral tables, standard SAT trick so
        // window sums never need boundary special-casing beyond clamping.
        sum_.assign(static_cast<size_t>(width + 1) * (height + 1), 0.0);
        cnt_.assign(static_cast<size_t>(width + 1) * (height + 1), 0.0);

        for (int y = 0; y < height; ++y) {
            double rowSum = 0.0;
            double rowCnt = 0.0;
            for (int x = 0; x < width; ++x) {
                const size_t idx = static_cast<size_t>(y) * width + x;
                const bool ok = valid == nullptr || (*valid)[idx] != 0;
                const double v = ok ? static_cast<double>(data[idx]) : 0.0;
                rowSum += v;
                rowCnt += ok ? 1.0 : 0.0;
                const size_t here = sat_idx(x + 1, y + 1);
                const size_t up = sat_idx(x + 1, y);
                sum_[here] = sum_[up] + rowSum;
                cnt_[here] = cnt_[up] + rowCnt;
            }
        }
    }

    // Sum of valid values inside the inclusive window centered at (cx, cy)
    // with half-size `radius` (i.e. a (2*radius+1)^2 box), clamped to the
    // grid boundary. Returns {sum, validCount}.
    inline std::pair<double, double> windowSum(int cx, int cy, int radius) const {
        const int x0 = std::max(0, cx - radius);
        const int y0 = std::max(0, cy - radius);
        const int x1 = std::min(width_ - 1, cx + radius);
        const int y1 = std::min(height_ - 1, cy + radius);
        // Inclusion-exclusion over the padded integral image: O(1).
        const double s = sum_[sat_idx(x1 + 1, y1 + 1)] - sum_[sat_idx(x0, y1 + 1)]
                        - sum_[sat_idx(x1 + 1, y0)] + sum_[sat_idx(x0, y0)];
        const double c = cnt_[sat_idx(x1 + 1, y1 + 1)] - cnt_[sat_idx(x0, y1 + 1)]
                        - cnt_[sat_idx(x1 + 1, y0)] + cnt_[sat_idx(x0, y0)];
        return {s, c};
    }

    // Mean of valid values in the window; returns `fallback` if no valid
    // samples fall in the window (e.g. fully cloud-obscured neighborhood).
    inline double windowMean(int cx, int cy, int radius, double fallback = 0.0) const {
        auto [s, c] = windowSum(cx, cy, radius);
        return c > 0.0 ? s / c : fallback;
    }

    int width() const { return width_; }
    int height() const { return height_; }

private:
    inline size_t sat_idx(int x, int y) const {
        return static_cast<size_t>(y) * (width_ + 1) + x;
    }

    int width_ = 0;
    int height_ = 0;
    std::vector<double> sum_;
    std::vector<double> cnt_;
};

} // namespace l3s
