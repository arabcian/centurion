#pragma once
// Local evidence archive, deliberately separate from the recovery journal.
#include <QFile>
#include <QMap>
#include <QCoreApplication>
#include <QStringList>
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
inline QStringList compare(const QJsonObject &a, const QJsonObject &b) {
    auto tr=[](const char *s){return QCoreApplication::translate("CoSearchDialog",s);};
    const auto fp=a.value("cpu_fingerprint").toString();
    if (fp.isEmpty() || fp!=b.value("cpu_fingerprint").toString()) return {tr("Unknown or different CPU topology. Per-core comparison is unavailable.")};
    auto decode=[](const QJsonObject &r,QMap<QString,int>& values){
        const auto rows=r.value("topology").toArray(); if(rows.isEmpty()||rows.size()>512)return false;
        for(const auto &v:rows){const auto c=v.toObject(); const auto ccd=c.value("ccd").toInteger(-1), slot=c.value("slot").toInteger(-1), val=c.value("value").toInteger(1);
            if(ccd<0||slot<0||val>0||val< -50)return false;
            const auto key=QStringLiteral("CCD %1 / %2").arg(ccd).arg(slot); if(values.contains(key))return false; values.insert(key,int(val));}
        return true;
    };
    QMap<QString,int> x,y;
    if (!decode(a,x)||!decode(b,y)||x.keys()!=y.keys())return {tr("Missing or ambiguous core identity.")};
    QStringList out{tr("Recorded offsets only; this is not a stability comparison.")};
    if(a.value("bios")!=b.value("bios"))out<<tr("BIOS versions differ.");
    if(a.value("mode")!=b.value("mode"))out<<tr("Search modes differ.");
    for(auto it=x.cbegin();it!=x.cend();++it) if(it.value()!=y.value(it.key()))out<<QStringLiteral("%1: %2 → %3").arg(it.key()).arg(it.value()).arg(y.value(it.key()));
    if(x==y)out<<tr("No recorded offset differences.");
    return out;
}
// Compare the entire displayed snapshot under the same lock as append. A new
// session or concurrent deletion invalidates the old row index (including legacy IDs).
inline bool remove(const QString &path, const QJsonArray &expected, qsizetype index) {
    QLockFile lock(path + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) return false;
    auto rows = read(path);
    if (!rows || *rows != expected || index < 0 || index >= rows->size()) return false;
    rows->removeAt(index);
    const auto bytes = encode(*rows);
    QSaveFile f(path);
    return f.open(QIODevice::WriteOnly) && f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        && f.write(bytes) == bytes.size() && f.commit();
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
    return f.open(QIODevice::WriteOnly) && f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) && f.write(bytes) == bytes.size() && f.commit();
}
} // namespace coarchive
