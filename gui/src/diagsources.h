#pragma once
// Read-only sources shared by the Health diagnostics (Evidence Lab, Investigations, System tools): bounded
// file reads, the logs Centurion saves (health.log: classified kernel faults; kernel.log: every kernel
// warning), private per-user records, and human names for PCI devices. Qt Core only.
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace diagsrc {

/// The file's text, trimmed; empty when missing or unreadable. At most `limit` bytes.
QString read(const QString &path, qint64 limit = 64 * 1024);
/// Entry names of a directory (files, dirs and sysfs links), sorted, at most `max`.
QStringList dir(const QString &path, const QStringList &filter = {QStringLiteral("*")}, int max = 512);
/// $XDG_STATE_HOME/centurion (health.log, kernel.log, CO Search records).
QString stateDir();
/// ~/.local/share/centurion (the diagnostics' own private records).
QString dataDir();
QString bootId();
QJsonObject loadJson(const QString &path, qint64 limit = 1 << 20);
/// Atomic, owner-only. Empty = saved, else the reason.
QString saveJson(const QString &path, const QJsonObject &o);

/// One saved log line. health.log: "<time>  boot=<8> seq=<n>  LEVEL KIND  title  |  text";
/// kernel.log: "<time>  boot=<8> seq=<n>  LEVEL  [s.us] message" (kind and title empty).
struct LogLine {
    QString time, boot, level, kind, title, text;
    quint64 seq = 0;
    QString key() const { return boot + QLatin1Char('/') + QString::number(seq); }
};
std::optional<LogLine> parseLine(const QString &line);
/// The newest `max` lines of health.log.1 + health.log, oldest first.
QList<LogLine> savedEvents(int max = 3000);
/// The newest `max` lines of kernel.log.1 + kernel.log, oldest first.
QList<LogLine> savedKernel(int max = 4000);
/// Lines of both logs, one per boot/seq (the same kernel record is in both).
QList<LogLine> mergeUnique(const QList<LogLine> &a, const QList<LogLine> &b);

/// "NVIDIA GeForce RTX 5080 Max-Q" from pci.ids when installed, else the PCI class ("VGA controller").
QString pciName(const QString &vendor, const QString &device, const QString &pciClass);
/// The PCI class code ("0x030000") as words.
QString pciClassName(const QString &pciClass);
/// Seconds since the epoch as local "yyyy-MM-dd HH:mm"; "—" for 0.
QString when(qint64 secs);
/// "3 h 12 min", "45 s".
QString duration(qint64 secs);

}  // namespace diagsrc
