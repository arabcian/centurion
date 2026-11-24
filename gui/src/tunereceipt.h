#pragma once
#include <cstdint>
namespace tunereceipt {
enum class Status { Unknown, Failed, Partial, Skipped, Unverified, Mismatch, Match };
struct Evidence {
    bool valid = false, ok = false, skipped = false;
    std::int64_t written = -1, refused = -1, expected = -1, matched = -1, missing = -1, mismatched = -1;
};
inline Status classify(const Evidence &e) {
    if (!e.valid || e.refused < 0) return Status::Unknown;
    if (!e.ok) return e.written > 0 ? Status::Partial : Status::Failed;
    if (e.skipped) return e.written > 0 ? Status::Unknown : Status::Skipped;
    if (e.mismatched > 0) return Status::Mismatch;
    if (e.refused > 0) return e.written > 0 ? Status::Partial : Status::Unverified;
    // Absence of mismatch is not proof: older helpers omit verification counts.
    if (e.expected <= 0 || e.matched < 0 || e.missing < 0 || e.mismatched != 0 || e.written < 0 ||
        e.matched > e.expected || e.missing > e.expected - e.matched || e.matched + e.missing != e.expected ||
        e.written > e.expected || e.missing != 0) return Status::Unverified;
    return Status::Match;
}
}
