#include "../src/slotmap.h"
#include <cstdio>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main() {
    using slotmap::CoreIds;
    // 8 contiguous cores per CCD (the second CCD's ids start higher): position == slot.
    CHECK(slotmap::problem({CoreIds{0,1,2,3,4,5,6,7}, CoreIds{8,9,10,11,12,13,14,15}}, {}).empty());
    // A core is offline: the remaining ones shift down.
    CHECK(!slotmap::problem({CoreIds{0,1,2,3,4,5,6,7}}, {5, 21}).empty());
    // A gap in the physical numbering (core 3 fused off): position 3 would be physical core 4.
    CHECK(!slotmap::problem({CoreIds{0,1,2,4,5,6}}, {}).empty());
    // Out of order, or an unreadable id.
    CHECK(!slotmap::problem({CoreIds{1,0,2}}, {}).empty());
    CHECK(!slotmap::problem({CoreIds{0, std::nullopt, 2}}, {}).empty());
    return 0;
}
