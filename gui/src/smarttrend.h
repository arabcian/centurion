#pragma once
#include <cstdint>
#include <optional>

// Monotonic, nonnegative counters. A decrease is not a negative error count:
// it invalidates the comparable interval (reset, replacement or bad sample).
namespace smarttrend {
using Count = std::optional<std::int64_t>;
inline Count increase(Count before, Count after) {
    if (!before || !after || *before < 0 || *after < *before) return {};
    return *after - *before;
}
class Interval {
public:
    void observe(Count value) {
        if (!value || *value < 0) return;
        if (!first_) first_ = value;
        if (last_ && *value < *last_) reset_ = true;
        last_ = value;
    }
    Count delta(Count current) const {
        if (reset_ || !last_ || !increase(last_, current)) return {};
        return increase(first_, current);
    }
private:
    Count first_, last_;
    bool reset_ = false;
};
}
