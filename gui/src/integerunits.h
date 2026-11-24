#pragma once
// Canonical byte values. No floating-point round-trip, no implicit unit guessing.
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <string>

namespace integerunits {
inline std::optional<std::int64_t> bytes(std::string_view text) {
    const auto trim = [](std::string_view s) {
        const auto begin = s.find_first_not_of(" \t");
        return begin == s.npos ? std::string_view{} : s.substr(begin, s.find_last_not_of(" \t") - begin + 1);
    };
    text = trim(text);
    if (text.empty()) return std::nullopt;
    std::int64_t number = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    if (parsed.ec != std::errc{} || number < 0) return std::nullopt;
    const auto suffix = trim(text.substr(static_cast<std::size_t>(parsed.ptr - text.data())));
    std::int64_t multiplier = 1;
    if (suffix == "KiB") multiplier = 1024;
    else if (suffix == "MiB") multiplier = 1024 * 1024;
    else if (suffix == "GiB") multiplier = 1024LL * 1024 * 1024;
    else if (suffix == "TiB") multiplier = 1024LL * 1024 * 1024 * 1024;
    else if (!suffix.empty() && suffix != "B") return std::nullopt;
    if (number > std::numeric_limits<std::int64_t>::max() / multiplier) return std::nullopt;
    return number * multiplier;
}
// Unit exponents are relative to nanoseconds. Only explicitly documented
// settings opt in; do not infer from a label or key suffix.
enum class TimeUnit { None = -1, Ns = 0, Us = 3, Ms = 6, Seconds = 9 };
inline constexpr std::string_view unitName(TimeUnit unit) {
    switch (unit) {
    case TimeUnit::Ns: return "ns";
    case TimeUnit::Us: return "us";
    case TimeUnit::Ms: return "ms";
    case TimeUnit::Seconds: return "s";
    default: return {};
    }
}
inline constexpr TimeUnit timeUnitForKey(std::string_view key) {
    for (auto k : {"sched.base_slice_ns", "sched.min_base_slice_ns", "sched.migration_cost_ns",
                   "sched.rseq_slice_ext_nsec", "blk.kyber_read_lat_ns", "blk.kyber_write_lat_ns",
                   "net.gro_flush_timeout"})
        if (key == k) return TimeUnit::Ns;
    for (auto k : {"cpu.wake_latency_us", "cpu.wake_latency_ccd0", "cpu.wake_latency_ccd1",
                   "cpu.schedutil_rate_limit_us", "kernel.cfs_bandwidth_slice_us",
                   "kernel.sched_rt_runtime_us", "net.netdev_budget_usecs", "blk.wbt_lat_usec",
                   "pm.nvme_latency_us", "pm.nvme1_latency_us", "pm.nvme2_latency_us",
                   "net.busy_poll", "net.busy_read"})
        if (key == k) return TimeUnit::Us;
    for (auto k : {"thp.khp_scan_sleep_ms", "thp.khp_alloc_sleep_ms", "sched.numa_scan_period_min_ms",
                   "sched.numa_scan_period_max_ms", "sched.numa_scan_delay_ms", "sched.numa_hot_threshold_ms",
                   "kernel.sched_rr_timeslice_ms", "net.mt76_idle_timeout", "sched.mc_min_interval",
                   "sched.mc_max_interval", "sched.pkg_min_interval", "sched.pkg_max_interval",
                   "blk.dl_read_expire_ms", "blk.dl_write_expire_ms", "blk.bfq_slice_idle_ms",
                   "usb.autosuspend_ms", "pm.ahci_runtime_timeout"})
        if (key == k) return TimeUnit::Ms;
    for (auto k : {"vm.stat_interval", "snd.hda_power_save", "usb.autosuspend", "mce.check_interval"})
        if (key == k) return TimeUnit::Seconds;
    return TimeUnit::None;
}
inline std::optional<std::int64_t> time(std::string_view text, TimeUnit canonical) {
    if (unitName(canonical).empty() || text.size() > 128) return std::nullopt;
    const auto trim = [](std::string_view s) {
        const auto begin = s.find_first_not_of(" \t");
        return begin == s.npos ? std::string_view{} : s.substr(begin, s.find_last_not_of(" \t") - begin + 1);
    };
    text = trim(text);
    if (text.empty()) return std::nullopt;
    const auto exactInteger = [](std::string_view s) -> std::optional<std::int64_t> {
        if (s.empty()) return std::nullopt;
        std::int64_t n = 0;
        const auto result = std::from_chars(s.data(), s.data()+s.size(), n);
        if (result.ec != std::errc{} || result.ptr != s.data()+s.size()) return std::nullopt;
        return n;
    };
    // Bare integers retain existing canonical semantics, including -1 sentinels.
    if (auto n = exactInteger(text)) return n;
    const auto split = text.find_first_not_of("0123456789.");
    if (split == text.npos || split == 0) return std::nullopt;
    const auto magnitude = text.substr(0, split);
    const auto suffix = trim(text.substr(split));
    TimeUnit input = TimeUnit::None;
    for (auto u : {TimeUnit::Ns, TimeUnit::Us, TimeUnit::Ms, TimeUnit::Seconds})
        if (suffix == unitName(u)) input = u;
    if (input == TimeUnit::None) return std::nullopt;
    const auto point = magnitude.find('.');
    if (point == 0 || (point != magnitude.npos && (point+1 == magnitude.size() || magnitude.find('.', point+1) != magnitude.npos))) return std::nullopt;
    std::string digits(magnitude);
    int fraction = 0;
    if (point != magnitude.npos) {
        fraction = static_cast<int>(magnitude.size()-point-1);
        digits.erase(point, 1);
    }
    const auto first = digits.find_first_not_of('0');
    if (first == digits.npos) return 0;
    digits.erase(0, first);
    // Decimal shift, not an intermediate nanosecond integer: even an INT64_MAX
    // value in seconds is representable when the canonical unit is seconds.
    const int shift = static_cast<int>(input)-static_cast<int>(canonical)-fraction;
    if (shift < 0) {
        const auto remove = static_cast<std::size_t>(-shift);
        if (remove >= digits.size() || digits.find_last_not_of('0') >= digits.size()-remove) return std::nullopt;
        digits.resize(digits.size()-remove);
    } else {
        if (digits.size()+static_cast<std::size_t>(shift) > 19) return std::nullopt;
        digits.append(static_cast<std::size_t>(shift), '0');
    }
    return exactInteger(digits);
}
}
