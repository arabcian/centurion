#pragma once
#include <string_view>

namespace sceneoutcome {
constexpr bool canSkipOptimizationFailure(bool reached, bool explicitlyNotApplied, std::string_view reason) {
    return reached && explicitlyNotApplied && reason == "storage_identity_unavailable";
}

// This is a reporting contract, not a change to scene apply/rollback policy.
// Successful callbacks do not establish independently verified hardware state.
constexpr std::string_view terminal(bool ok, bool rollback, bool compensated,
                                    bool gameOwns, bool incomplete, bool deferred) {
    if (gameOwns) return "interrupted";
    if (rollback) return compensated ? "compensated_unverified" : "partial_rollback";
    if (!ok || incomplete) return "partial";
    if (deferred) return "deferred_pending";
    return "returned_unverified";
}
}
