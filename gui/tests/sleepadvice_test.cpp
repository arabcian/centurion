// Unit test for the Health → Sleep wake-source suggestions (sleepadvice.cpp).
// Plain asserts, like int64spinbox_test.cpp.
#include "../src/sleepadvice.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

using namespace sleepadvice;

static Sleep row(qint64 start, qint64 end, const char *woke, double before, double after, const char *status = "Discharging") {
    return parse(QJsonDocument::fromJson(QStringLiteral(R"({"start":%1,"end":%2,"woke_by":"%3","status_before":"%4",
        "energy_before":%5,"energy_after":%6,"full":80000000})").arg(start).arg(end).arg(QLatin1String(woke), QLatin1String(status))
        .arg(before, 0, 'f', 0).arg(after, 0, 'f', 0).toUtf8()).object());
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    // 0000:00:14.0 (USB controller) → XHC0; 1-3 is a mouse behind it; the lid has its own device.
    auto resolve = [](const QString &n) -> QString {
        if (n == QLatin1String("0000:00:14.0") || n == QLatin1String("1-3")) return QStringLiteral("XHC0");
        if (n == QLatin1String("PNP0C0D:00")) return QStringLiteral("LID0");
        return {};
    };

    CHECK(parse(QJsonObject{{"woke_by", "a,b"}}).wokeBy == (QStringList{"a", "b"}));
    CHECK(isUserWake("PNP0C0D:00") && isUserWake("LNXPWRBN:00") && isUserWake("LID0") && isUserWake("PWRB"));
    CHECK(!isUserWake("XHC0") && !isUserWake("0000:00:14.0"));

    // Two lid-closed wakes by the USB controller; each followed by 30 min awake on battery.
    // 1 % of 80 Wh = 800000 µWh.
    QList<Sleep> rows{
        row(0, 3600, "0000:00:14.0", 60e6, 59e6),       // then awake 59e6 → 58.2e6 = 1 %
        row(5400, 9000, "1-3", 58.2e6, 58e6),            // then awake 58e6 → 57.2e6 = 1 %
        row(10800, 20000, "PNP0C0D:00", 57.2e6, 57e6),   // woken by the lid: not counted
        row(30000, 40000, "", 50e6, 49e6),
    };
    QList<Advice> a = analyse(rows, resolve);
    CHECK(a.size() == 1);
    if (!a.isEmpty()) {
        CHECK(a[0].device == QLatin1String("XHC0"));
        CHECK(a[0].wakes == 2);
        CHECK(qAbs(a[0].awakePct - 2.0) < 1e-6);
    }

    // A single wake is not a pattern.
    CHECK(analyse(rows.mid(0, 1), resolve).isEmpty());

    // The lid together with another source: the user opened it, nothing to blame.
    CHECK(analyse({row(0, 1, "PNP0C0D:00,0000:00:14.0", 1, 1), row(2, 3, "PNP0C0D:00,0000:00:14.0", 1, 1)}, resolve).isEmpty());

    // On AC, or awake for longer than MAX_AWAKE_S: counted as wakes, no battery cost.
    a = analyse({row(0, 10, "1-3", 60e6, 60e6, "Charging"), row(20, 30, "1-3", 60e6, 60e6, "Charging"),
                 row(30 + MAX_AWAKE_S + 1, 40 + MAX_AWAKE_S, "x", 50e6, 50e6)}, resolve);
    CHECK(a.size() == 1 && a[0].wakes == 2 && a[0].awakePct == 0);

    // Sources that resolve to nothing switchable are left out.
    CHECK(analyse({row(0, 1, "serio0", 1, 1), row(2, 3, "serio0", 1, 1)}, resolve).isEmpty());

    // Hardware sleep: the last three long sleeps with a reading must all be nearly idle.
    auto hw = [](qint64 start, qint64 len, double pct) {
        Sleep s;
        s.start = start;
        s.end = start + len;
        s.hasHwSleep = true;
        s.hwSleepUs = pct / 100.0 * double(len) * 1e6;
        return s;
    };
    CHECK(hwSleepIdle({hw(0, 3600, 0), hw(4000, 3600, 1), hw(8000, 3600, 0.5)}));
    CHECK(!hwSleepIdle({hw(0, 3600, 0), hw(4000, 3600, 40), hw(8000, 3600, 0)}));       // one busy sleep among the three
    CHECK(!hwSleepIdle({hw(0, 3600, 0), hw(4000, 3600, 0)}));                           // only two
    CHECK(hwSleepIdle({hw(0, 60, 50), hw(100, 3600, 0), hw(4000, 3600, 0), hw(8000, 3600, 0)})); // short sleep ignored
    Sleep unread = hw(0, 3600, 0);
    unread.hasHwSleep = false;  // no sleep-hook reading
    CHECK(!hwSleepIdle({unread, unread, unread}));

    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::puts("sleepadvice: all checks passed");
    return failures ? 1 : 0;
}
