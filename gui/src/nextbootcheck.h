#pragma once
// Values written for the next boot (BIOS CPU overclocking, MUX, ...) checked against what the firmware
// reports once that boot has happened. Requested and read-back values are kept side by side, so a value the
// firmware limited is visible on the first start after the reboot instead of looking "applied".
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace nextbootcheck {

enum class State { Pending, Matched, Limited, Unreadable };

struct Row { std::string key; int64_t requested; std::optional<int64_t> readBack; State state; };

/// `savedBoot`: boot id when the values were written; `currentBoot`: this boot's id.
/// Same boot (or an unknown id) = still pending: the firmware applies them at the next start.
inline std::vector<Row> check(const std::map<std::string, int64_t> &requested, const std::string &savedBoot,
                              const std::string &currentBoot, const std::map<std::string, int64_t> &readBack) {
    std::vector<Row> rows;
    const bool rebooted = !savedBoot.empty() && !currentBoot.empty() && savedBoot != currentBoot;
    for (const auto &[key, want] : requested) {
        Row r{key, want, std::nullopt, State::Pending};
        if (const auto it = readBack.find(key); it != readBack.end()) r.readBack = it->second;
        if (rebooted) r.state = !r.readBack ? State::Unreadable : *r.readBack == want ? State::Matched : State::Limited;
        rows.push_back(r);
    }
    return rows;
}

/// The pending record can be dropped once every row was seen after the reboot (matched, limited or unreadable).
inline bool settled(const std::vector<Row> &rows) {
    for (const auto &r : rows) if (r.state == State::Pending) return false;
    return true;
}

} // namespace nextbootcheck
