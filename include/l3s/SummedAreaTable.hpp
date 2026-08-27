#pragma once

#include <algorithm>
#include <vector>

namespace l3s {

// Integral image over a width x height grid of values: after O(width *
// height) construction, the sum (or mean) of any axis-aligned rectangular
// window costs O(1) regardless of the window's size.
class SummedAreaTable {
public:
    SummedAreaTable(const std::vector<float>& values, int width, int height)
        : width_(width), height_(height), sat_(static_cast<size_t>(width + 1) * (height + 1), 0.0) {
        for (int y = 0; y < height; ++y) {
            double rowSum = 0.0;
            for (int x = 0; x < width; ++x) {
                rowSum += values[static_cast<size_t>(y) * width + x];
                sat_[static_cast<size_t>(y + 1) * (width_ + 1) + (x + 1)] =
                    sat_[static_cast<size_t>(y) * (width_ + 1) + (x + 1)] + rowSum;
            }
        }
    }

    // Sum of values within the half-open window [x0,x1) x [y0,y1),
    // clamped to the grid bounds.
    double boxSum(int x0, int y0, int x1, int y1) const {
        x0 = clampX(x0);
        x1 = clampX(x1);
        y0 = clampY(y0);
        y1 = clampY(y1);
        if (x1 <= x0 || y1 <= y0) return 0.0;
        return sat_[idx(x1, y1)] - sat_[idx(x1, y0)] - sat_[idx(x0, y1)] + sat_[idx(x0, y0)];
    }

    // Mean of the (2*radius+1) x (2*radius+1) window centered on
    // (cx, cy), clamped at the grid edges (the denominator shrinks to
    // match, so edge pixels are not diluted by out-of-bounds zeros).
    double boxMean(int cx, int cy, int radius) const {
        const int x0 = cx - radius, x1 = cx + radius + 1;
        const int y0 = cy - radius, y1 = cy + radius + 1;
        const int cx0 = clampX(x0), cx1 = clampX(x1);
        const int cy0 = clampY(y0), cy1 = clampY(y1);
        const double area = static_cast<double>(cx1 - cx0) * (cy1 - cy0);
        if (area <= 0.0) return 0.0;
        return boxSum(x0, y0, x1, y1) / area;
    }

private:
    size_t idx(int x, int y) const { return static_cast<size_t>(y) * (width_ + 1) + x; }
    int clampX(int x) const { return std::min(std::max(x, 0), width_); }
    int clampY(int y) const { return std::min(std::max(y, 0), height_); }

    int width_;
    int height_;
    std::vector<double> sat_;
};

} // namespace l3s
