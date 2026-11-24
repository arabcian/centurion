#pragma once
// Historical presentation only. Never an authorization to apply a preset.
#include <cmath>
#include <optional>
namespace abverify {
struct Pair { double first, second; };
inline bool finite(Pair p) { return std::isfinite(p.first) && std::isfinite(p.second); }
inline bool validEstimate(Pair p) { return finite(p) && p.second >= 0; }
inline bool validInterval(Pair p) { return finite(p) && p.first <= p.second; }
enum class Status { Unknown, Inconsistent, Incomplete, NotPassed, ReportedPass };
struct Evidence {
    bool schema = false, paired = false, complete = false, passed = false, ended = false;
    bool validFlags = false, validContext = false;
    int rounds = -1, skipped = -1, presetOoms = -1;
    std::optional<Pair> gain, decision;
};
inline Status status(const Evidence &e) {
    if (!e.schema || !e.validFlags || !e.paired || e.rounds < 0 || e.skipped < 0 || e.presetOoms < 0)
        return Status::Unknown;
    const bool sufficient = e.rounds >= 3 && e.skipped == 0 && !e.ended;
    const bool measured = e.gain && validEstimate(*e.gain) && e.decision && validInterval(*e.decision);
    // The helper's recorded flags are not substitutes for its actual required evidence.
    if (e.complete != sufficient || (e.passed && (!sufficient || !measured || e.presetOoms != 0 || e.decision->first <= 0)))
        return Status::Inconsistent;
    if (!sufficient || !measured) return Status::Incomplete;
    if (!e.validContext) return Status::Unknown;
    return e.passed ? Status::ReportedPass : Status::NotPassed;
}
}
