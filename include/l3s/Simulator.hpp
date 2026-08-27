#pragma once

#include <cmath>
#include <cstddef>
#include <limits>
#include <random>
#include <vector>

namespace l3s {

constexpr float NODATA = std::numeric_limits<float>::quiet_NaN();

struct SimulatorConfig {
    int width = 64;
    int height = 64;
    double baselineKelvin = 288.15;   // ~15C open-ocean background
    double blobAmplitudeKelvin = 6.0; // peak warm-core excess over baseline
    double blobSigmaPx = 12.0;        // gaussian spread of the warm core

    int numCloudBlobs = 4;
    double cloudRadiusMinPx = 4.0;
    double cloudRadiusMaxPx = 10.0;
    unsigned seed = 42;
};

// Produces a synthetic sea-surface-temperature field: a flat baseline
// with one gaussian warm core that drifts in a slow circular path over
// time, then overlaid with randomly placed circular cloud blobs whose
// pixels are marked NODATA (unobservable, not a temperature of zero).
class Simulator {
public:
    explicit Simulator(SimulatorConfig cfg) : cfg_(cfg), rng_(cfg.seed) {}

    std::vector<float> generate(double t) {
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

        applyCloudMask(field);
        return field;
    }

private:
    void applyCloudMask(std::vector<float>& field) {
        std::uniform_real_distribution<double> cxDist(0.0, cfg_.width);
        std::uniform_real_distribution<double> cyDist(0.0, cfg_.height);
        std::uniform_real_distribution<double> rDist(cfg_.cloudRadiusMinPx, cfg_.cloudRadiusMaxPx);

        for (int b = 0; b < cfg_.numCloudBlobs; ++b) {
            const double bx = cxDist(rng_);
            const double by = cyDist(rng_);
            const double br = rDist(rng_);
            const double br2 = br * br;

            for (int y = 0; y < cfg_.height; ++y) {
                for (int x = 0; x < cfg_.width; ++x) {
                    const double dx = x - bx;
                    const double dy = y - by;
                    if (dx * dx + dy * dy <= br2) {
                        field[static_cast<size_t>(y) * cfg_.width + x] = NODATA;
                    }
                }
            }
        }
    }

    SimulatorConfig cfg_;
    std::mt19937 rng_;
};

} // namespace l3s
