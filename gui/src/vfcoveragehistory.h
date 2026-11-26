#pragma once

#include "curvereadback.h"
#include "diagsources.h"
#include "vfcoverage.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <utility>

namespace vfcoverage {

inline constexpr int HistoryLimit = 24;
inline constexpr int MaxSamples = 86400;

inline QString fingerprint(const QJsonObject &device, const QJsonArray &points) {
    if (!curvereadback::verifiedDevice(device)) return {};
    QJsonArray curve;
    for (const auto &value : points) {
        const auto p = value.toObject();
        if (p.value("domain").toString() != QStringLiteral("gpu")) continue;
        curve.append(QJsonArray{p.value("index"), p.value("volt_uV"), p.value("freq_kHz"),
                                p.value("freq_offset_kHz"), p.value("base_kHz")});
    }
    if (curve.isEmpty()) return {};
    const QJsonArray identity{device.value("uuid").toString().toLower(), device.value("driver"),
                               device.value("vbios"), curve};
    const auto bytes = QJsonDocument(identity).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

struct Session {
    QString curve;
    QDateTime started;
    QVector<qint64> voltages;
    Coverage coverage;
};

inline bool comparable(const Session &a, const Session &b) {
    return a.curve == b.curve && a.voltages == b.voltages
        && a.coverage.counts.size() == b.coverage.counts.size();
}

inline std::pair<qint64, qint64> estimatedRange(const Session &session) {
    const auto first = session.voltages.at(session.coverage.lowest());
    auto lo = first, hi = first;
    for (qsizetype i = 0; i < session.voltages.size(); ++i) {
        if (!session.coverage.counts.at(i)) continue;
        lo = std::min(lo, session.voltages.at(i));
        hi = std::max(hi, session.voltages.at(i));
    }
    return {lo, hi};
}

inline QJsonObject encode(const Session &session) {
    QJsonArray points;
    for (qsizetype i = 0; i < session.voltages.size(); ++i)
        points.append(QJsonArray{session.voltages.at(i), session.coverage.counts.at(i)});
    return {{"curve", session.curve}, {"started", session.started.toUTC().toString(Qt::ISODateWithMs)}, {"points", points}};
}

inline std::optional<Session> decode(const QJsonObject &row) {
    static const QRegularExpression hash(QStringLiteral("\\A[0-9a-f]{64}\\z"));
    Session session;
    session.curve = row.value("curve").toString();
    session.started = QDateTime::fromString(row.value("started").toString(), Qt::ISODateWithMs);
    const auto points = row.value("points").toArray();
    if (!hash.match(session.curve).hasMatch() || !session.started.isValid() || session.started.timeSpec() != Qt::UTC
        || points.isEmpty() || points.size() > 255) return std::nullopt;
    for (const auto &value : points) {
        const auto point = value.toArray();
        if (point.size() != 2) return std::nullopt;
        const auto voltage = point.at(0).toInteger(-1);
        const auto count = point.at(1).toInteger(-1);
        if (voltage < 0 || voltage > 10000000 || count < 0 || count > MaxSamples - session.coverage.samples)
            return std::nullopt;
        session.voltages.append(voltage);
        session.coverage.counts.push_back(int(count));
        session.coverage.samples += int(count);
    }
    if (!session.coverage.samples) return std::nullopt;
    return session;
}

inline std::optional<QList<Session>> loadHistory(const QString &path) {
    const QFileInfo file(path);
    if (file.isSymLink()) return std::nullopt;
    if (!file.exists()) return QList<Session>{};
    const auto archive = diagsrc::loadJson(path, 256 * 1024);
    if (archive.value("version").toInt() != 1 || !archive.value("sessions").isArray()) return std::nullopt;
    const auto rows = archive.value("sessions").toArray();
    if (rows.size() > HistoryLimit) return std::nullopt;
    QList<Session> out;
    for (const auto &row : rows) {
        const auto session = decode(row.toObject());
        if (!session) return std::nullopt;
        out.append(*session);
    }
    return out;
}

inline bool saveSession(const QString &path, const Session &session) {
    if (session.voltages.size() != qsizetype(session.coverage.counts.size())) return false;
    const auto row = encode(session);
    if (!decode(row) || !QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QLockFile lock(path + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) return false;
    const auto history = loadHistory(path);
    if (!history) return false;
    QJsonArray rows;
    const auto keep = std::min<qsizetype>(history->size(), HistoryLimit - 1);
    for (qsizetype i = history->size() - keep; i < history->size(); ++i)
        rows.append(encode(history->at(i)));
    rows.append(row);
    return diagsrc::saveJson(path, {{"version", 1}, {"sessions", rows}}).isEmpty();
}

} // namespace vfcoverage
