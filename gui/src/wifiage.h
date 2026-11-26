#pragma once
#include <bit>
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
struct Range { int low, high, center, width, bandIndex; unsigned disabled = 0; };
inline bool vht80Center(double center) {
    return center == 5210 || center == 5290 || center == 5530 || center == 5610 ||
        center == 5690 || center == 5775 || center == 5855;
}
// Validate the helper's advertised contiguous interval independently. Never
// treat a primary-channel marker, arbitrary width, or 80+80 as a known range.
inline std::optional<Range> channelRange(double primary, double center, double width, bool modern = false) {
    const int b = band(primary);
    if (b < 0 || band(center) != b || (b == 2 && !modern)) return {};
    if (width != 20 && width != 40 && (b == 0 || (width != 80 && width != 160 && (b != 2 || width != 320)))) return {};
    if (b == 1 && width == 80 && !vht80Center(center)) return {};
    if (b == 1 && width == 160 && center != 5250 && center != 5570 && center != 5815) return {};
    if (modern && b == 0 && (primary < 2412 || primary > 2472 || std::fmod(primary - 2412, 5) != 0)) return {};
    if (modern && b == 1) {
        const bool lowBand = ((primary >= 5180 && primary <= 5320) || (primary >= 5500 && primary <= 5720))
            && std::fmod(primary, 20) == 0;
        const bool highBand = primary >= 5745 && primary <= 5885 && std::fmod(primary - 5745, 20) == 0;
        if (!lowBand && !highBand) return {};
    }
    if (b == 2 && !(primary == 5935 && center == 5935 && width == 20)) {
        if (primary < 5955 || std::fmod(primary - 5955, 20) != 0) return {};
        const double first = 5945 + width / 2, step = width == 320 ? 160 : width;
        if (center < first || std::fmod(center - first, step) != 0) return {};
    }
    const auto delta = std::abs(primary-center);
    if (delta > width/2-10 || std::fmod(width/2-10-delta,20) != 0) return {};
    const int lo = static_cast<int>(center-width/2), hi = static_cast<int>(center+width/2);
    if (band(lo) != b || band(hi) != b) return {};
    return Range{lo,hi,static_cast<int>(center),static_cast<int>(width),b};
}
inline bool validDisabledMask(const Range &range, int primary, unsigned mask) {
    if (mask == 0) return true;
    if (mask > 0xffff || primary < range.low + 10 || primary > range.high - 10
        || range.width > 320 || (mask & (1u << ((primary - range.low - 10) / 20)))) return false;
    const auto bits = std::popcount(mask);
    const auto pair = [](unsigned m) {
        return std::popcount(m) == 2 && std::countr_zero(m) % 2 == 0 && (m >> std::countr_zero(m)) == 3;
    };
    if (range.width == 80) return mask < 16 && bits == 1;
    if (range.width == 160) return mask < 256 && (bits == 1 || pair(mask));
    if (range.width != 320) return false;
    return pair(mask) || (bits == 4 && std::countr_zero(mask) % 4 == 0 && (mask >> std::countr_zero(mask)) == 15)
        || ((mask & 0x000f) == 0x000f && pair(mask & 0xfff0))
        || ((mask & 0xf000) == 0xf000 && pair(mask & 0x0fff));
}

template<class F>
void forEachActiveSpan(const Range &range, F &&fn) {
    if (!range.disabled) { fn(range.low, range.high); return; }
    int start = range.low;
    for (int i = 0; i < range.width / 20; ++i) {
        if (!(range.disabled & (1u << i))) continue;
        const int edge = range.low + i * 20;
        if (start < edge) fn(start, edge);
        start = edge + 20;
    }
    if (start < range.high) fn(start, range.high);
}

inline int overlapMHz(const Range &a, const Range &b) {
    if (a.bandIndex != b.bandIndex) return 0;
    int overlap = 0;
    forEachActiveSpan(a, [&](int aLo, int aHi) {
        forEachActiveSpan(b, [&](int bLo, int bHi) {
            const int lo = aLo > bLo ? aLo : bLo, hi = aHi < bHi ? aHi : bHi;
            if (hi > lo) overlap += hi - lo;
        });
    });
    return overlap;
}
struct Ranges { Range first; std::optional<Range> second; };
inline std::optional<Ranges> split80Ranges(double primary, double firstCenter, double secondCenter, bool modern = false) {
    if (band(firstCenter) != band(secondCenter) || std::abs(firstCenter-secondCenter) <= 80) return {};
    const auto first = channelRange(primary, firstCenter, 80, modern);
    const auto second = channelRange(secondCenter - 30, secondCenter, 80, modern);
    if (!first || !second) return {};
    return Ranges{*first, *second};
}
inline bool overlaps(const Ranges &a, const Ranges &b) {
    return overlapMHz(a.first, b.first) > 0 ||
        (b.second && overlapMHz(a.first, *b.second) > 0) ||
        (a.second && (overlapMHz(*a.second, b.first) > 0 ||
                      (b.second && overlapMHz(*a.second, *b.second) > 0)));
}
}
