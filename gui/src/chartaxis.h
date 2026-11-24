#pragma once
// Presentation-only finite axis math. Long double avoids overflow when the
// endpoints are opposite extreme doubles and preserves subnormal ranges.
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace chartaxis {
struct Range {
    double low, high;
    static std::optional<Range> make(double a, double b) {
        if (!std::isfinite(a) || !std::isfinite(b)) return {};
        return Range{std::min(a,b), std::max(a,b)};
    }
    double fraction(double v) const {
        if (!std::isfinite(v)) return std::numeric_limits<double>::quiet_NaN();
        if (high == low) return v < low ? 0.0 : v > high ? 1.0 : 0.5;
        if (v <= low) return 0;
        if (v >= high) return 1;
        return double((static_cast<long double>(v)-low)/(static_cast<long double>(high)-low));
    }
    std::vector<double> ticks(unsigned divisions = 4) const {
        if (low == high) return {low};
        divisions = std::clamp(divisions, 1u, 12u);
        std::vector<double> out{low};
        for (unsigned i=1; i<divisions; ++i) {
            const auto x = double(std::lerp(static_cast<long double>(low), static_cast<long double>(high),
                                            static_cast<long double>(i)/divisions));
            if (x > out.back() && x < high) out.push_back(x);
        }
        out.push_back(high);
        return out;
    }
};
}
