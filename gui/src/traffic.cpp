#include "traffic.h"

#include <QJsonDocument>
#include <QJsonObject>

QString trafficFilePath() { return QStringLiteral("/var/lib/centurion/traffic.json"); }

TrafficDays parseTraffic(const QByteArray &json) {
    TrafficDays out;
    const QJsonObject days = QJsonDocument::fromJson(json).object().value(QLatin1String("days")).toObject();
    for (auto d = days.constBegin(); d != days.constEnd(); ++d) {
        const QJsonObject progs = d.value().toObject();
        for (auto p = progs.constBegin(); p != progs.constEnd(); ++p) {
            const QJsonObject t = p.value().toObject();
            out[d.key()][p.key()] = TrafficTotal{t.value(QLatin1String("tx")).toInteger(), t.value(QLatin1String("rx")).toInteger(),
                                                 t.value(QLatin1String("conns")).toInteger()};
        }
    }
    return out;
}

qint64 trafficBytes(const TrafficDays &days, const QString &day, const QString &program) {
    const auto d = days.constFind(day);
    if (d == days.constEnd()) return 0;
    const auto p = d->constFind(program);
    return p == d->constEnd() ? 0 : p->tx + p->rx;
}
