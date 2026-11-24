#pragma once
// One fixed-size cache per metric. Only presentation paths should use it;
// safety checks, device writes and transaction readback must read directly.
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace telemetry {
enum class Quality { Unavailable, Fresh, Pending, Expired, Failed };
using Tick = std::chrono::milliseconds;

template<class T> struct Sample {
    std::optional<T> value;
    Tick captured{}; // monotonic time at start of acquisition, never wall clock
    std::uint64_t generation = 0;
    Quality quality = Quality::Unavailable;
};

// Caller provides a monotonic clock and a reader returning optional<T>.
// The winning caller performs the read outside the lock. Other callers never
// wait for I/O and never receive an expired value presented as fresh. A slow
// read remains in flight until it returns; no unbounded retry queue is created.
template<class T> class Slot {
public:
    template<class Clock, class Reader>
    Sample<T> read(Tick ttl, Clock now, Reader reader) {
        Tick start;
        std::uint64_t generation;
        {
            const std::lock_guard lock(mutex_);
            start = now();
            if (ttl <= Tick::zero()) return {{}, start, generation_, Quality::Unavailable};
            if (last_ && start >= last_->captured && start - last_->captured < ttl)
                return *last_;
            if (inFlight_) return {{}, start, generation_, Quality::Pending};
            inFlight_ = true;
            generation = generation_;
        }
        std::optional<T> value;
        Quality quality = Quality::Unavailable;
        try {
            value = reader();
            if (value) quality = Quality::Fresh;
        } catch (...) { quality = Quality::Failed; }
        const std::lock_guard lock(mutex_);
        inFlight_ = false;
        const auto finish = now();
        if (generation != generation_ || finish < start || finish - start >= ttl)
            return {{}, start, generation, Quality::Expired};
        last_ = Sample<T>{std::move(value), start, generation, quality};
        return *last_;
    }
    void invalidate() {
        const std::lock_guard lock(mutex_);
        ++generation_;
        last_.reset();
    }
private:
    std::mutex mutex_;
    std::optional<Sample<T>> last_;
    std::uint64_t generation_ = 0;
    bool inFlight_ = false;
};
} // namespace telemetry
