#pragma once
// Presentation-only identity checks; no permission or socket mutation authority.
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <limits>
namespace processidentity {
inline std::optional<uint64_t> number(std::string_view value) {
    if (value.empty()) return {};
    uint64_t n{};
    const auto r = std::from_chars(value.data(), value.data()+value.size(), n);
    if (r.ec != std::errc{} || r.ptr != value.data()+value.size()) return {};
    return n;
}
inline std::optional<uint64_t> startTicks(std::string_view stat, uint64_t pid) {
    const auto l=stat.find(" ("), r=stat.rfind(')');
    if (pid==0 || l==stat.npos || r==stat.npos || r<=l+1 || stat.substr(r+1,1)!=" " || number(stat.substr(0,l))!=pid) return {};
    auto tail=stat.substr(r+2);
    for (int i=0;i<19;++i) {
        const auto sep=tail.find(' ');
        if (sep==tail.npos || sep==0) return {};
        tail.remove_prefix(sep+1);
    }
    return number(tail.substr(0,tail.find_first_of(" \n")));
}
inline bool matches(std::string_view boot, std::string_view currentBoot, std::string_view expectedTicks,
                    std::optional<uint64_t> observedTicks, std::string_view exe, std::string_view observedExe) {
    const auto expected=number(expectedTicks);
    return !boot.empty() && boot==currentBoot && expected && observedTicks && *expected==*observedTicks &&
        !exe.empty() && exe==observedExe;
}
inline bool fresh(uint64_t started, uint64_t now, uint64_t maxAge) { return now>=started && now-started<=maxAge; }
}
