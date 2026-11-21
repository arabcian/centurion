#include "diagnostictools.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>

#include <algorithm>

namespace {
constexpr qint64 MAX_READ = 128 * 1024;
QString read(const QString &path, qint64 limit = MAX_READ) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(file.read(limit)).trimmed();
}
QString baseName(const QString &path) { return QFileInfo(path).fileName(); }
QString snapPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
           QStringLiteral("/centurion/hardware-baseline.json");
}
QString normCmdline() {
    // Never persist boot secrets or encryption key material. Compare safe
    // parameter NAMES and values only for an allowlist of hardware settings.
    const QSet<QString> allow = {"pcie_aspm", "iommu", "amd_iommu", "intel_iommu", "amd_pstate",
        "nvme_core.default_ps_max_latency_us", "amdgpu.dc", "nvidia-drm.modeset", "nvidia_drm.modeset",
        "nvidia.NVreg_EnableGpuFirmware", "nvidia.NVreg_PreserveVideoMemoryAllocations"};
    QStringList flags;
    for (const QString &token : read(QStringLiteral("/proc/cmdline")).split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        const QString name = token.section(QLatin1Char('='), 0, 0);
        if (!QRegularExpression(QStringLiteral("^[a-zA-Z0-9_.-]{1,80}$")).match(name).hasMatch()) continue;
        flags << (allow.contains(name) ? token.left(120) : name + QStringLiteral("=<redacted>"));
    }
    flags.sort();
    return flags.join(QLatin1Char(' '));
}
QJsonObject snapshot() {
    QJsonObject obj;
    obj.insert(QStringLiteral("host/kernel"), read(QStringLiteral("/proc/sys/kernel/osrelease")));
    obj.insert(QStringLiteral("host/cmdline"), normCmdline());
    obj.insert(QStringLiteral("host/bios_version"), read(QStringLiteral("/sys/class/dmi/id/bios_version")));
    obj.insert(QStringLiteral("host/bios_date"), read(QStringLiteral("/sys/class/dmi/id/bios_date")));
    QSet<QString> mc;
    const QRegularExpression mcRe(QStringLiteral(R"(^microcode\s*:\s*(\S+))"), QRegularExpression::MultilineOption);
    auto it = mcRe.globalMatch(read(QStringLiteral("/proc/cpuinfo")));
    while (it.hasNext()) mc.insert(it.next().captured(1));
    QStringList microcodes = mc.values(); microcodes.sort();
    obj.insert(QStringLiteral("host/microcode"), microcodes.join(QStringLiteral(", ")));
    const QDir pci(QStringLiteral("/sys/bus/pci/devices"));
    const QStringList devices = pci.entryList(QDir::Dirs | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &dev : devices.mid(0, 512)) {
        const QString path = pci.filePath(dev) + QLatin1Char('/');
        const QString k = QStringLiteral("pci/") + dev + QLatin1Char('/');
        for (const QString &name : {"vendor", "device", "class", "subsystem_vendor", "subsystem_device",
                                    "current_link_speed", "current_link_width", "max_link_speed", "max_link_width"}) {
            const QString val = read(path + name);
            if (!val.isEmpty()) obj.insert(k + name, val);
        }
        const QFileInfo drv(path + QStringLiteral("driver"));
        if (drv.exists() && drv.isSymLink()) obj.insert(k + QStringLiteral("driver"), baseName(drv.canonicalFilePath()));
    }
    // Only loaded modules: built-in modules aren't present in /proc/modules.
    for (const QString &line : read(QStringLiteral("/proc/modules")).split(QLatin1Char('\n'))) {
        const QString module = line.section(QLatin1Char(' '), 0, 0);
        if (module.isEmpty() || !QRegularExpression(QStringLiteral("^[a-zA-Z0-9_]+$")).match(module).hasMatch()) continue;
        const QString version = read(QStringLiteral("/sys/module/") + module + QStringLiteral("/version"));
        obj.insert(QStringLiteral("module/") + module, version.isEmpty() ? QStringLiteral("loaded; version unavailable") : version);
    }
    return obj;
}
QString pretty(const QJsonObject &data) {
    QStringList lines;
    for (auto it = data.begin(); it != data.end(); ++it)
        lines << QStringLiteral("%1 = %2").arg(it.key(), it.value().toString());
    return lines.join(QLatin1Char('\n'));
}
QString baselineError(const QString &file) {
    return QStringLiteral("No baseline available at %1. Press 'Save baseline' to capture one.\n").arg(file);
}

quint8 byte(const QByteArray &bytes, int offset) { return static_cast<quint8>(bytes.at(offset)); }
QString edidOne(const QByteArray &data) {
    const QByteArray header = QByteArray::fromHex("00ffffffffffff00");
    if (data.size() < 128) return QStringLiteral("EDID unavailable or shorter than 128 bytes.\n");
    QStringList result;
    if (!data.startsWith(header)) result << QStringLiteral("WARNING: EDID header invalid");
    const int declared = byte(data, 126) + 1;
    const int available = data.size() / 128;
    result << QStringLiteral("Blocks: declared %1, readable %2; checksum status:").arg(declared).arg(available);
    for (int block = 0; block < std::min(available, 32); ++block) {
        unsigned int sum = 0;
        for (int i = block * 128; i < (block + 1) * 128; ++i) sum += byte(data, i);
        const QString kind = block == 0 ? QStringLiteral("base") :
            byte(data, block * 128) == 0x02 ? QStringLiteral("CTA-861") :
            byte(data, block * 128) == 0x70 ? QStringLiteral("DisplayID") : QStringLiteral("other extension");
        result << QStringLiteral("  [%1] %2 : %3").arg(block).arg(kind, sum % 256 == 0 ? QStringLiteral("valid") : QStringLiteral("INVALID"));
    }
    if (available < declared) result << QStringLiteral("WARNING: missing/truncated extension block(s)");
    // First detailed timing descriptor is normally the preferred timing. It is
    // not a declaration of VRR capability or a minimum/maximum refresh rate.
    for (int offset = 54; offset <= 108; offset += 18) {
        if (byte(data, offset) == 0 && byte(data, offset+1) == 0) {
            if (byte(data, offset+3) == 0xfd) {
                result << QStringLiteral("EDID monitor vertical-frequency range: %1–%2 Hz (NOT necessarily VRR)")
                    .arg(byte(data, offset+5)).arg(byte(data, offset+6));
            }
            continue;
        }
        const int clk = byte(data, offset) | (byte(data, offset+1) << 8);
        const int ha = byte(data, offset+2) | ((byte(data, offset+4) & 0xf0) << 4);
        const int hb = byte(data, offset+3) | ((byte(data, offset+4) & 0x0f) << 8);
        const int va = byte(data, offset+5) | ((byte(data, offset+7) & 0xf0) << 4);
        const int vb = byte(data, offset+6) | ((byte(data, offset+7) & 0x0f) << 8);
        if (clk && (ha+hb) && (va+vb)) {
            const double hz = clk * 10000.0 / ((ha + hb) * (va + vb));
            result << QStringLiteral("Detailed timing: %1×%2 @ %3 Hz (EDID timing, not VRR)")
                .arg(ha).arg(va).arg(hz, 0, 'f', 2);
        }
    }
    return result.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

struct Param { QString source, value; };
using Params = QMap<QString, Param>;
Params configParams() {
    Params params;
    for (const QString &path : {"/usr/lib/modprobe.d", "/lib/modprobe.d", "/run/modprobe.d", "/etc/modprobe.d"}) {
        const QDir d(path);
        for (const QString &file : d.entryList({QStringLiteral("*.conf")}, QDir::Files, QDir::Name).mid(0, 128)) {
            const QMap<QString, QString> options = diagnostictools::parseNvidiaOptionsText(read(d.filePath(file), 256 * 1024));
            for (auto it = options.cbegin(); it != options.cend(); ++it)
                params.insert(it.key(), {d.filePath(file), it.value().left(200)});
        }
    }
    for (const QString &token : read(QStringLiteral("/proc/cmdline")).split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        const auto match = QRegularExpression(QStringLiteral(R"(^(nvidia(?:_drm|_uvm|_modeset|\-drm|\-uvm|\-modeset)?)\.([A-Za-z0-9_]+)=([^\s]+)$)")).match(token);
        if (match.hasMatch()) params.insert(match.captured(1).replace(QLatin1Char('-'), QLatin1Char('_')) + QLatin1Char('/') + match.captured(2), {QStringLiteral("/proc/cmdline"), match.captured(3).left(200)});
    }
    return params;
}
QString procNvValue(const QString &key, const QMap<QString, QString> &proc) {
    if (proc.contains(key)) return proc.value(key);
    if (key.startsWith(QStringLiteral("NVreg_"))) return proc.value(key.mid(6));
    return {};
}
}  // namespace

namespace diagnostictools {
QString inspectEdidBytes(const QByteArray &data) { return edidOne(data); }

QMap<QString,QString> parseNvidiaOptionsText(const QString &raw) {
    QMap<QString, QString> result;
    QString content = raw.left(256 * 1024);
    content.replace(QStringLiteral("\\\n"), QStringLiteral(" "));
    for (QString line : content.split(QLatin1Char('\n'))) {
        line = line.section(QLatin1Char('#'), 0, 0).trimmed();
        const QRegularExpressionMatch m = QRegularExpression(QStringLiteral(R"(^options\s+(\S+)\s+(.+)$)"),
            QRegularExpression::CaseInsensitiveOption).match(line);
        if (!m.hasMatch()) continue;
        const QString module = m.captured(1).replace(QLatin1Char('-'), QLatin1Char('_'));
        if (!module.startsWith(QStringLiteral("nvidia"))) continue;
        const QRegularExpression arg(QStringLiteral(R"(([^\s=]+)=("[^"]*"|'[^']*'|[^\s]+))"));
        auto match = arg.globalMatch(m.captured(2));
        while (match.hasNext()) {
            const auto x = match.next();
            const QString name = x.captured(1);
            if (!QRegularExpression(QStringLiteral("^[a-zA-Z0-9_]+$")).match(name).hasMatch()) continue;
            QString v = x.captured(2);
            if (v.size() > 1 && (v.front() == QLatin1Char('"') || v.front() == QLatin1Char('\''))) v = v.mid(1,v.size()-2);
            result.insert(module + QLatin1Char('/') + name, v);
        }
    }
    return result;
}

QString snapshotDifferenceText(const QJsonObject &before, const QJsonObject &after) {
    QStringList lines;
    int changes = 0;
    QSet<QString> keys;
    for (auto i = before.begin(); i != before.end(); ++i) keys.insert(i.key());
    for (auto i = after.begin(); i != after.end(); ++i) keys.insert(i.key());
    QStringList sorted = keys.values(); sorted.sort();
    for (const QString &key : sorted) {
        if (before.value(key) == after.value(key)) continue;
        ++changes;
        if (lines.size() >= 250) continue;
        if (!before.contains(key)) lines << QStringLiteral("+ %1 = %2").arg(key, after.value(key).toString());
        else if (!after.contains(key)) lines << QStringLiteral("- %1 = %2").arg(key, before.value(key).toString());
        else lines << QStringLiteral("~ %1\n    before: %2\n    after : %3").arg(key, before.value(key).toString(), after.value(key).toString());
    }
    return QStringLiteral("Changed fields: %1\n%2").arg(changes).arg(lines.isEmpty() ?
        QStringLiteral("(no differences)") : lines.join(QLatin1Char('\n')));
}

QString hardwarePreview() {
    return QStringLiteral("HARDWARE INVENTORY — READ ONLY\n"
        "This inventory contains no serial numbers or MAC addresses. Kernel cmdline values are redacted except allowlisted flags.\n"
        "No baseline is changed by 'Run again'. Press Save baseline to store a private snapshot; Compare uses the saved one.\n\n") +
        pretty(snapshot()) + QLatin1Char('\n');
}
QString saveHardwareBaseline() {
    const QString filename = snapPath();
    if (!QDir().mkpath(QFileInfo(filename).absolutePath())) return QStringLiteral("Cannot create baseline directory");
    QSaveFile f(filename);
    if (!f.open(QIODevice::WriteOnly)) return QStringLiteral("Cannot open baseline file: ") + f.errorString();
    QJsonObject o;
    o.insert(QStringLiteral("schema"), 1);
    o.insert(QStringLiteral("captured_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    o.insert(QStringLiteral("items"), snapshot());
    const QByteArray raw = QJsonDocument(o).toJson(QJsonDocument::Indented);
    if (raw.size() > 1024*1024 || f.write(raw) != raw.size() || !f.commit())
        return QStringLiteral("Could not write baseline atomically");
    QFile::setPermissions(filename, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return QStringLiteral("Baseline saved: %1\nCaptured: %2\nOnly the user-requested inventory snapshot was written.\n")
        .arg(filename, o.value(QStringLiteral("captured_at")).toString());
}
QString compareHardwareBaseline() {
    const QString filename = snapPath();
    QFile f(filename);
    if (!f.open(QIODevice::ReadOnly)) return baselineError(filename);
    const auto data = f.read(1024*1024 + 1);
    if (data.size() > 1024*1024) return QStringLiteral("Baseline too large; refusing to parse");
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject() || doc.object().value(QStringLiteral("schema")).toInt() != 1 ||
        !doc.object().value(QStringLiteral("items")).isObject()) return QStringLiteral("Invalid or unsupported baseline");
    const QJsonObject before = doc.object().value(QStringLiteral("items")).toObject();
    const QJsonObject after = snapshot();
    return QStringLiteral("HARDWARE SNAPSHOT DIFFERENCE\nBaseline: %1\nCurrent: %2\n"
        "Changed PCI BDF may mean re-enumeration, not replacement. Idle PCIe speed changes can be normal.\n"
        "Missing readings may indicate permission/driver changes, not failed hardware.\n\n%3\n")
        .arg(doc.object().value(QStringLiteral("captured_at")).toString(),
             QDateTime::currentDateTimeUtc().toString(Qt::ISODate), snapshotDifferenceText(before, after));
}
QString edidReport() {
    QStringList out = {QStringLiteral("EDID / VRR INSPECTOR — READ ONLY"),
        QStringLiteral("Reports the EDID exposed by DRM (may be overridden). It does NOT read a separate physical-panel EEPROM."),
        QStringLiteral("An EDID vertical-frequency range is not proof of an Adaptive-Sync/VRR range. No EDID writes occur.\n")};
    const QDir d(QStringLiteral("/sys/class/drm"));
    int count = 0;
    for (const QString &conn : d.entryList(QDir::Dirs | QDir::System | QDir::NoDotAndDotDot, QDir::Name)) {
        if (!conn.contains(QLatin1Char('-'))) continue;
        const QString root = d.filePath(conn) + QLatin1Char('/');
        if (!QFile::exists(root + QStringLiteral("status"))) continue;
        const QString status = read(root + QStringLiteral("status"));
        out << QStringLiteral("== %1 (%2) ==").arg(conn, status);
        if (status != QStringLiteral("connected")) { out << QStringLiteral("Disconnected; no EDID read.\n"); continue; }
        QFile f(root + QStringLiteral("edid"));
        if (!f.open(QIODevice::ReadOnly)) out << QStringLiteral("EDID inaccessible/unsupported");
        else out << edidOne(f.read(4097));
        const QString modes = read(root + QStringLiteral("modes"), 8192);
        out << QStringLiteral("DRM advertised modes:\n%1").arg(modes.isEmpty() ? QStringLiteral("(unavailable)") : modes);
        const QString vrr = read(root + QStringLiteral("vrr_capable"));
        out << QStringLiteral("DRM connector VRR capability: %1\n").arg(vrr.isEmpty() ? QStringLiteral("Unknown (not exported by driver)") : vrr);
        if (++count >= 32) break;
    }
    if (!count) out << QStringLiteral("No DRM connectors found. This does not prove lack of VRR support.");
    return out.join(QLatin1Char('\n')) + QLatin1Char('\n');
}
QString moduleParameterReport() {
    QStringList out = {QStringLiteral("NVIDIA / KERNEL MODULE PARAMETER COMPARISON — READ ONLY"),
        QStringLiteral("The configured value may apply only after module reload/reboot. Only exposed runtime values can be verified."),
        QStringLiteral("A missing runtime parameter is UNKNOWN, not automatically a mismatch.\n")};
    QMap<QString, QString> proc;
    for (const QString &line : read(QStringLiteral("/proc/driver/nvidia/params")).split(QLatin1Char('\n'))) {
        const int i = line.indexOf(QLatin1Char(':'));
        if (i > 0) proc.insert(line.left(i).trimmed(), line.mid(i+1).trimmed().remove(QLatin1Char('"')));
    }
    const Params config = configParams();
    if (config.isEmpty()) out << QStringLiteral("No configured NVIDIA module options found in scanned modprobe.d files or kernel cmdline.");
    for (auto it = config.begin(); it != config.end(); ++it) {
        const QString mod = it.key().section(QLatin1Char('/'), 0, 0);
        const QString key = it.key().section(QLatin1Char('/'), 1);
        const QString root = QStringLiteral("/sys/module/") + mod;
        QString live = read(root + QStringLiteral("/parameters/") + key);
        QString source = QStringLiteral("sysfs");
        if (live.isEmpty() && mod == QStringLiteral("nvidia")) {
            live = procNvValue(key, proc);
            source = QStringLiteral("/proc/driver/nvidia/params");
        }
        const QString verdict = live.isEmpty() ? QStringLiteral("UNKNOWN (not exposed)") :
            live == it.value().value ? QStringLiteral("MATCH") : QStringLiteral("DIFFERENT / format may differ");
        out << QStringLiteral("%1\n  configured: %2 (from %3)\n  effective : %4 (%5)\n  result    : %6\n")
               .arg(it.key(), it.value().value, it.value().source,
                    live.isEmpty() ? QStringLiteral("Unknown") : live, source, verdict);
    }
    out << QStringLiteral("Additional runtime NVIDIA fields (read from /proc; may be driver defaults):");
    if (proc.isEmpty()) out << QStringLiteral("(NVIDIA proc parameters unavailable; no GPU wake attempted)");
    for (auto it = proc.begin(); it != proc.end(); ++it)
        out << QStringLiteral("  %1 = %2").arg(it.key(), it.value());
    return out.join(QLatin1Char('\n')) + QLatin1Char('\n');
}
} // namespace diagnostictools
