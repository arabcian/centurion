// Unit test for coinsights.cpp. Plain asserts, like costrategy_test.cpp.
#include "../src/coinsights.h"
#include "../src/costrategy.h"

#include <cmath>
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

using namespace coinsights;

int main() {
    // changePercent
    CHECK(!changePercent(0, 10));
    CHECK(!changePercent(-1, 10));
    CHECK(std::abs(*changePercent(100, 97) + 3.0) < 1e-9);
    CHECK(std::abs(*changePercent(100, 100)) < 1e-9);
    CHECK(*changePercent(100, 97) < STRETCH_PERCENT);
    CHECK(!(*changePercent(100, 99) < STRETCH_PERCENT));

    // boostShare
    CHECK(!boostShare(0, 5400));
    CHECK(!boostShare(5000, 0));
    CHECK(std::abs(*boostShare(5130, 5400) - 0.95) < 1e-9);
    CHECK(*boostShare(5000, 5400) < LOW_BOOST);

    // rankAnomalies: too few ranked undervolted cores -> nothing
    CHECK(rankAnomalies({200, 150, 100}, {-10, -20, -30}).isEmpty());
    // unknown rank / stock cores are ignored
    CHECK(rankAnomalies({200, -1, 150, 100, 90}, {-10, -40, -20, -30, 0}).isEmpty());
    // best-ranked core (200) deeper than every below-median core (-25, -20) -> flagged
    CHECK((rankAnomalies({200, 180, 120, 100}, {-30, -10, -25, -20}) == QList<int>{0}));
    // the usual pattern (better rank, less negative) -> nothing
    CHECK(rankAnomalies({200, 180, 120, 100}, {-10, -15, -25, -30}).isEmpty());
    // equal to the deepest low core is not "more negative"
    CHECK(rankAnomalies({200, 180, 120, 100}, {-25, -10, -25, -20}).isEmpty());

    // etaMinutes
    const int verify = cosearch::VERIFY_SECONDS / 60;
    CHECK(etaMinutes(10, 0, 16, 120, true) == 110);
    CHECK(etaMinutes(200, 0, 16, 120, true) == 0);
    CHECK(etaMinutes(40, 4, 16, 120, true) == 120 + verify);
    CHECK(etaMinutes(40, 16, 16, 120, true) == verify);
    CHECK(etaMinutes(40, 16, 16, 120, false) == 0);

    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::puts("coinsights: all checks passed");
    return failures ? 1 : 0;
}
