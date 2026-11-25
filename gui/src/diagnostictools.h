#pragma once
// Health → System tools: Hardware snapshot (S01), Displays / EDID / VRR (S03), NVIDIA module parameters (S06).
// Read-only (sysfs, procfs, modprobe.d); the S01 baseline is the only thing written, and only on request.
#include "diagreport.h"

#include <QByteArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>

namespace edidfields { enum class Kind; }

namespace diagnostictools {

// S01. The inventory compared against the baseline: no serial numbers, MACs or UUIDs; boot parameters
// redacted except an allowlist; no link speeds (they change with power saving).
QJsonObject snapshot();
/// /proc/cmdline for the snapshot: every parameter readable, values that identify a disk, a LUKS volume or a
/// network address (root=, resume=, cryptdevice=, rd.luks.*, ip=, UUIDs, MACs) shown as "name=…".
QString bootParameters(const QString &raw);
diag::Report hardwareSnapshot(const QJsonObject &baseline, const QString &capturedAt, const QJsonObject &now);
/// Empty = saved, else the reason.
QString saveHardwareBaseline();
/// The saved baseline's items; empty with `error` set when there is none or it is unreadable.
QJsonObject loadHardwareBaseline(QString *capturedAt, QString *error);

// S03.
struct Edid {
    bool header = false, truncated = false, structuralValid = false;
    int declared = 0, readable = 0;
    QStringList badBlocks;            // "1 (CTA-861)" for every block whose checksum fails
    QString maker, name;              // three-letter PNP id, monitor name / panel text
    int year = 0;
    QStringList timings;              // "2560×1600 @ 240.00 Hz"
    int rangeMin = 0, rangeMax = 0;   // range-limits descriptor (vertical, Hz)
    int vrrMin = 0, vrrMax = 0;       // FreeSync or HDMI VRR block
    QString vrrSource;
    QStringList extensions;           // "CTA-861", "DisplayID"
    int widthCm = 0, heightCm = 0, week = 0, bitDepth = 0;  // physical size, manufacture week, bits per colour (digital inputs)
    double gamma = 0;
    bool audio = false, ycc444 = false, ycc422 = false, hdmi = false;  // CTA-861 capabilities, HDMI vendor block
    QStringList hdr, colorimetry;     // "HDR10 (PQ)", "HLG"; "BT.2020 RGB", "opRGB"
    double maxNits = 0, minNits = 0;  // HDR static metadata: peak and black level, cd/m²
};
Edid decodeEdid(const QByteArray &data);
QString edidFieldName(edidfields::Kind kind);
diag::Report displays(const QString &root = {});

// S06.
QMap<QString, QString> parseNvidiaOptionsText(const QString &raw);
/// Same setting value however written: 0x02 = 2, Y = 1, N = 0, quotes ignored.
bool sameValue(const QString &a, const QString &b);
diag::Report nvidiaParams(const QString &root = {});

}  // namespace diagnostictools
