// Simulator.hpp
//
// Synthetic multi-overpass SST scene generator, standing in for real
// ACSPO L3U granules. Produces exactly the kind of scene the paper's
// worked examples (Figs. 2, 6, 8) are built around:
//   - a smooth macro-scale "true" SST field carrying at least one strong,
//     wide thermal front (paper: Agulhas Current front, ~3.5 K jump),
//   - several overpasses (Metop-A/B/C style: A1, B1, A2, C1) each with a
//     spatially varying view-zenith-angle (VZA) field mimicking swath
//     geometry (low near nadir/track center, high near swath edges),
//   - per-overpass VZA-dependent atmospheric-path bias (paper Sec. 2.1),
//   - macro-level cloud gaps PLUS residual "cloud leakage" -- pixels that
//     pass the clear-sky mask but are still a few K too cold, exactly the
//     failure mode Sec. 2 is built to suppress.
//
// Each call to generate() advances an internal clock so consecutive calls
// drift (moving front, drifting clouds), giving the live telemetry loop
// something dynamic to show frame over frame.
#pragma once

#include "Grid.hpp"
#include <cmath>
#include <random>
#include <vector>
#include <string>
#include <limits>

namespace l3s {

struct SimulatorConfig {
    int width = 480;
    int height = 320;
    int numOverpasses = 4;     // e.g. A1, B1, A2, C1
    int numCloudBlobs = 14;    // macro-level cloud gaps
    double cloudBlobRadiusFrac = 0.09; // as a fraction of min(width,height)
    unsigned seed = 42;

    // Probability [0,1] that any given storm is "persistent": pinned to
    // the EXACT same position across every overpass in a scene (no
    // per-overpass jitter -- see jitterStorms()), guaranteeing pixels
    // that are 100% cloud-obscured in all overpasses simultaneously,
    // every frame. Default 0.0 preserves prior visual behavior; set e.g.
    // 0.15 to reliably exercise Engine.hpp's all-invalid fallback path
    // (buildWeightedComposite's wsum==0 branch, buildLvzaComposite's
    // found==false branch) for testing, rather than hoping independent
    // per-overpass jitter coincidentally overlaps.
    double persistentStormProbability = 0.0;
};

class Simulator {
public:
    explicit Simulator(const SimulatorConfig& cfg)
        : cfg_(cfg), rng_(cfg.seed), baseStorms_(buildStorms()) {}

    // Generates one multi-overpass scene at simulated time `t` (arbitrary
    // monotonically increasing units, e.g. frame index). Fronts drift and
    // clouds advect slowly with t so consecutive frames are correlated.
    std::vector<Overpass> generate(double t) {
        std::vector<Overpass> overpasses;
        overpasses.reserve(cfg_.numOverpasses);

        static const char* names[] = {"A1", "B1", "A2", "C1", "D1", "E1"};
        std::uniform_real_distribution<double> jitter(-1.0, 1.0);

        for (int k = 0; k < cfg_.numOverpasses; ++k) {
            Overpass op;
            op.name = names[k % 6];
            // Spread overpass acquisition/geometry over the collation window.
            const double phase = static_cast<double>(k) / cfg_.numOverpasses;
            op.sst = Grid(cfg_.width, cfg_.height);
            op.vza = Grid(cfg_.width, cfg_.height);

            // Swath center drifts left-to-right across the grid per overpass,
            // VZA grows with distance from the swath center line (like a
            // real cross-track swath geometry).
            const double swathCenterX = cfg_.width * (0.15 + 0.7 * phase);
            const double maxVza = 20.0 + 55.0 * (0.3 + 0.7 * std::fabs(std::sin(k * 1.7 + t * 0.05)));

            // Cloud systems are largely shared across overpasses (real
            // clouds don't reshuffle between consecutive passes within the
            // 24h collation window) but each overpass sees them from a
            // slightly different moment, so we jitter the shared storm
            // field per-overpass. This produces both (a) genuine full
            // gaps where a storm core is cloudy in every overpass, and
            // (b) large-scale cloud-leakage rings where the storm's edge
            // shifts just enough that a "clear" pixel in one overpass
            // sits inside the cloud-contaminated boundary of another --
            // exactly the failure mode Sec. 2.1 describes.
            std::vector<Storm> storms = jitterStorms(baseStorms_, t + (k * 80.0), k);

            double vzaSum = 0.0;
            for (int y = 0; y < cfg_.height; ++y) {
                for (int x = 0; x < cfg_.width; ++x) {
                    const double trueSST = trueField(x, y, t);

                    const double dx = (x - swathCenterX);
                    const double vza = std::min(85.0, maxVza * std::fabs(dx) / (cfg_.width * 0.5));
                    op.vza.at(x, y) = static_cast<float>(vza);
                    vzaSum += vza;

                    // VZA-dependent atmospheric-path bias: longer path at
                    // high VZA => small systematic cold/warm offset, the
                    // physical cause of inter-overpass bias (Sec. 2.1).
                    const double secTheta = 1.0 / std::cos(vza * M_PI / 180.0);
                    const double atmBias = -0.35 * (secTheta - 1.0) + 0.05 * jitterAt(k, x, y);

                    double sst = trueSST + atmBias;

                    const bool cloudy = isCloudy(x, y, storms);
                    uint8_t valid = 1;
                    if (cloudy) {
                        valid = 0; // masked out entirely (a true cloud gap)
                    } else {
                        // Residual cloud leakage: pixels near a cloud
                        // boundary pass the mask but retain 1-3 K cold bias
                        // -- the exact failure mode motivating LCR weighting.
                        const double proximity = cloudProximity(x, y, storms);
                        if (proximity > 0.0) {
                            sst -= (1.0 + 2.0 * proximity) * std::max(0.0, 1.0 - proximity * 0.3);
                        }
                    }

                    op.sst.at(x, y) = static_cast<float>(sst);
                    op.sst.v(x, y) = valid;
                }
            }
            op.avgVZA = vzaSum / (cfg_.width * cfg_.height);
            overpasses.push_back(std::move(op));
        }
        return overpasses;
    }

private:
    // Macro-scale "true" SST field: smooth background gradient + a strong
    // wide thermal front (tanh step, paper-scale ~3-4 K) + gentle eddies,
    // all drifting slowly with t.
    double trueField(int x, int y, double t) const {
        const double nx = static_cast<double>(x) / cfg_.width;
        const double ny = static_cast<double>(y) / cfg_.height;

        const double base = 288.0 + 6.0 * ny; // warmer toward one edge (latitude proxy)

        // Wide meandering thermal front (Agulhas-Current-like), amplitude ~3.6K
        const double frontX = 0.45 + 0.06 * std::sin(ny * 6.0 + t * 0.03) + 0.02 * std::sin(t * 0.017);
        const double front = 1.8 * std::tanh((nx - frontX) * 14.0);

        // Slowly rotating macro eddies for visual/physical texture.
        const double eddy = 0.6 * std::sin(nx * 9.0 + t * 0.02) * std::cos(ny * 7.0 - t * 0.015);

        return base + front + eddy;
    }

    double jitterAt(int k, int x, int y) const {
        // Cheap deterministic pseudo-noise (no per-pixel RNG allocation cost).
        uint32_t h = static_cast<uint32_t>(x * 374761393 + y * 668265263 + k * 2246822519u);
        h = (h ^ (h >> 13)) * 1274126177u;
        h ^= (h >> 16);
        return (static_cast<double>(h & 0xFFFF) / 65535.0) * 2.0 - 1.0;
    }

    // A "storm" is a macro-scale organic cloud system: a primary disc plus
    // a few overlapping lobe discs, so cloud gaps read as amoeba-shaped
    // weather systems rather than perfect circles.
    struct Lobe { double dx, dy, r; };
    struct Storm {
        double cx, cy;           // slowly-orbiting center (base position)
        double orbitR, angle0, speed, speed2;
        double r;                // primary disc radius
        std::vector<Lobe> lobes; // relative sub-discs, fixed shape
        bool persistent = false; // if true: identical position across all
                                  // overpasses in a scene (see jitterStorms)
    };

    std::vector<Storm> buildStorms() const {
        std::mt19937 local(cfg_.seed + 777u);
        std::uniform_real_distribution<double> u01(0.0, 1.0);
        std::uniform_real_distribution<double> uAngle(0.0, 2.0 * M_PI);
        std::uniform_real_distribution<double> uSpeed(0.006, 0.02);

        std::vector<Storm> storms;
        storms.reserve(cfg_.numCloudBlobs);
        const double baseR = cloudBlobRadius();
        for (int i = 0; i < cfg_.numCloudBlobs; ++i) {
            Storm s;
            s.cx = u01(local) * cfg_.width;
            s.cy = u01(local) * cfg_.height;
            s.orbitR = 8.0 + u01(local) * 0.15 * std::min(cfg_.width, cfg_.height);
            s.angle0 = uAngle(local);
            s.speed = uSpeed(local);
            s.speed2 = uSpeed(local) * (u01(local) > 0.5 ? 1.0 : -1.0);
            s.r = baseR * (0.7 + 1.3 * u01(local));
            s.persistent = (u01(local) < cfg_.persistentStormProbability);
            const int nLobes = 2 + static_cast<int>(u01(local) * 3.0); // 2-4 lobes
            for (int j = 0; j < nLobes; ++j) {
                Lobe lobe;
                lobe.dx = (u01(local) - 0.5) * s.r * 1.6;
                lobe.dy = (u01(local) - 0.5) * s.r * 1.6;
                lobe.r = s.r * (0.45 + 0.4 * u01(local));
                s.lobes.push_back(lobe);
            }
            storms.push_back(std::move(s));
        }
        return storms;
    }

    // Advance storm centers along their orbit at time t, then apply a
    // small per-overpass offset so consecutive overpasses see mostly (but
    // not exactly) the same cloud field -- this is what produces both true
    // multi-overpass gaps (storm cores) and cloud-leakage rings (storm
    // edges disagreeing between overpasses).
    std::vector<Storm> jitterStorms(const std::vector<Storm>& base, double t, int k) const {
        std::vector<Storm> out = base;
        for (auto& s : out) {
            // Orbit motion depends only on t (shared across all overpasses
            // in the same scene, since they're all generated at the same
            // t), not on k -- it's already common ground. The jitter term
            // below is what normally breaks that agreement per-overpass.
            const double orbitedX = s.cx + s.orbitR * std::cos(s.angle0 + s.speed * t);
            const double orbitedY = s.cy + s.orbitR * std::sin(s.angle0 + s.speed2 * t);
            if (s.persistent) {
                // No per-overpass jitter: every k lands on the exact same
                // (cx, cy), so this storm's full disc+lobe footprint masks
                // the identical pixels in every overpass -- a guaranteed,
                // not merely probable, all-4-clouded region.
                s.cx = orbitedX;
                s.cy = orbitedY;
                continue;
            }
            const double jitterAmp = s.r * 0.12;
            s.cx = orbitedX + jitterAmp * std::cos(k * 2.1 + t * 0.11 + s.angle0);
            s.cy = orbitedY + jitterAmp * std::sin(k * 1.7 + t * 0.09 + s.angle0);
        }
        return out;
    }

    static double discSignedDist(double x, double y, double cx, double cy, double r) {
        return std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) - r;
    }

    // Signed distance to the nearest storm boundary (primary disc + lobes),
    // negative = inside a cloud.
    double distanceToNearestCloud(int x, int y, const std::vector<Storm>& storms) const {
        double best = std::numeric_limits<double>::infinity();
        for (auto& s : storms) {
            best = std::min(best, discSignedDist(x, y, s.cx, s.cy, s.r));
            for (auto& l : s.lobes)
                best = std::min(best, discSignedDist(x, y, s.cx + l.dx, s.cy + l.dy, l.r));
        }
        return best;
    }

    bool isCloudy(int x, int y, const std::vector<Storm>& storms) const {
        return distanceToNearestCloud(x, y, storms) < 0.0;
    }

    // 0 outside cloud-influence band, ->1 approaching a cloud edge from
    // the clear side (simulates residual cloud-adjacency leakage).
    double cloudProximity(int x, int y, const std::vector<Storm>& storms) const {
        const double d = distanceToNearestCloud(x, y, storms);
        const double band = cloudBlobRadius() * 0.5;
        if (d < 0.0 || d >= band) return 0.0;
        return 1.0 - d / band;
    }

    double cloudBlobRadius() const {
        return cfg_.cloudBlobRadiusFrac * std::min(cfg_.width, cfg_.height);
    }

    SimulatorConfig cfg_;
    std::mt19937 rng_;
    std::vector<Storm> baseStorms_;
};

} // namespace l3s
