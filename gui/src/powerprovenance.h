#pragma once
// Presentation-only strict firmware metadata and PMU evidence. Never authorizes a write.
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include "topologyfields.h"
namespace powerprovenance {
inline std::optional<std::int64_t> integer(std::string_view s) {
    s = topologyfields::trim(s);
    if (s.empty() || s.size() > 20) return {};
    std::int64_t v{};
    const auto [end, ec] = std::from_chars(s.data(), s.data()+s.size(), v);
    if (ec != std::errc{} || end != s.data()+s.size()) return {};
    return v;
}
struct Range {
    std::int64_t min, max, step;
    bool contains(std::int64_t value) const { return value >= min && value <= max; }
};
inline std::optional<Range> range(std::string_view lo, std::string_view hi, std::string_view step) {
    const auto a=integer(lo), b=integer(hi), c=integer(step);
    if (!a || !b || !c || *a > *b || *c <= 0) return {};
    return Range{*a,*b,*c};
}
enum class PmuKind { Unknown, Core, Atom, Conflict };
inline PmuKind pmuKind(int cpu, const std::optional<std::vector<int>> &core,
                        const std::optional<std::vector<int>> &atom) {
    // Require both valid lists: a missing PMU is not proof of a homogeneous CPU.
    if (!core || !atom) return PmuKind::Unknown;
    const bool p=std::binary_search(core->begin(),core->end(),cpu);
    const bool e=std::binary_search(atom->begin(),atom->end(),cpu);
    if (p && e) return PmuKind::Conflict;
    return p ? PmuKind::Core : e ? PmuKind::Atom : PmuKind::Unknown;
}
}
