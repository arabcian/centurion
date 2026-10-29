// Unit test for the Health → Battery use charger advice (batteryadvice.cpp). Plain asserts, like sleepadvice_test.cpp.
#include "../src/batteryadvice.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <cmath>
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

using namespace batteryadvice;

// `hours` on the charger on `daysAgo` days ago, `fullHours` of them at full charge.
static void add(QJsonObject &days, QDate today, int daysAgo, double hours, double fullHours) {
    QJsonObject d = days.value(today.addDays(-daysAgo).toString(Qt::ISODate)).toObject();
    d[QStringLiteral("ac")] = d.value(QStringLiteral("ac")).toDouble() + hours * 3600;
    d[QStringLiteral("full")] = d.value(QStringLiteral("full")).toDouble() + fullHours * 3600;
    days[today.addDays(-daysAgo).toString(Qt::ISODate)] = d;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QDate today(2026, 10, 29);
    const QStringList withLongLife{QStringLiteral("Standard"), QStringLiteral("Fast"), QStringLiteral("Long_Life")};
    const QStringList standardOnly{QStringLiteral("Standard"), QStringLiteral("Fast")};

    // charge_types: the bracketed word is the selected mode.
    const ChargeModes cm = parseChargeTypes(QStringLiteral("Standard [Fast] Long_Life"));
    CHECK(cm.modes == withLongLife && cm.current == QLatin1String("Fast"));
    CHECK(parseChargeTypes(QStringLiteral("  Fast  Standard [Long_Life] ")).current == QLatin1String("Long_Life"));
    CHECK(parseChargeTypes(QStringLiteral("")).modes.isEmpty());

    // Most charger time at full charge, enough of it, Long_Life available and not selected → advice.
    QJsonObject days;
    for (int i = 0; i < 10; ++i) add(days, today, i, 3, 2.5);  // 30 h on the charger, 25 h full (83 %)
    const Advice yes = evaluate(days, today, withLongLife, QStringLiteral("Standard"));
    CHECK(yes.show);
    CHECK(std::fabs(yes.acHours - 30) < 1e-9 && std::fabs(yes.fullShare - 25.0 / 30.0) < 1e-9);

    // Already on Long_Life, no Long_Life on this battery, or no charge modes at all → nothing.
    CHECK(!evaluate(days, today, withLongLife, QStringLiteral("Long_Life")).show);
    CHECK(!evaluate(days, today, standardOnly, QStringLiteral("Standard")).show);
    CHECK(!evaluate(days, today, {}, QString()).show);

    // Too little charger time to judge.
    QJsonObject few;
    for (int i = 0; i < 3; ++i) add(few, today, i, 3, 3);  // 9 h, all full
    CHECK(!evaluate(few, today, withLongLife, QStringLiteral("Standard")).show);

    // A full share below the limit is not enough.
    QJsonObject mixed;
    for (int i = 0; i < 10; ++i) add(mixed, today, i, 3, 2);  // 67 %
    CHECK(!evaluate(mixed, today, withLongLife, QStringLiteral("Standard")).show);

    // Days outside the 30-day window are not counted.
    QJsonObject old;
    for (int i = 0; i < 10; ++i) add(old, today, WINDOW_DAYS + i, 3, 3);  // all full, but too old
    CHECK(!evaluate(old, today, withLongLife, QStringLiteral("Standard")).show);

    // Pruning keeps today and the KEEP_DAYS - 1 days before it.
    QJsonObject kept;
    add(kept, today, 0, 1, 1);
    add(kept, today, KEEP_DAYS - 1, 1, 1);
    add(kept, today, KEEP_DAYS, 1, 1);
    const QJsonObject p = pruned(kept, today, KEEP_DAYS);
    CHECK(p.contains(today.toString(Qt::ISODate)));
    CHECK(p.contains(today.addDays(1 - KEEP_DAYS).toString(Qt::ISODate)));
    CHECK(!p.contains(today.addDays(-KEEP_DAYS).toString(Qt::ISODate)));

    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("batteryadvice: all checks passed\n");
    return 0;
}
