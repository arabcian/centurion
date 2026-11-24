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
