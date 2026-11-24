#pragma once
// Evidence only, never permission to write. Exact kHz comparison matches the
// Rust CT verifier. A projected frequency is not an observed frequency.
#include <cstdint>
#include <cstddef>
#include <utility>
#include <map>
#include <optional>
#include <vector>
namespace curveplan {
struct Point { int index; std::int64_t voltage, base, delta, projected; };
using Layout = std::vector<std::pair<int,std::int64_t>>;
inline std::optional<std::map<int,Point>> validate(const Layout &layout, const std::vector<Point> &points) {
    if (layout.empty() || layout.size() > 255 || points.size() != layout.size()) return {};
    std::map<int,Point> result;
    for (std::size_t i=0;i<points.size();++i) {
        const auto &p=points[i];
        if (p.index < 0 || p.index > 254 || p.voltage < 0 || p.voltage > 10000000 ||
            p.base < 0 || p.base > 100000000 || p.delta < -100000000 || p.delta > 100000000 ||
            p.projected < 0 || p.projected > 100000000 || p.base+p.delta != p.projected ||
            layout[i] != std::pair{p.index,p.voltage} || !result.emplace(p.index,p).second) return {};
    }
    return result;
}
}
