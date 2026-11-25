#pragma once
// Per-component summary of one scene apply plus what triggered it.
// Presentation only: "applied" means the helper returned success, not an independent read-back.
#include <string>
#include <vector>

namespace sceneapplysummary {

enum class Bucket { Applied, Failed, Skipped, Deferred, Running, Unknown };

inline Bucket bucket(const std::string &state) {
    if (state == "returned_ok" || state == "returned_unverified" || state == "deferred_returned_unverified") return Bucket::Applied;
    if (state == "failed" || state == "deferred_failed" || state == "soft_failure") return Bucket::Failed;
    if (state == "skipped" || state == "deferred_skipped" || state == "deferred_superseded") return Bucket::Skipped;
    if (state == "deferred_pending" || state == "deferred_running") return Bucket::Deferred;
    if (state == "running") return Bucket::Running;
    return Bucket::Unknown;
}

struct Component { std::string step, state, detail; };

// Latest state per step, first-seen order kept. A later event for the same step replaces the earlier one,
// so "running" → "failed" ends as failed, and a deferred GPU curve's final state wins over "deferred_pending".
class Summary {
public:
    void record(const std::string &step, const std::string &state, const std::string &detail = {}) {
        if (step.empty()) return;
        for (auto &c : items_) if (c.step == step) { c.state = state; c.detail = detail; return; }
        items_.push_back({step, state, detail});
    }
    const std::vector<Component> &items() const { return items_; }
    int count(Bucket b) const {
        int n = 0;
        for (const auto &c : items_) n += bucket(c.state) == b;
        return n;
    }
    // A failure of one component does not hide the components that did go through.
    bool mixed() const { return count(Bucket::Failed) > 0 && count(Bucket::Applied) > 0; }
private:
    std::vector<Component> items_;
};

// Trigger text stored with the apply. `source`: "manual", "tray", "shortcut", "dbus", "power", "startup",
// "resume", "unpause", "game", ...; `detail` is free text (e.g. "AC → battery"). Never empty.
inline std::string trigger(const std::string &source, const std::string &detail) {
    const std::string s = source.empty() ? std::string("unknown") : source;
    return detail.empty() ? s : s + ": " + detail;
}

} // namespace sceneapplysummary
