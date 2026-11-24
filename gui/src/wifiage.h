#pragma once
#include <cmath>
#include <cstdint>
#include <optional>
#include <limits>

namespace wifiage {
using Tick = std::uint64_t;
// Restrict conversion to integers exactly representable in JSON numbers.
inline std::optional<Tick> integer(double value) {
    if (!std::isfinite(value) || value < 0 || value > 9007199254740991.0 || std::floor(value) != value) return {};
    return static_cast<Tick>(value);
}
inline std::optional<Tick> elapsed(std::optional<Tick> received, std::optional<Tick> now) {
    if (!received || !now || *now < *received) return {};
    return *now - *received;
}
// Kernel reported age at dump + elapsed since reception is a lower bound:
// helper/IPC latency is not included. Missing clock/age remains unknown.
inline std::optional<Tick> age(std::optional<Tick> seen, std::optional<Tick> sinceReceipt) {
    if (!seen || !sinceReceipt || *seen > std::numeric_limits<Tick>::max() - *sinceReceipt) return {};
    return *seen + *sinceReceipt;
}
inline constexpr Tick staleMs = 60000;
inline bool validSignal(double value) { return std::isfinite(value) && value >= -127 && value <= 0; }
inline int band(double freq) {
    if (!std::isfinite(freq) || std::floor(freq) != freq) return -1;
    if (freq >= 2400 && freq <= 2500) return 0;
    if (freq >= 5150 && freq <= 5895) return 1;
    if (freq >= 5925 && freq <= 7125) return 2;
    return -1;
}
}

namespace wifiage {
struct Age {
    std::optional<Tick> ms;
    bool observed = false; // Kernel last-seen CLOCK_BOOTTIME (ms resolution).
};
// Dump completion is measured in the helper's local CLOCK_BOOTTIME domain.
// Old helpers without that field fall back to receipt. Never substitute UTC.
inline Age ageFromSnapshot(std::optional<Tick> seen, std::optional<Tick> observed,
                           std::optional<Tick> dump, std::optional<Tick> receipt,
                           std::optional<Tick> now) {
    if (!receipt || !now || *receipt > *now || (dump && *dump > *receipt) ||
        (observed && (*observed > *receipt || (dump && *observed > *dump)))) return {};
    const bool dumpValid = dump.has_value();
    if (observed && *observed <= *receipt && (!dump || (dumpValid && *observed <= *dump)))
        return {*now - *observed, true};
    return {age(seen, elapsed(dumpValid ? dump : receipt, now)), false};
}
struct Range { int low, high, center, width, bandIndex; };
// Validate the helper's advertised contiguous interval independently. Never
// treat a primary-channel marker, arbitrary width, or 80+80 as a known range.
inline std::optional<Range> channelRange(double primary, double center, double width) {
    const int b = band(primary);
    if (b < 0 || b == 2 || band(center) != b || !std::isfinite(width)) return {};
    if (width != 20 && width != 40 && (b != 1 || (width != 80 && width != 160))) return {};
    const auto delta = std::abs(primary-center);
    if (delta > width/2-10 || std::fmod(width/2-10-delta,20) != 0) return {};
    const int lo = static_cast<int>(center-width/2), hi = static_cast<int>(center+width/2);
    if (band(lo) != b || band(hi) != b) return {};
    return Range{lo,hi,static_cast<int>(center),static_cast<int>(width),b};
}
inline int overlapMHz(const Range &a, const Range &b) {
    if (a.bandIndex != b.bandIndex) return 0;
    const int lo = a.low > b.low ? a.low : b.low, hi = a.high < b.high ? a.high : b.high;
    return hi > lo ? hi-lo : 0;
}
}
