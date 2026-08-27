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

    int numOverpasses = 4;
    double maxVzaDeg = 60.0; // view zenith angle at the swath edge

    unsigned seed = 42;
};

// One satellite pass over the scene: a retrieved SST field (Kelvin,
// NODATA where cloud-obscured) and the per-pixel view zenith angle
// (degrees) that pass observed each pixel at.
struct Overpass {
    std::vector<float> sst;
    std::vector<float> vza;
};

// Produces `numOverpasses` independent passes over the same underlying
// scene: a flat Kelvin baseline with one gaussian warm core that drifts
// along a slow circular path over time. Each pass gets its own
// cross-track scan geometry (a nadir ground-track line at a different
// angle/offset, with VZA increasing linearly with perpendicular distance
// from it) and its own independently placed cloud blobs.
class Simulator {
public:
    explicit Simulator(SimulatorConfig cfg) : cfg_(cfg), rng_(cfg.seed) {}

    std::vector<Overpass> generateOverpasses(double t) {
        std::vector<Overpass> overpasses;
        overpasses.reserve(cfg_.numOverpasses);

        for (int k = 0; k < cfg_.numOverpasses; ++k) {
            Overpass op;
            op.sst = generateTrueField(t);
            op.vza = computeVza(k, t);
            applyCloudMask(op.sst);
            overpasses.push_back(std::move(op));
        }
        return overpasses;
    }

private:
    std::vector<float> generateTrueField(double t) const {
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

    // Nadir ground track k is a line through the grid at angle
    // (pi/numOverpasses)*k, slowly rotating with t, offset perpendicular
    // to itself so different passes don't all cross through the center.
    // VZA grows linearly with perpendicular distance from that line and
    // saturates at maxVzaDeg.
    std::vector<float> computeVza(int k, double t) const {
        std::vector<float> vza(static_cast<size_t>(cfg_.width) * cfg_.height);

        const double angle = (M_PI / cfg_.numOverpasses) * k + 0.15 * t;
        const double perpOffset =
            (k - (cfg_.numOverpasses - 1) / 2.0) * (cfg_.width / static_cast<double>(cfg_.numOverpasses));
        const double px = cfg_.width * 0.5 + perpOffset * std::cos(angle + M_PI / 2.0);
        const double py = cfg_.height * 0.5 + perpOffset * std::sin(angle + M_PI / 2.0);

        const double satDist = 0.5 * std::max(cfg_.width, cfg_.height);
        const double gainDegPerPx = cfg_.maxVzaDeg / satDist;

        for (int y = 0; y < cfg_.height; ++y) {
            for (int x = 0; x < cfg_.width; ++x) {
                const double dist = std::abs((x - px) * std::sin(angle) - (y - py) * std::cos(angle));
                const double deg = std::min(cfg_.maxVzaDeg, dist * gainDegPerPx);
                vza[static_cast<size_t>(y) * cfg_.width + x] = static_cast<float>(deg);
            }
        }
        return vza;
    }

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
