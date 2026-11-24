#include "traffic.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QDate>
#include <algorithm>
#include <limits>

qint64 addTrafficCounter(qint64 a, qint64 b) {
    a = qMax(qint64(0), a); b = qMax(qint64(0), b);
    const auto max = std::numeric_limits<qint64>::max();
    return b > max - a ? max : a + b;
}

QString trafficFilePath() { return QStringLiteral("/var/lib/centurion/traffic.json"); }

TrafficLedger decodeTraffic(const QByteArray &json) {
    if (json.size() > TrafficMaxBytes) return {};
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return {};
    const auto root = document.object();
    if (root.value(QStringLiteral("version")).toInteger(-1) != 1 || !root.value(QStringLiteral("days")).isObject()) return {};
    const auto days = root.value(QStringLiteral("days")).toObject();
    TrafficLedger out;
    if (days.size() > 366) return {};
    for (auto d = days.constBegin(); d != days.constEnd(); ++d) {
        const auto date = QDate::fromString(d.key(), Qt::ISODate);
        if (!date.isValid() || date.toString(Qt::ISODate) != d.key() || !d.value().isObject()) return {};
        const auto progs = d.value().toObject();
        if (progs.size() > 4096) return {};
        auto &day = out.days[d.key()];
        for (auto p = progs.constBegin(); p != progs.constEnd(); ++p) {
            if (p.key().isEmpty() || p.key().size() > 4096 || !p.value().isObject()) return {};
            const auto t = p.value().toObject();
            // Qt 6 retains signed 64-bit JSON integers exactly. Values outside this
            // range, negative or fractional are not measured zeroes.
            const auto tx = t.value(QStringLiteral("tx")).toInteger(-1);
            const auto rx = t.value(QStringLiteral("rx")).toInteger(-1);
            const auto conns = t.value(QStringLiteral("conns")).toInteger(-1);
            if (tx < 0 || rx < 0 || conns < 0) return {};
            day.insert(p.key(), TrafficTotal{tx, rx, conns});
        }
    }
    out.valid = true;
    return out;
}

TrafficDays parseTraffic(const QByteArray &json) { return decodeTraffic(json).days; }

QStringList rankTraffic(const TrafficDays &days, const QString &first, const QString &last) {
    QMap<QString, qint64> sums;
    for (auto d = days.lowerBound(first); d != days.cend() && d.key() <= last; ++d)
        for (auto p = d->cbegin(); p != d->cend(); ++p)
            sums[p.key()] = addTrafficCounter(sums.value(p.key()), addTrafficCounter(p->tx, p->rx));
    auto names = sums.keys();
    std::sort(names.begin(), names.end(), [&sums](const QString &a, const QString &b) {
        return sums.value(a) == sums.value(b) ? a < b : sums.value(a) > sums.value(b);
    });
    return names;
}

TrafficTotal sumTraffic(const QMap<QString, TrafficTotal> &day, const QStringList &excluded) {
    TrafficTotal total;
    for (auto it = day.cbegin(); it != day.cend(); ++it) {
        if (excluded.contains(it.key())) continue;
        total.tx = addTrafficCounter(total.tx, it->tx);
        total.rx = addTrafficCounter(total.rx, it->rx);
        total.conns = addTrafficCounter(total.conns, it->conns);
    }
    return total;
}

qint64 trafficBytes(const TrafficDays &days, const QString &day, const QString &program) {
    const auto d = days.constFind(day);
    if (d == days.constEnd()) return 0;
    const auto p = d->constFind(program);
    return p == d->constEnd() ? 0 : addTrafficCounter(p->tx, p->rx);
}
