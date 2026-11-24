#pragma once
// Local stat identity, not authentication or a content hash. Does not open an
// archive, follow its final symlink, or confer restore authorization.
#include <optional>
#include <string>
#include <sys/stat.h>
namespace archiveidentity {
inline std::optional<std::string> read(const char *path) {
    struct stat s{};
    if (::lstat(path, &s) != 0 || !S_ISREG(s.st_mode) || s.st_size < 0) return {};
    return std::to_string(s.st_dev) + ":" + std::to_string(s.st_ino) + ":" + std::to_string(s.st_size) + ":" +
        std::to_string(s.st_mtim.tv_sec) + ":" + std::to_string(s.st_mtim.tv_nsec) + ":" +
        std::to_string(s.st_ctim.tv_sec) + ":" + std::to_string(s.st_ctim.tv_nsec);
}
}
