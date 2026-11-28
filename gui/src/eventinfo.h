#pragma once
// What a classified kernel event (health.rs: kind, code, title, text) means, what usually causes it,
// what to do about it, and the details its own record carries (process, device, kernel function).
// Pure text, no I/O: shared by Health → Events & alerts and the kernel-log page.
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace eventinfo {

struct Info {
    QString meaning;         // one paragraph: what happened
    QStringList causes;      // usual causes, most likely first
    QStringList steps;       // what to do, in order
    QStringList facts;       // "Process: firefox (pid 1234)" — read from the record itself
    QString urgency;         // one line: how worried to be
};

/// The explanation of one event object ({"kind","code","title","text"}).
Info explain(const QJsonObject &event);
/// The same, as HTML for a text browser (headings, lists; raw record escaped).
QString html(const QJsonObject &event, const QString &headline);
/// A kernel-log line classified the way health.rs does (kind, code, title, text), or empty when it is
/// not one of the known event types. Lets the kernel-log page explain any line.
QJsonObject classifyLine(const QString &message);

/// The file system and kernel device name a file system's kernel message names ("EXT4-fs error (device
/// sda1)" → ext4, sda1). Empty fields when the message does not say.
struct FsRef { QString fstype, dev; };
FsRef fsOfMessage(const QString &message);
/// What to do about errors on a file system of this type: back up, then its own repair tool.
QString fsRepairAction(const QString &fstype);

}  // namespace eventinfo
