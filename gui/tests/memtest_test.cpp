// Unit test for Health → Curve stability → Memory test (memtest.cpp). Plain asserts, like sleepadvice_test.cpp.
#include "../src/memtest.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

using namespace memtest;

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    CHECK(availableKib("MemTotal:       32000000 kB\nMemFree:         1000000 kB\nMemAvailable:   20480000 kB\n") == 20480000);
    CHECK(availableKib("MemTotal: 1 kB\n") == -1);

    // 50 % of 20 GiB available for 10 min.
    Plan s = plan(Tool::Stressapptest, 20 * 1024 * 1024, 50, 600, 8 << 20);
    CHECK(s.error.isEmpty() && s.mib == 10240 && !s.clamped);
    CHECK(s.args == (QStringList{"-M", "10240", "-s", "600"}));
    // memtester is held to the memlock limit; the default 8 MiB is too little to start.
    Plan m = plan(Tool::Memtester, 20 * 1024 * 1024, 50, 600, 8 << 20);
    CHECK(m.clamped && m.error == QLatin1String("memlock") && m.args.isEmpty());
    Plan m2 = plan(Tool::Memtester, 20 * 1024 * 1024, 50, 600, qint64(4) << 30);
    CHECK(m2.clamped && m2.mib == 4096 && m2.args == QStringList{"4096M"});
    Plan m3 = plan(Tool::Memtester, 20 * 1024 * 1024, 50, 600, -1);
    CHECK(!m3.clamped && m3.mib == 10240);
    CHECK(plan(Tool::Stressapptest, 100 * 1024, 50, 60, -1).error == QLatin1String("memory"));
    CHECK(!plan(Tool::None, 20 * 1024 * 1024, 50, 60, -1).error.isEmpty());

    // memtester: a FAILURE line fails the run; reaching the deadline without one passes.
    Run r;
    feed(r, Tool::Memtester, QStringLiteral("  Stuck Address       : ok"));
    CHECK(r.errors == 0);
    CHECK(verdict(r, Tool::Memtester, 0, true, true, false, 0) == Verdict::Pass);   // killed by us at the deadline
    CHECK(verdict(r, Tool::Memtester, 0, true, false, true, 0) == Verdict::Incomplete);
    CHECK(verdict(r, Tool::Memtester, 1, false, false, false, 0) == Verdict::Incomplete);  // could not allocate
    CHECK(verdict(r, Tool::Memtester, 0, true, true, false, 1) == Verdict::Fail);    // a machine check during the run
    feed(r, Tool::Memtester, QStringLiteral("  Random Value        : FAILURE: 0x1234 != 0x1235 at offset 0x00ab."));
    CHECK(r.errors == 1 && verdict(r, Tool::Memtester, 0, true, false, true, 0) == Verdict::Fail);
    Run u;
    feed(u, Tool::Memtester, QStringLiteral("insufficient permission.\nTrying again, unlocked:"));
    CHECK(u.unlocked);
    CHECK(verdict(Run{}, Tool::Memtester, 4, false, false, false, 0) == Verdict::Fail);

    // stressapptest: its own Status line decides; no Status line (killed, crashed) is incomplete.
    Run a;
    feed(a, Tool::Stressapptest, QStringLiteral("Stats: Found 0 hardware incidents"));
    feed(a, Tool::Stressapptest, QStringLiteral("Status: PASS - please verify no corrections are recorded in your system log"));
    CHECK(a.statusPass && verdict(a, Tool::Stressapptest, 0, false, false, false, 0) == Verdict::Pass);
    CHECK(verdict(Run{}, Tool::Stressapptest, 0, true, false, false, 0) == Verdict::Incomplete);
    Run b;
    feed(b, Tool::Stressapptest, QStringLiteral("Log: Hardware Error: miscompare on CPU 3(0x8) at 0x7f... : read:0x0 expected:0x1"));
    feed(b, Tool::Stressapptest, QStringLiteral("Stats: Found 3 hardware incidents"));
    feed(b, Tool::Stressapptest, QStringLiteral("Status: FAIL - test discovered HW problems"));
    CHECK(b.errors == 3 && verdict(b, Tool::Stressapptest, 1, false, false, false, 0) == Verdict::Fail);

    // The record keeps the newest KEEP results.
    QTemporaryDir dir;
    qputenv("XDG_STATE_HOME", dir.path().toUtf8());
    CHECK(load().isEmpty());
    for (int i = 0; i < KEEP + 3; ++i) CHECK(append(QJsonObject{{"t", i}}));
    const QJsonArray all = load();
    CHECK(all.size() == KEEP && all.first().toObject().value("t").toInt() == 3 && all.last().toObject().value("t").toInt() == KEEP + 2);

    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
