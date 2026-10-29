#include "batteryadvice.h"

namespace batteryadvice {

Advice evaluate(const QJsonObject &days, QDate today, const QStringList &modes, const QString &current) {
    Advice a;
    if (!modes.contains(QStringLiteral("Long_Life")) || current == QLatin1String("Long_Life")) return a;
    double ac = 0, full = 0;
    for (int i = 0; i < WINDOW_DAYS; ++i) {
        const QJsonObject d = days.value(today.addDays(-i).toString(Qt::ISODate)).toObject();
        ac += d.value(QStringLiteral("ac")).toDouble();
        full += d.value(QStringLiteral("full")).toDouble();
    }
    a.acHours = ac / 3600.0;
    a.fullShare = ac > 0 ? full / ac : 0;
    a.show = ac >= MIN_AC_SEC && a.fullShare > FULL_SHARE;
    return a;
}

QJsonObject pruned(const QJsonObject &days, QDate today, int keepDays) {
    // ISO dates compare as strings in date order.
    const QString oldest = today.addDays(1 - keepDays).toString(Qt::ISODate);
    QJsonObject out;
    for (auto it = days.constBegin(); it != days.constEnd(); ++it) {
        if (it.key() >= oldest) out.insert(it.key(), it.value());
    }
    return out;
}

ChargeModes parseChargeTypes(const QString &line) {
    ChargeModes c;
    for (QString word : line.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        if (word.size() > 2 && word.startsWith(QLatin1Char('[')) && word.endsWith(QLatin1Char(']'))) {
            word = word.mid(1, word.size() - 2);
            c.current = word;
        }
        c.modes << word;
    }
    return c;
}

}  // namespace batteryadvice
