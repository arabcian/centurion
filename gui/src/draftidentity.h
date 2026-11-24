#pragma once
// Shared by the normal draft editor and startup restore. No hardware access.
#include <QString>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <optional>
namespace draftidentity {
inline constexpr qint64 fileLimit=1LL<<20;
struct Snapshot {
    bool exists=false;
    QByteArray bytes;
    bool operator==(const Snapshot &) const = default;
};
inline std::optional<Snapshot> snapshot(const QString &path) {
    QFileInfo info(path);
    if (!info.exists() && !info.isSymLink()) return Snapshot{};
    if (!info.isFile() || info.isSymLink()) return std::nullopt;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size()>fileLimit) return std::nullopt;
    const auto b=f.read(fileLimit+1);
    if (f.error()!=QFileDevice::NoError || b.size()>fileLimit || !f.atEnd()) return std::nullopt;
    return Snapshot{true,b};
}
inline QJsonObject record(const std::optional<Snapshot> &s) {
    return s && s->exists ? QJsonDocument::fromJson(s->bytes).object() : QJsonObject{};
}
inline bool legacyKey(const QString &key) { return key.startsWith(QStringLiteral("vm.")) || key.startsWith(QStringLiteral("sched.")); }
inline QString identity(const QString &key, const QJsonValue &value) {
    QString source = value.toString();
    // Preserve the older VM/scheduler editor scope, including old helpers.
    if (source.isEmpty() && legacyKey(key)) source = QStringLiteral("legacy:") + key;
    if (source.isEmpty() || source.size() > 2048) return {};
    return QString::fromLatin1(QCryptographicHash::hash(source.toUtf8(), QCryptographicHash::Sha256).toHex());
}
inline bool validHash(const QString &s) {
    if (s.size()!=64) return false;
    for (QChar c:s) if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) return false;
    return true;
}
inline bool compatible(int version, const QString &key, const QJsonObject &saved, const QString &current) {
    if (version==1) return legacyKey(key);
    return version==2 && validHash(current) && saved.value("identity").toString()==current;
}
inline bool validRecord(const QJsonObject &record, const QString &fingerprint) {
    const int version=record.value("version").toInt(-1);
    if (fingerprint.isEmpty() || (version!=1 && version!=2) || record.value("fingerprint").toString()!=fingerprint || !record.value("values").isObject()) return false;
    const auto values=record.value("values").toObject();
    if (values.size()>512) return false;
    for (auto it=values.constBegin();it!=values.constEnd();++it) {
        if (it.key().isEmpty() || it.key().size()>256 || !it.value().isObject()) return false;
        const auto row=it.value().toObject();
        for (const auto *field : {"draft","live","boot"}) if (!row.value(QLatin1String(field)).isString() || row.value(QLatin1String(field)).toString().size()>4096) return false;
        if (row.value("draft").toString().isEmpty() || (version==2 && !validHash(row.value("identity").toString()))) return false;
    }
    return true;
}
}
