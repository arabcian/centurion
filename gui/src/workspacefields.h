#pragma once
// Validation shared by the workspace UI and standalone native tests.
#include <algorithm>
#include <cstddef>
#include <vector>
namespace workspacefields {
inline bool permutation(const std::vector<int> &order, int count) {
    if (count <= 0 || count > 128 || order.size() != static_cast<std::size_t>(count)) return false;
    std::vector<bool> seen(static_cast<std::size_t>(count));
    for (int n : order) { if (n < 0 || n >= count || seen[n]) return false; seen[n] = true; }
    return true;
}
inline bool widths(const std::vector<int> &values, int count, int minimum) {
    return count > 0 && count <= 128 && minimum > 0 && minimum <= 4000 &&
        values.size() == static_cast<std::size_t>(count) &&
        std::all_of(values.begin(), values.end(), [minimum](int w){return w >= minimum && w <= 4000;});
}
inline bool split(const std::vector<int> &values, int count) {
    return count >= 2 && count <= 16 && values.size() == static_cast<std::size_t>(count) &&
        std::all_of(values.begin(), values.end(), [](int v){return v > 0 && v <= 100000;});
}
inline int panelColumns(int width, int required, int previous, int visible) {
    const long long threshold = 2LL * std::max(0, required) + 12 + (previous == 1 ? 24 : 0);
    return visible > 1 && width >= threshold ? 2 : 1;
}
}
