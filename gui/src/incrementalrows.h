#pragma once
// Sorted unique keys. Structural edits are grouped, and value edits are decided
// by the caller after the keys have been reconciled. No Qt or I/O dependency.
#include <algorithm>
#include <cstddef>
#include <vector>
namespace incrementalrows {
struct Range { std::size_t first, count; };
struct Plan { std::vector<Range> remove, insert; bool valid = false; };
template<class Keys> Plan plan(const Keys &old, const Keys &next) {
    const auto unique = [](const Keys &k) {
        return std::adjacent_find(k.begin(), k.end(), [](const auto &a, const auto &b){return !(a < b);}) == k.end();
    };
    if (!unique(old) || !unique(next)) return {};
    Plan out; out.valid = true;
    const auto append = [](auto &ranges, std::size_t i) {
        if (!ranges.empty() && ranges.back().first + ranges.back().count == i) ++ranges.back().count;
        else ranges.push_back({i, 1});
    };
    std::size_t i=0, j=0;
    while (i<old.size() || j<next.size()) {
        if (j==next.size() || (i<old.size() && old[i]<next[j])) append(out.remove,i++);
        else if (i==old.size() || next[j]<old[i]) append(out.insert,j++);
        else { ++i; ++j; }
    }
    return out;
}
}
