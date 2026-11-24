#pragma once
// Metadata only: do not open a sysfs sensor to establish its identity.
#include <sys/stat.h>
#include <optional>
namespace sensorincarnation {
struct Identity {
    dev_t device; ino_t inode;
    bool operator==(const Identity &) const = default;
};
inline std::optional<Identity> identify(const char *path) {
    struct stat s{};
    if (::stat(path,&s) != 0 || !S_ISDIR(s.st_mode)) return {};
    return Identity{s.st_dev,s.st_ino};
}
inline bool same(const char *path, const std::optional<Identity> &expected) {
    return expected && identify(path) == expected;
}
}
