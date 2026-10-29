#pragma once
// Health → Network → Traffic: bytes per program per local day, as the netguard daemon keeps them in
// /var/lib/centurion/traffic.json (see crates/centurion-helpers/src/traffic.rs). Pure parsing only; the
// page reads the file directly, like the connection log (readable by the wheel group).

#include <QByteArray>
#include <QMap>
#include <QString>

struct TrafficTotal {
    qint64 tx = 0;     // sent
    qint64 rx = 0;     // received
    qint64 conns = 0;  // connections that closed
};

/// local day (YYYY-MM-DD) → program → totals
using TrafficDays = QMap<QString, QMap<QString, TrafficTotal>>;

QString trafficFilePath();
TrafficDays parseTraffic(const QByteArray &json);
/// Sent + received of one program on one day; 0 when it has none.
qint64 trafficBytes(const TrafficDays &days, const QString &day, const QString &program);
