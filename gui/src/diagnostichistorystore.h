#pragma once

#include "diagnostichistorydata.h"

#include <QLockFile>

namespace diagnostichistory {

inline QString archiveDirectory() {
    return diagsrc::dataDir() + QStringLiteral("/diagnostic-history");
}

inline std::optional<QJsonArray> decodeArchive(const QJsonObject &archive, const QString &code) {
    if (!validArchive(archive)) return std::nullopt;
    const auto entries = archive.value("entries").toArray();
    QSet<QString> ids;
    for (const auto &value : entries) {
        const auto entry = value.toObject();
        const auto id = full(entry.value("id").toString());
        if (!validEntry(entry) || ids.contains(id) || entry.value("snapshot").toObject().value("code").toString() != code)
            return std::nullopt;
        ids.insert(id);
    }
    return entries;
}

inline std::optional<QJsonArray> readArchive(const QString &dir, const QString &code) {
    const QFileInfo file(QDir(dir).filePath(archiveFileName(code)));
    if (file.isSymLink()) return std::nullopt;
    if (!file.exists()) return QJsonArray{};
    return decodeArchive(diagsrc::loadJson(file.filePath(), ArchiveFileBytes), code);
}

inline QJsonObject archiveObject(const QJsonArray &entries) {
    return {{"version", 1}, {"entries", entries}};
}

inline QString archiveLockPath(const QString &dir) {
    return QDir(dir).filePath(QStringLiteral("archive.lock"));
}

enum class SaveArchive { Saved, Full, WriteError };

// The caller holds the archive lock. Refuse growth past the reader's limits;
// allow shrinking a previously oversized archive.
inline SaveArchive writeArchive(const QString &dir, const QString &code, const QJsonArray &entries) {
    const auto filename = archiveFileName(code);
    const QDir archive(dir);
    if (entries.isEmpty()) {
        return QFile::remove(archive.filePath(filename)) ? SaveArchive::Saved : SaveArchive::WriteError;
    }
    const auto document = archiveObject(entries);
    const qint64 size = QJsonDocument(document).toJson(QJsonDocument::Compact).size();
    if (entries.size() > ArchiveEntriesPerReport || size > ArchiveFileBytes) return SaveArchive::Full;

    const auto files = archive.entryInfoList({QStringLiteral("*.json")}, QDir::Files | QDir::NoSymLinks);
    int count = 1;
    qint64 total = size;
    qint64 replaced = 0;
    for (const auto &file : files) {
        if (file.fileName() == filename) { replaced = file.size(); continue; }
        ++count;
        total += std::min(file.size(), ArchiveBytes);
    }
    if ((count > ArchiveFiles || total > ArchiveBytes) && (replaced == 0 || size > replaced))
        return SaveArchive::Full;
    return diagsrc::saveJson(archive.filePath(filename), document).isEmpty()
        ? SaveArchive::Saved : SaveArchive::WriteError;
}

inline bool removeImported(const QString &dir, const QString &code, const QString &id) {
    QLockFile lock(archiveLockPath(dir));
    if (!lock.tryLock(0)) return false;
    auto entries = readArchive(dir, code);
    if (!entries) return false;
    for (qsizetype i = 0; i < entries->size(); ++i) {
        const auto entry = entries->at(i).toObject();
        if (full(entry.value("id").toString()) != id || !entry.value("imported").toBool()) continue;
        entries->removeAt(i);
        return writeArchive(dir, code, *entries) == SaveArchive::Saved;
    }
    return false;
}

enum class ImportArchive { Imported, AlreadyPresent, Invalid, Conflict, Full, Busy, WriteError };

inline ImportArchive importArchive(const QString &dir, const QString &path) {
    const auto input = diagsrc::loadJson(path, ArchiveFileBytes);
    const auto inputEntries = input.value("entries").toArray();
    if (inputEntries.isEmpty()) return ImportArchive::Invalid;
    const auto code = inputEntries.first().toObject().value("snapshot").toObject().value("code").toString();
    const auto incoming = decodeArchive(input, code);
    if (!incoming || incoming->isEmpty()) return ImportArchive::Invalid;
    if (!QDir().mkpath(dir)) return ImportArchive::WriteError;
    QLockFile lock(archiveLockPath(dir));
    if (!lock.tryLock(0)) return ImportArchive::Busy;

    auto current = readArchive(dir, code);
    if (!current) return ImportArchive::Conflict;
    const auto archive = load(dir);
    if (archive.limited) return ImportArchive::Full;
    QSet<QString> foreignIds;
    for (const auto &entry : archive.entries)
        if (entry.code != code) foreignIds.insert(entry.id);

    const auto before = current->size();
    for (const auto &value : *incoming) {
        const auto entry = value.toObject();
        const auto id = full(entry.value("id").toString());
        if (foreignIds.contains(id) || archive.ambiguousIds.contains(id)) return ImportArchive::Conflict;
        const auto snap = entry.value("snapshot").toObject();
        const QJsonObject snapshot{{"version", 1}, {"code", code}, {"title", snap.value("title")},
                                   {"verdict", snap.value("verdict")}, {"records", snap.value("records")}};
        const QJsonObject incomingEntry{{"id", id}, {"boot", entry.value("boot").toString().left(80)},
            {"time", entry.value("time").toString().left(80)}, {"locale", entry.value("locale").toString().left(32)},
            {"note", diag::redactPrivateText(entry.value("note").toString()).left(4096)},
            {"snapshot", snapshot}, {"imported", true}};
        bool duplicate = false;
        for (const auto &existing : *current) {
            const auto old = existing.toObject();
            if (full(old.value("id").toString()) != id) continue;
            if (old.value("snapshot").toObject() != snapshot
                || old.value("boot").toString() != incomingEntry.value("boot").toString()
                || old.value("time").toString() != incomingEntry.value("time").toString()
                || old.value("locale").toString() != incomingEntry.value("locale").toString())
                return ImportArchive::Conflict;
            duplicate = true;
            break;
        }
        if (!duplicate) current->append(incomingEntry);
    }
    if (current->size() == before) return ImportArchive::AlreadyPresent;
    switch (writeArchive(dir, code, *current)) {
    case SaveArchive::Saved: return ImportArchive::Imported;
    case SaveArchive::Full: return ImportArchive::Full;
    case SaveArchive::WriteError: return ImportArchive::WriteError;
    }
    Q_UNREACHABLE();
}

} // namespace diagnostichistory
