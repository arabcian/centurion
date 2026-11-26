#pragma once

#include "diagnostichistorystore.h"

#include <QRegularExpression>
#include <QStringDecoder>

namespace diagnostichistory {

inline constexpr int ImportedLogsPerSource = 1000;
inline constexpr qint64 LogImportBytes = 4LL << 20;

inline QString logArchivePath(const QString &dir) {
    // Keep this typed store outside the v1 report bucket scan.
    return QDir(dir).filePath(QStringLiteral("logs/events.json"));
}

inline QJsonObject encodeLogEvent(const RecordedEvent &event) {
    return {{"id", event.id}, {"source", logName(event.source)}, {"boot", event.reportedBoot},
            {"time", event.time}, {"sequence", QString::number(event.sequence)}, {"level", event.level},
            {"title", event.title}, {"detail", event.detail}};
}

inline std::optional<QList<RecordedEvent>> readImportedLogs(const QString &dir) {
    const QFileInfo file(logArchivePath(dir));
    if (file.isSymLink()) return std::nullopt;
    if (!file.exists()) return QList<RecordedEvent>{};
    const auto archive = diagsrc::loadJson(file.filePath(), ArchiveFileBytes);
    const auto records = archive.value("events").toArray();
    if (archive.value("version").toInt() != 1 || !archive.value("events").isArray()
        || records.size() > 2 * ImportedLogsPerSource) return std::nullopt;

    static const QRegularExpression sha256(QStringLiteral("\\A[0-9a-f]{64}\\z"));
    static const QRegularExpression decimal(QStringLiteral("\\A(?:0|[1-9][0-9]{0,19})\\z"));
    QList<RecordedEvent> out;
    QSet<QString> ids;
    int healthCount = 0, kernelCount = 0;
    for (const auto &value : records) {
        const auto obj = value.toObject();
        const auto source = obj.value("source").toString();
        const bool health = source == logName(RecordSource::HealthLog);
        if (!health && source != logName(RecordSource::KernelLog)) return std::nullopt;
        auto &count = health ? healthCount : kernelCount;
        if (++count > ImportedLogsPerSource) return std::nullopt;
        const auto id = obj.value("id").toString();
        const auto seq = obj.value("sequence").toString();
        bool ok = false;
        const auto sequence = seq.toULongLong(&ok);
        if (!sha256.match(id).hasMatch() || ids.contains(id) || !ok || !decimal.match(seq).hasMatch())
            return std::nullopt;
        for (const auto *field : {"boot", "time", "level", "title", "detail"})
            if (!obj.value(field).isString()) return std::nullopt;
        ids.insert(id);
        const auto boot = obj.value("boot").toString();
        out.append({full(boot), boot.left(80), obj.value("time").toString().left(80),
                    obj.value("level").toString().left(80), obj.value("title").toString().left(300),
                    obj.value("detail").toString().left(8192), id, sequence,
                    health ? RecordSource::HealthLog : RecordSource::KernelLog, false, false, true});
    }
    return out;
}

// The caller holds the shared archive lock.
inline SaveArchive writeImportedLogs(const QString &dir, const QList<RecordedEvent> &events) {
    const auto path = logArchivePath(dir);
    if (events.isEmpty()) return QFile::remove(path) ? SaveArchive::Saved : SaveArchive::WriteError;
    QJsonArray records;
    int health = 0, kernel = 0;
    for (const auto &event : events) {
        auto &count = event.source == RecordSource::HealthLog ? health : kernel;
        if (++count > ImportedLogsPerSource) return SaveArchive::Full;
        records.append(encodeLogEvent(event));
    }
    const QJsonObject archive{{"version", 1}, {"events", records}};
    if (QJsonDocument(archive).toJson(QJsonDocument::Compact).size() > ArchiveFileBytes) return SaveArchive::Full;
    return diagsrc::saveJson(path, archive).isEmpty() ? SaveArchive::Saved : SaveArchive::WriteError;
}

struct LogImport {
    ImportArchive status;
    qsizetype added = 0, duplicates = 0, omitted = 0;
};

inline LogImport importSavedLog(const QString &dir, const QString &path) {
    const auto bytes = diagsrc::readRecord(path, LogImportBytes);
    if (!bytes || bytes->isEmpty()) return {ImportArchive::Invalid};
    if (!QDir().mkpath(dir)) return {ImportArchive::WriteError};
    QLockFile lock(archiveLockPath(dir));
    if (!lock.tryLock(0)) return {ImportArchive::Busy};
    auto existing = readImportedLogs(dir);
    if (!existing) return {ImportArchive::Conflict};
    QSet<QString> ids;
    for (const auto &event : *existing) ids.insert(event.id);

    LogImport result{ImportArchive::Invalid};
    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    for (qsizetype start = 0; start < bytes->size();) {
        const auto end = bytes->indexOf('\n', start);
        if (end < 0) { ++result.omitted; break; }
        const QByteArrayView line(bytes->constData() + start, end - start);
        start = end + 1;
        if (line.size() > 8192 || line.contains('\0')) { ++result.omitted; continue; }
        decoder.resetState();
        const QString text = decoder(line);
        const auto parsed = decoder.hasError() ? std::nullopt : diagsrc::parseLine(text.trimmed());
        if (!parsed) { ++result.omitted; continue; }

        auto event = recordedEvent(*parsed, parsed->kind.isEmpty() ? RecordSource::KernelLog : RecordSource::HealthLog);
        if (ids.contains(event.id)) { ++result.duplicates; continue; }
        ids.insert(event.id);
        event.imported = true;
        existing->append(std::move(event));
        ++result.added;
        if (existing->size() > 2 * ImportedLogsPerSource) return {ImportArchive::Full};
    }
    if (!result.added) {
        result.status = result.duplicates ? ImportArchive::AlreadyPresent : ImportArchive::Invalid;
        return result;
    }
    switch (writeImportedLogs(dir, *existing)) {
    case SaveArchive::Saved: result.status = ImportArchive::Imported; return result;
    case SaveArchive::Full: return {ImportArchive::Full};
    case SaveArchive::WriteError: return {ImportArchive::WriteError};
    }
    Q_UNREACHABLE();
}

inline bool removeImportedLog(const QString &dir, const QString &id) {
    QLockFile lock(archiveLockPath(dir));
    if (!lock.tryLock(0)) return false;
    auto entries = readImportedLogs(dir);
    if (!entries || !entries->removeIf([&](const RecordedEvent &entry) { return entry.id == id; })) return false;
    return writeImportedLogs(dir, *entries) == SaveArchive::Saved;
}

} // namespace diagnostichistory
