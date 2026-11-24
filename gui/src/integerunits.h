#pragma once
// Canonical byte values. No floating-point round-trip, no implicit unit guessing.
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

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
}
