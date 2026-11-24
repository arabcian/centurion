#pragma once
// Strict, bounded Linux cpulist and amdgpu DPM readers for presentation only.
#include <algorithm>
#include <charconv>
#include <optional>
#include <string_view>
#include <vector>
#include <set>

namespace topologyfields {
inline std::string_view trim(std::string_view s) {
    constexpr std::string_view ws = " \t\r\n";
    const auto first = s.find_first_not_of(ws);
    if (first == s.npos) return {};
    return s.substr(first, s.find_last_not_of(ws) - first + 1);
}
inline std::optional<int> integer(std::string_view s, int max = 1048575) {
    if (s.empty() || s.size() > 10) return {};
    int n = 0;
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (ec != std::errc{} || p != s.data() + s.size() || n < 0 || n > max || s.front() == '-') return {};
    return n;
}
inline std::optional<std::vector<int>> cpuList(std::string_view s, size_t maxCount = 65536) {
    s = trim(s);
    maxCount = std::min<size_t>(maxCount, 65536);
    if (s.empty() || s.size() > 65536) return {};
    std::set<int> ids;
    for (;;) {
        const auto comma = s.find(',');
        const auto field = s.substr(0, comma);
        const auto dash = field.find('-');
        const auto a = integer(field.substr(0, dash));
        const auto b = dash == field.npos ? a : integer(field.substr(dash + 1));
        if (!a || !b || *b < *a || size_t(*b - *a + 1) > maxCount - ids.size()) return {};
        for (int n = *a; n <= *b; ++n) if (!ids.insert(n).second) return {};
        if (comma == s.npos) break;
        s.remove_prefix(comma + 1);
    }
    return std::vector<int>(ids.begin(), ids.end());
}
struct Cpu { int id; std::optional<int> package, core, die; std::optional<std::vector<int>> siblings; };
// Only symmetric, same-package/core/die sibling declarations establish a group.
// A partial/offline member without topology is not silently inferred.
inline bool sameCore(const Cpu &a, const Cpu &b) {
    return a.package && a.core && a.package == b.package && a.core == b.core && a.die == b.die &&
        a.siblings && b.siblings && *a.siblings == *b.siblings &&
        std::binary_search(a.siblings->begin(), a.siblings->end(), a.id) &&
        std::binary_search(a.siblings->begin(), a.siblings->end(), b.id);
}
struct Dpm { int index, mhz; bool active; };
inline std::optional<std::vector<Dpm>> dpm(std::string_view s) {
    if (s.size() > 65536) return {};
    std::vector<Dpm> out;
    std::set<int> ids;
    bool active = false;
    while (!s.empty()) {
        const auto newline = s.find('\n');
        auto line = trim(s.substr(0, newline));
        if (newline == s.npos) s = {}; else s.remove_prefix(newline + 1);
        if (line.empty()) continue;
        const auto colon = line.find(':');
        if (colon == line.npos) return {};
        const auto idx = integer(trim(line.substr(0, colon)), 65535);
        line = trim(line.substr(colon + 1));
        bool marked = !line.empty() && line.back() == '*';
        if (marked) { line.remove_suffix(1); line = trim(line); }
        // Only real frequency units; PCIe link speed and unsupported/unknown strings are not clocks.
        if (!line.ends_with("Mhz") && !line.ends_with("MHz") && !line.ends_with("mhz")) return {};
        line.remove_suffix(3);
        const auto mhz = integer(trim(line), 1000000);
        if (!idx || !mhz || !ids.insert(*idx).second || out.size() >= 256 || (marked && active)) return {};
        active |= marked;
        out.push_back({*idx, *mhz, marked});
    }
    if (out.empty()) return {};
    return out;
}
} // namespace topologyfields
