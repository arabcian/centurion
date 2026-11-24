#pragma once
// Existing lspci -nnk capture only; omit free-form descriptions.
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>
#include <utility>
namespace structuredpci {
struct Device { std::string address, classCode, vendor, device, driver, modules; };
inline std::optional<std::vector<Device>> parse(const std::vector<std::string> &lines) {
    if (lines.empty() || lines.size() > 4096) return std::nullopt;
    static const std::regex header(R"(^((?:[0-9A-Fa-f]{4}:)?[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}\.[0-7]) .+ \[([0-9A-Fa-f]{4})\]: .+ \[([0-9A-Fa-f]{4}):([0-9A-Fa-f]{4})\](?: \(rev [0-9A-Fa-f]{2}\))?$)");
    static const std::regex subsystem(R"(^Subsystem: .+ \[[0-9A-Fa-f]{4}:[0-9A-Fa-f]{4}\]$)");
    static const std::regex driver(R"(^Kernel driver in use: ([A-Za-z0-9_.-]+)$)");
    static const std::regex modules(R"(^Kernel modules: ([A-Za-z0-9_.-]+(?:, [A-Za-z0-9_.-]+)*)$)");
    static const std::regex slot(R"(^Physical Slot: [A-Za-z0-9_.:-]+$)");
    std::vector<Device> result; std::set<std::string> seen;
    bool hadSubsystem = false, hadSlot = false;
    auto lower = [](std::string s) { for (char &c : s) if (c >= 'A' && c <= 'F') c = char(c - 'A' + 'a'); return s; };
    for (const auto &line : lines) {
        if (line.empty() || line.size() > 1024 || line.find('\0') != std::string::npos) return std::nullopt;
        std::smatch m;
        if (std::regex_match(line, m, header)) {
            auto addr = lower(m[1].str());
            const auto key = addr.size() == 7 ? "0000:" + addr : addr;
            if (!seen.insert(key).second) return std::nullopt;
            result.push_back({addr, lower(m[2].str()), lower(m[3].str()), lower(m[4].str()), {}, {}});
            hadSubsystem = hadSlot = false;
        } else {
            if (result.empty()) return std::nullopt;
            auto &d = result.back();
            if (std::regex_match(line, m, driver)) {
                if (!d.driver.empty()) return std::nullopt;
                d.driver = m[1].str();
            } else if (std::regex_match(line, m, modules)) {
                if (!d.modules.empty()) return std::nullopt;
                d.modules = m[1].str();
            } else if (std::regex_match(line, m, subsystem)) {
                if (hadSubsystem) return std::nullopt;
                hadSubsystem = true;
            } else if (std::regex_match(line, m, slot)) {
                if (hadSlot) return std::nullopt;
                hadSlot = true;
            } else return std::nullopt;
        }
    }
    return result.empty() ? std::nullopt : std::optional<std::vector<Device>>(std::move(result));
}
}
