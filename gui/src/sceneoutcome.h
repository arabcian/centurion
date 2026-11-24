#pragma once
#include <string_view>

namespace sceneoutcome {
// Successful callbacks do not establish independently verified hardware state.
constexpr std::string_view terminal(bool ok, bool gameOwns, bool incomplete, bool deferred) {
    if (gameOwns) return "interrupted";
    if (!ok || incomplete) return "partial";
    if (deferred) return "deferred_pending";
    return "returned_unverified";
}
}
