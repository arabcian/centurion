#pragma once
// Owner-local evidence only. Never used to authorize writes or restore.
#include "tunereceipt.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <optional>
namespace tunejournal {
inline constexpr qint64 limit = 4LL << 20;
inline constexpr int maxRows = 100;
inline std::optional<QJsonArray> read(const QString &path) {
    QFile f(path);
    if (!f.exists()) return QJsonArray{};
    if (!f.open(QIODevice::ReadOnly) || f.size() > limit) return std::nullopt;
    const auto bytes = f.read(limit + 1);
    const auto doc = QJsonDocument::fromJson(bytes);
    const auto o = doc.object();
    if (bytes.size() > limit || !doc.isObject() || o.value("version").toInt() != 1 || !o.value("transactions").isArray()) return std::nullopt;
    const auto a = o.value("transactions").toArray();
    if (a.size() > maxRows) return std::nullopt;
    QSet<QString> ids;
    for (const auto &v : a) {
        const auto id = v.toObject().value("id").toString();
        if (!v.isObject() || id.isEmpty() || ids.contains(id)) return std::nullopt;
        ids.insert(id);
    }
    return a;
}
inline QByteArray encode(const QJsonArray &a) {
    return QJsonDocument(QJsonObject{{"version",1},{"transactions",a}}).toJson(QJsonDocument::Compact);
}
inline bool store(const QString &path, const QJsonObject &record, bool replace) {
    QLockFile lock(path + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) return false;
    auto a = read(path);
    const auto id = record.value("id").toString();
    if (!a || id.isEmpty() || encode(QJsonArray{record}).size() > limit) return false;
    qsizetype at = -1;
    for (qsizetype i = 0; i < a->size(); ++i)
        if (a->at(i).toObject().value("id").toString() == id) at = i;
    if (replace != (at >= 0)) return false;
    if (replace) a->replace(at,record); else a->append(record);
    while (a->size() > maxRows || encode(*a).size() > limit) {
        // Never evict an unresolved transaction or the record being written.
        // If no completed record can be removed, refuse the append atomically.
        qsizetype victim = -1;
        for (qsizetype i = 0; i < a->size(); ++i) {
            const auto row = a->at(i).toObject();
            if (row.value("id").toString() != id && row.value("reply").toString() != QStringLiteral("pending")
                && row.value("outcome").toString() != QStringLiteral("pending")) { victim = i; break; }
        }
        if (victim < 0) return false;
        a->removeAt(victim);
    }
    const auto bytes = encode(*a);
    QSaveFile f(path);
    return f.open(QIODevice::WriteOnly) && f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) && f.write(bytes) == bytes.size() && f.commit();
}
inline tunereceipt::Status status(const QJsonObject &o) {
    using tunereceipt::Evidence;
    return tunereceipt::classify(Evidence{o.value("ok").isBool(), o.value("ok").toBool(), o.value("skipped").isString(),
        o.value("written").toInteger(-1), o.contains("refused") ? o.value("refused").toInteger(-1) : 0, o.value("readback_expected").toInteger(-1),
        o.value("readback_matched").toInteger(-1), o.value("readback_missing").toInteger(-1), o.value("mismatch_count").toInteger(-1)});
}
// Retain only structured fields, not helper stderr, command lines, paths or raw replies.
inline QJsonObject receipt(const QJsonObject &o) {
    QJsonObject out;
    for (const auto *key : {"key","value","ok","written","refused","readback_expected","readback_matched","readback_missing","mismatch_count","restored","deferred","policy_skipped"}) {
        const auto v = o.value(QLatin1String(key));
        if (v.isBool() || v.isDouble() || (v.isString() && v.toString().size() <= 4096)) out.insert(QLatin1String(key),v);
    }
    if (o.value("skipped").isString()) out.insert("skipped",QStringLiteral("reported"));
    if (o.value("errors").isArray()) out.insert("error_count",o.value("errors").toArray().size());
    return out;
}
}
