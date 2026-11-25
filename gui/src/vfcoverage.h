#pragma once
// Which part of the NVIDIA V/F curve a session actually exercised. NVML reports the graphics clock, not the
// voltage; the GPU runs the lowest-voltage curve point that reaches the clock, so each clock sample is
// attributed to that point. Clocks above the curve's top count on the last point; idle clocks below the
// first point count on the first. Only points with samples are "observed": stability evidence covers them,
// not the whole curve.
#include <algorithm>
#include <vector>

namespace vfcoverage {

/// `curveMhz`: frequency of each point in voltage order (non-decreasing in a sane curve, not required).
inline int pointFor(const std::vector<double> &curveMhz, double clockMhz) {
    if (curveMhz.empty()) return -1;
    for (int i = 0; i < int(curveMhz.size()); ++i)
        if (curveMhz[i] >= clockMhz) return i;
    return int(curveMhz.size()) - 1;
}

struct Coverage {
    std::vector<int> counts;  // samples per point
    int samples = 0;
    int lowest() const { for (int i = 0; i < int(counts.size()); ++i) if (counts[i]) return i; return -1; }
    int highest() const { for (int i = int(counts.size()) - 1; i >= 0; --i) if (counts[i]) return i; return -1; }
    int observedPoints() const { return int(std::count_if(counts.begin(), counts.end(), [](int c) { return c > 0; })); }
};

inline Coverage add(Coverage c, const std::vector<double> &curveMhz, double clockMhz) {
    if (c.counts.size() != curveMhz.size()) c = Coverage{std::vector<int>(curveMhz.size(), 0), 0};
    if (const int i = pointFor(curveMhz, clockMhz); i >= 0 && clockMhz > 0) { ++c.counts[i]; ++c.samples; }
    return c;
}

} // namespace vfcoverage
