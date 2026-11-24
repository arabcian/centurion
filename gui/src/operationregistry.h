#pragma once
// GUI-process registry. Tracks actual helper lifetime, not widget polling.
// This is not a cross-process hardware lock: root helpers keep their own locks.
#include <cstdint>
#include <chrono>
#include <map>
#include <deque>
#include <optional>
#include <string>
#include <utility>

namespace operations {
enum class Phase { Running, TimedOut, OwnerGone, Exited };
enum class Outcome { Unknown, ReportedOk, ReportedFailure, NotStarted };
struct Entry { std::uint64_t id; std::string resource; Phase phase; Outcome outcome; std::chrono::system_clock::time_point time; };
class Registry {
    std::uint64_t next_ = 0;
    std::map<std::uint64_t, Entry> active_;
    std::deque<Entry> history_;
    void record(const Entry &e) { history_.push_back(e); if (history_.size() > 200) history_.pop_front(); }
public:
    std::uint64_t begin(std::string resource) {
        const auto id = ++next_;
        Entry e{id, std::move(resource), Phase::Running, Outcome::Unknown, std::chrono::system_clock::now()};
        active_.emplace(id, e); record(e); return id;
    }
    bool transition(std::uint64_t id, Phase phase) {
        auto it = active_.find(id);
        if (it == active_.end() || phase == Phase::Exited || phase == Phase::Running || it->second.phase == phase) return false;
        // A timeout is more important than the later disappearance of a widget.
        if (it->second.phase == Phase::TimedOut) return false;
        it->second.phase = phase; it->second.time = std::chrono::system_clock::now(); record(it->second); return true;
    }
    bool finish(std::uint64_t id, Outcome outcome) {
        auto it = active_.find(id); if (it == active_.end()) return false;
        auto e = it->second;
        if (e.phase == Phase::TimedOut) outcome = Outcome::Unknown;
        e.phase = Phase::Exited; e.outcome = outcome; e.time = std::chrono::system_clock::now(); record(e); active_.erase(it); return true;
    }
    const auto &active() const { return active_; }
    const auto &history() const { return history_; }
};
}
