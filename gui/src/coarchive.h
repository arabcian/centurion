#pragma once
// Local evidence archive, deliberately separate from the recovery journal.
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <optional>
#include <utility>

namespace coarchive {
inline constexpr qint64 limit = 1LL << 20;
inline std::optional<QJsonArray> read(const QString &path) {
    QFile f(path);
    if (!f.exists()) return QJsonArray{};
    if (!f.open(QIODevice::ReadOnly) || f.size() > limit) return std::nullopt;
    const auto bytes = f.read(limit + 1);
    const auto doc = QJsonDocument::fromJson(bytes);
    const auto o = doc.object();
    if (bytes.size() > limit || !doc.isObject() || o.value("version").toInt() != 2 || !o.value("history").isArray()) return std::nullopt;
    const auto rows = o.value("history").toArray();
    if (rows.size() > 40) return std::nullopt;
    for (const auto &v : rows) if (!v.isObject()) return std::nullopt;
    return rows;
}
inline QByteArray encode(const QJsonArray &rows) {
    return QJsonDocument(QJsonObject{{"version", 2}, {"history", rows}}).toJson(QJsonDocument::Compact);
}
inline bool append(const QString &path, QJsonObject record) {
    QLockFile lock(path + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) return false;
    auto rows = read(path);
    if (!rows) return false; // never silently overwrite unreadable or incompatible evidence
    // If an unusually long session exceeds the cap on its own, retain its newest trials
    // and explicitly report how many oldest trials are omitted.
    auto attempts = record.value("attempts").toArray();
    const qsizetype original = attempts.size();
    // Halve the retained tail on each pass to bound serialization work for long runs.
    while (encode(QJsonArray{record}).size() > limit && !attempts.isEmpty()) {
        QJsonArray tail;
        for (qsizetype i = (attempts.size() + 1) / 2; i < attempts.size(); ++i) tail.append(attempts.at(i));
        attempts = std::move(tail);
        record["attempts"] = attempts;
        record["omitted_attempts"] = qint64(original - attempts.size());
    }
    if (encode(QJsonArray{record}).size() > limit) return false;
    rows->append(record);
    while (rows->size() > 40 || encode(*rows).size() > limit) rows->removeAt(0);
    const auto bytes = encode(*rows);
    QSaveFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size() && f.commit();
}
} // namespace coarchive
