#pragma once
// Health → Network → Traffic: bytes per program per local day, as the netguard daemon keeps them in
// /var/lib/centurion/traffic.json (see crates/centurion-helpers/src/traffic.rs). Pure parsing only; the
// page reads the file directly, like the connection log (readable by the wheel group).

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>

struct TrafficTotal {
    qint64 tx = 0;     // sent
    qint64 rx = 0;     // received
    qint64 conns = 0;  // connections that closed
};

/// local day (YYYY-MM-DD) → program → totals
using TrafficDays = QMap<QString, QMap<QString, TrafficTotal>>;

/// Saturating addition of non-negative ledger counters (never signed-overflows).
qint64 addTrafficCounter(qint64 a, qint64 b);
QString trafficFilePath();
struct TrafficLedger { TrafficDays days; bool valid = false; };
constexpr qsizetype TrafficMaxBytes = 16 * 1024 * 1024;
TrafficLedger decodeTraffic(const QByteArray &json);
TrafficDays parseTraffic(const QByteArray &json);
/// Rank by saturated sent + received, with deterministic tie-breaking, within the selected dates.
QStringList rankTraffic(const TrafficDays &days, const QString &first, const QString &last);
TrafficTotal sumTraffic(const QMap<QString, TrafficTotal> &day, const QStringList &excluded = {});
/// Sent + received of one program on one day; 0 when it has none.
qint64 trafficBytes(const TrafficDays &days, const QString &day, const QString &program);
