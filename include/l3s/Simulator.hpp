#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

namespace l3s {

struct SimulatorConfig {
    int width = 64;
    int height = 64;
    double baselineKelvin = 288.15;   // ~15C open-ocean background
    double blobAmplitudeKelvin = 6.0; // peak warm-core excess over baseline
    double blobSigmaPx = 12.0;        // gaussian spread of the warm core
};

// Produces a single synthetic sea-surface-temperature field: a flat
// baseline with one gaussian warm core that drifts in a slow circular
// path over time, in Kelvin.
class Simulator {
public:
    explicit Simulator(SimulatorConfig cfg) : cfg_(cfg) {}

    std::vector<float> generate(double t) const {
        std::vector<float> field(static_cast<size_t>(cfg_.width) * cfg_.height);

        const double cx = cfg_.width * 0.5 + cfg_.width * 0.25 * std::cos(t * 0.2);
        const double cy = cfg_.height * 0.5 + cfg_.height * 0.25 * std::sin(t * 0.2);
        const double twoSigma2 = 2.0 * cfg_.blobSigmaPx * cfg_.blobSigmaPx;

        for (int y = 0; y < cfg_.height; ++y) {
            for (int x = 0; x < cfg_.width; ++x) {
                const double dx = x - cx;
                const double dy = y - cy;
                const double r2 = dx * dx + dy * dy;
                const double warm = cfg_.blobAmplitudeKelvin * std::exp(-r2 / twoSigma2);
                field[static_cast<size_t>(y) * cfg_.width + x] =
                    static_cast<float>(cfg_.baselineKelvin + warm);
            }
        }
        return field;
    }

private:
    SimulatorConfig cfg_;
};

} // namespace l3s
