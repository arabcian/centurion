#pragma once
// Read-only evidence accounting. A pass is an observation, never a stability guarantee.
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace cohistory {
enum class Result { Passed, Failed, Interrupted, Unknown };
struct Observation {
    std::string id;
    int ccd = -1, slot = -1, offset = 1;
    Result result = Result::Unknown;
};
struct Counts {
    std::size_t passed = 0, failed = 0, interrupted = 0;
    bool contradictory() const { return passed && (failed || interrupted); }
};
using Key = std::tuple<int,int,int>;
struct Summary {
    std::map<Key, Counts> cells;
    std::size_t rejected = 0;
};
inline Summary summarize(const std::vector<Observation> &observations) {
    Summary out;
    std::map<std::string, std::size_t> ids;
    for (const auto &o : observations) if (!o.id.empty()) ++ids[o.id];
    for (const auto &o : observations) {
        if (o.id.empty() || ids[o.id] != 1 || o.ccd < 0 || o.ccd > 511 || o.slot < 0 || o.slot > 511
            || o.offset < -50 || o.offset >= 0 || o.result == Result::Unknown) { ++out.rejected; continue; }
        auto &c = out.cells[{o.ccd,o.slot,o.offset}];
        switch (o.result) {
        case Result::Passed: ++c.passed; break;
        case Result::Failed: ++c.failed; break;
        case Result::Interrupted: ++c.interrupted; break;
        case Result::Unknown: break;
        }
    }
    return out;
}
} // namespace cohistory
