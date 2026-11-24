#pragma once
// Best-effort identity of a sysfs directory incarnation, never a hardware serial.
// Canonical containment also rejects detached, cyclic and escaping fixture links.
#include <filesystem>
#include <optional>
#include <string>
#include <sys/stat.h>
namespace deviceidentity {
struct Stamp {
    std::string path, incarnation;
    bool operator==(const Stamp &) const = default;
};
inline std::optional<Stamp> capture(const std::filesystem::path &devices, const std::filesystem::path &alias) {
    std::error_code ec;
    const auto base = std::filesystem::canonical(devices, ec);
    if (ec) return {};
    const auto target = std::filesystem::canonical(alias, ec);
    if (ec) return {};
    const auto relative = target.lexically_relative(base);
    if (relative.empty() || relative == "." || relative.is_absolute() || *relative.begin() == "..") return {};
    struct stat st{};
    if (::stat(target.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return {};
    // ctime is intentionally not used: sysfs attribute changes are not a replacement.
    // Inode reuse is possible; this is evidence of continuity, not proof of physical identity.
    return Stamp{relative.string(), std::to_string(st.st_dev) + ":" + std::to_string(st.st_ino)};
}
inline bool mayReadLink(const std::string &runtime) { return runtime == "active"; }
} // namespace deviceidentity
