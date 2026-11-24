#pragma once
// GUI-process registry. Tracks actual helper lifetime, not widget polling.
// This is not a cross-process hardware lock: root helpers keep their own locks.
#include "operationclaims.h"
#include <cstdint>
#include <chrono>
#include <map>
#include <deque>
#include <optional>
#include <string>
#include <utility>

namespace operations {
enum class Phase { Running, TimedOut, OwnerGone, Exited };
enum class Outcome { Unknown, ReportedOk, ReportedFailure, NotStarted, Conflict };
struct Entry { std::uint64_t id; std::string resource; Phase phase; Outcome outcome; std::chrono::system_clock::time_point time; std::vector<Claim> claims; bool lease = false; std::uint64_t owner = 0; };
class Registry {
    std::uint64_t next_ = 0;
    std::map<std::uint64_t, Entry> active_;
    std::deque<Entry> history_;
    void record(const Entry &e) { history_.push_back(e); if (history_.size() > 200) history_.pop_front(); }
public:
    // Called on the GUI thread. All claims are checked before any is acquired;
    // the entry owns them until finish(), including after timeout or owner loss.
    std::optional<std::uint64_t> tryBegin(std::string resource, RequestClaims request, std::uint64_t owner = 0) {
        const auto id = ++next_;
        Entry e{id, std::move(resource), Phase::Running, Outcome::Unknown, std::chrono::system_clock::now(), std::move(request.claims)};
        e.owner = owner;
        if (owner) {
            const auto it = active_.find(owner);
            bool valid = !e.claims.empty() && it != active_.end() && it->second.lease && it->second.phase == Phase::Running;
            if (valid) for (const auto &b : e.claims) {
                bool covered = false;
                for (const auto &a : it->second.claims)
                    if (a.resource == b.resource && (a.access == Access::Write || b.access == Access::Read)) covered = true;
                if (!covered) valid = false;
            }
            if (!valid) { e.phase=Phase::Exited; e.outcome=Outcome::Conflict; record(e); return std::nullopt; }
        }
        // A session's own operation bypasses only its lease, never a live helper.
        // Unowned emergency recovery retains the original override semantics.
        if (!request.recoveryOverride || owner) for (const auto &item : active_) if (item.first != owner) for (const auto &a : item.second.claims)
            for (const auto &b : e.claims) if (conflicts(a,b) && !(item.second.lease && b.access == Access::Read)) {
                e.phase=Phase::Exited; e.outcome=Outcome::Conflict; record(e); return std::nullopt;
            }
        active_.emplace(id, e); record(e); return id;
    }
    std::optional<std::uint64_t> tryLease(std::string resource, std::vector<Claim> claims) {
        if (claims.empty()) return std::nullopt;
        auto id = tryBegin(std::move(resource), {std::move(claims), false});
        if (id) { active_.at(*id).lease = true; history_.back().lease = true; }
        return id;
    }
    bool releaseLease(std::uint64_t id) {
        auto it = active_.find(id);
        if (it == active_.end() || !it->second.lease) return false;
        for (const auto &item : active_) if (item.second.owner == id) return false;
        auto e = it->second;
        e.phase = Phase::Exited; e.outcome = Outcome::Unknown;
        e.time = std::chrono::system_clock::now(); record(e); active_.erase(it);
        return true;
    }
    void abandonLease(std::uint64_t id) {
        const auto it = active_.find(id);
        if (it == active_.end() || !it->second.lease) return;
        transition(id, Phase::OwnerGone);
        releaseLease(id); // deferred until the owned process exits if still active
    }
    std::uint64_t begin(std::string resource) { return *tryBegin(std::move(resource),{}); }
    bool transition(std::uint64_t id, Phase phase) {
        auto it = active_.find(id);
        if (it == active_.end() || phase == Phase::Exited || phase == Phase::Running || it->second.phase == phase) return false;
        // A timeout is more important than the later disappearance of a widget.
        if (it->second.phase == Phase::TimedOut) return false;
        it->second.phase = phase; it->second.time = std::chrono::system_clock::now(); record(it->second); return true;
    }
    bool finish(std::uint64_t id, Outcome outcome) {
        auto it = active_.find(id); if (it == active_.end()) return false;
        if (it->second.lease) return false;
        auto e = it->second;
        if (e.phase == Phase::TimedOut) outcome = Outcome::Unknown;
        e.phase = Phase::Exited; e.outcome = outcome; e.time = std::chrono::system_clock::now(); record(e); active_.erase(it);
        if (e.owner) {
            const auto lease = active_.find(e.owner);
            if (lease != active_.end() && lease->second.phase == Phase::OwnerGone) releaseLease(e.owner);
        }
        return true;
    }
    const auto &active() const { return active_; }
    const auto &history() const { return history_; }
};
}
