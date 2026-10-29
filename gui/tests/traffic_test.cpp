// Unit test for the traffic ledger reader (traffic.cpp). Plain asserts, like blocklistsubs_test.cpp.
#include "../src/traffic.h"

#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main() {
    const TrafficDays days = parseTraffic(
        R"({"version":1,"days":{"2026-10-29":{"firefox":{"tx":100,"rx":900,"conns":3},"game.exe":{"tx":5,"rx":7,"conns":1}},)"
        R"("2026-10-30":{"firefox":{"tx":1,"rx":1,"conns":1}}}})");
    CHECK(days.size() == 2);
    CHECK(days.value(QStringLiteral("2026-10-29")).value(QStringLiteral("firefox")).rx == 900);
    CHECK(days.value(QStringLiteral("2026-10-29")).value(QStringLiteral("firefox")).conns == 3);
    CHECK(trafficBytes(days, QStringLiteral("2026-10-29"), QStringLiteral("firefox")) == 1000);
    CHECK(trafficBytes(days, QStringLiteral("2026-10-29"), QStringLiteral("nope")) == 0);
    CHECK(trafficBytes(days, QStringLiteral("2000-01-01"), QStringLiteral("firefox")) == 0);

    CHECK(parseTraffic("not json").isEmpty());
    CHECK(parseTraffic("{}").isEmpty());
    CHECK(parseTraffic(R"({"days":{"d":{"p":{"tx":"x"}}}})").value(QStringLiteral("d")).value(QStringLiteral("p")).tx == 0);

    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::printf("traffic: all checks passed\n");
    return 0;
}
