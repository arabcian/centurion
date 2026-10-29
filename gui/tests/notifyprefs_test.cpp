// Unit test for the notification gate (notifyprefs.cpp decide()). Plain asserts, like sleepadvice_test.cpp.
#include "../src/notifyprefs.h"

#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

using namespace notifyprefs;

int main() {
    const QString none;
    Prefs p;  // defaults: everything shown, nothing deferred

    CHECK(decide(p, KERNEL, ERROR_LEVEL, false) == Verdict::Show);
    CHECK(decide(p, NETWORK, none, false) == Verdict::Show);
    CHECK(ALL_CATEGORIES.size() == 7);

    // A switched-off category is hidden, whatever the game state; the others are unaffected.
    p.off = {BACKUP};
    CHECK(decide(p, BACKUP, none, false) == Verdict::Hide);
    CHECK(decide(p, BACKUP, none, true) == Verdict::Hide);
    CHECK(decide(p, CURVE, none, false) == Verdict::Show);

    // Kernel alerts: "critical only" hides errors, keeps critical ones; it does not touch other categories.
    p = Prefs{};
    p.criticalOnly = true;
    CHECK(decide(p, KERNEL, ERROR_LEVEL, false) == Verdict::Hide);
    CHECK(decide(p, KERNEL, CRITICAL_LEVEL, false) == Verdict::Show);
    CHECK(decide(p, SMART, none, false) == Verdict::Show);

    // Deferral holds alerts only while a game runs.
    p = Prefs{};
    p.deferGame = true;
    CHECK(decide(p, HEAT, none, true) == Verdict::Defer);
    CHECK(decide(p, HEAT, none, false) == Verdict::Show);

    // Switched off wins over deferral.
    p.off = {HEAT};
    CHECK(decide(p, HEAT, none, true) == Verdict::Hide);

    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("notifyprefs: all checks passed\n");
    return 0;
}
