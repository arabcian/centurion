#pragma once
// Health → System tools: Hardware snapshot (S01), Displays / EDID / VRR (S03), NVIDIA module parameters (S06).
// Read-only (sysfs, procfs, modprobe.d); the S01 baseline is the only thing written, and only on request.
#include "diagreport.h"

#include <QByteArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>

namespace diagnostictools {

// S01. The inventory compared against the baseline: no serial numbers, MACs or UUIDs; boot parameters
// redacted except an allowlist; no link speeds (they change with power saving).
QJsonObject snapshot();
diag::Report hardwareSnapshot(const QJsonObject &baseline, const QString &capturedAt, const QJsonObject &now);
/// Empty = saved, else the reason.
QString saveHardwareBaseline();
/// The saved baseline's items; empty with `error` set when there is none or it is unreadable.
QJsonObject loadHardwareBaseline(QString *capturedAt, QString *error);

// S03.
struct Edid {
    bool header = false, truncated = false;
    int declared = 0, readable = 0;
    QStringList badBlocks;            // "1 (CTA-861)" for every block whose checksum fails
    QString maker, name;              // three-letter PNP id, monitor name / panel text
    int year = 0;
    QStringList timings;              // "2560×1600 @ 240.00 Hz"
    int rangeMin = 0, rangeMax = 0;   // range-limits descriptor (vertical, Hz)
    int vrrMin = 0, vrrMax = 0;       // FreeSync or HDMI VRR block
    QString vrrSource;
    QStringList extensions;           // "CTA-861", "DisplayID"
};
Edid decodeEdid(const QByteArray &data);
diag::Report displays(const QString &root = {});

// S06.
QMap<QString, QString> parseNvidiaOptionsText(const QString &raw);
/// Same setting value however written: 0x02 = 2, Y = 1, N = 0, quotes ignored.
bool sameValue(const QString &a, const QString &b);
diag::Report nvidiaParams(const QString &root = {});

}  // namespace diagnostictools
