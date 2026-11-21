#include "advanceddiagnostics.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QSysInfo>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr qint64 MAX_TEXT = 1024 * 1024;
constexpr int MAX_DIR = 64;
QString rd(const QString &file, qint64 limit = 4096) {
    QFile f(file);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.read(qMin(limit, qint64(2 * 1024 * 1024)))).trimmed() : QString();
}
QString val(const QString &file) {
    const QString v = rd(file);
    return v.isEmpty() ? QStringLiteral("Unknown (missing, not exported, or unreadable)") : v;
}
QStringList entries(const QString &dir, const QStringList &filter = {QStringLiteral("*")}) {
    return QDir(dir).entryList(filter, QDir::Dirs | QDir::Files | QDir::System | QDir::NoDotAndDotDot, QDir::Name).mid(0, MAX_DIR);
}
QString dataDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/centurion");
}
QString stateDir() {
    const QString root = qEnvironmentVariable("XDG_STATE_HOME");
    return (root.isEmpty() ? QDir::homePath() + QStringLiteral("/.local/state") : root) + QStringLiteral("/centurion");
}
QString bootId() { return rd(QStringLiteral("/proc/sys/kernel/random/boot_id"), 80); }
QJsonObject loadObject(const QString &path, qint64 limit = MAX_TEXT) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > limit) return {};
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    return err.error == QJsonParseError::NoError && doc.isObject() ? doc.object() : QJsonObject{};
}
QString saveObject(const QString &path, const QJsonObject &v) {
    const QByteArray b = QJsonDocument(v).toJson(QJsonDocument::Compact);
    if (b.size() > MAX_TEXT || !QDir().mkpath(QFileInfo(path).absolutePath())) return QStringLiteral("Could not prepare private data directory");
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QStringLiteral("Could not open private record");
    if (!f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        f.write(b) != b.size() || !f.commit()) return QStringLiteral("Could not write private record atomically");
    return QString();
}
QString when(qint64 seconds) {
    return seconds <= 0 ? QStringLiteral("Unknown") : QDateTime::fromSecsSinceEpoch(seconds, Qt::UTC).toString(Qt::ISODate);
}
QStringList logTail(const QString &file, int maxLines = 200, qsizetype maxRead = 1 << 20) {
    QStringList out;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return out;
    if (f.size() > maxRead) { f.seek(f.size() - maxRead); f.readLine(8192); }
    while (!f.atEnd()) {
        const QString s = QString::fromUtf8(f.readLine(4096)).trimmed();
        if (!s.isEmpty()) out << s.left(700);
        if (out.size() > maxLines) out.removeFirst();
    }
    return out;
}
QStringList recentLog(int limit=400) {
    QStringList out;
    const QString root = stateDir();
    for (const QString &file : {QStringLiteral("health.log.1"), QStringLiteral("health.log"),
                                QStringLiteral("kernel.log.1"), QStringLiteral("kernel.log")}) {
        out += logTail(root + QLatin1Char('/') + file, limit);
        while (out.size() > limit * 2) out.removeFirst();
    }
    return out;
}
QString reportHeader(const QString &code, const QString &title) {
    return code + QStringLiteral(" — ") + title + QStringLiteral("\nREAD ONLY · ON DEMAND · Missing information means UNKNOWN, not healthy.\n\n");
}
QString join(const QStringList &s) { return s.join(QLatin1Char('\n')) + QLatin1Char('\n'); }
QString status(const QString &p) { return rd(p).isEmpty() ? QStringLiteral("unavailable") : rd(p); }

// Store one observation per boot, not a background daemon and not a synthetic
// retrospective kernel version. For boots never sampled, version remains unknown.
QString contextPath() { return dataDir() + QStringLiteral("/boot-regression-contexts.json"); }
QJsonObject contextsWithCurrent() {
    QJsonObject obj = loadObject(contextPath());
    if (obj.value(QStringLiteral("schema")).toInt() != 1) obj = {{QStringLiteral("schema"), 1}};
    QJsonObject observations = obj.value(QStringLiteral("boots")).toObject();
    const QString id = bootId();
    if (!id.isEmpty()) {
        QJsonObject rec;
        rec.insert(QStringLiteral("kernel"), rd(QStringLiteral("/proc/sys/kernel/osrelease")));
        rec.insert(QStringLiteral("bios"), rd(QStringLiteral("/sys/class/dmi/id/bios_version")));
        const QString nv = rd(QStringLiteral("/proc/driver/nvidia/version"), 512);
        const QRegularExpression version(QStringLiteral(R"(Kernel Module\s+([^\s]+))"));
        const QRegularExpressionMatch m = version.match(nv);
        rec.insert(QStringLiteral("nvidia"), m.hasMatch() ? m.captured(1) : QStringLiteral("Unknown"));
        rec.insert(QStringLiteral("observed_at"), when(QDateTime::currentSecsSinceEpoch()));
        observations.insert(id, rec);
        // Bound retention; removing oldest observations does not affect actual boot history.
        if (observations.size() > 48) {
            QStringList ids = observations.keys();
            std::sort(ids.begin(), ids.end(), [&observations](const QString &a, const QString &b) {
                return observations.value(a).toObject().value(QStringLiteral("observed_at")).toString()
                     < observations.value(b).toObject().value(QStringLiteral("observed_at")).toString();
            });
            for (int i=0; i<ids.size()-48; ++i) observations.remove(ids.at(i));
        }
        obj.insert(QStringLiteral("boots"), observations);
        // Sampled only on user request; warning is added by caller if the save fails.
    }
    return obj;
}
QString changeText(const QJsonObject &a, const QJsonObject &b, const QString &field) {
    const QString x = a.value(field).toString(), y = b.value(field).toString();
    if (x.isEmpty() || y.isEmpty() || x == QStringLiteral("Unknown") || y == QStringLiteral("Unknown"))
        return field + QStringLiteral(": change unknown (not sampled for both boots)");
    return field + QStringLiteral(": ") + (x == y ? QStringLiteral("unchanged (") + y + QLatin1Char(')')
        : QStringLiteral("changed ") + x + QStringLiteral(" → ") + y + QStringLiteral(" (temporal association only)"));
}
QString currentPcie() {
    QJsonObject links;
    for (const QString &name : entries(QStringLiteral("/sys/bus/pci/devices"))) {
        const QString root = QStringLiteral("/sys/bus/pci/devices/") + name + QLatin1Char('/');
        if (!QFile::exists(root + QStringLiteral("current_link_speed"))) continue;
        QJsonObject row;
        for (const QString &key : {"vendor", "device", "current_link_speed", "current_link_width",
                                    "max_link_speed", "max_link_width"}) {
            const QString v = rd(root + key, 8192);
            if (!v.isEmpty()) row.insert(key, v.left(2048));
        }
        const std::pair<const char*,const char*> aerFiles[] = {
            {"aer_dev_correctable", "TOTAL_ERR_COR"},
            {"aer_dev_nonfatal", "TOTAL_ERR_NONFATAL"},
            {"aer_dev_fatal", "TOTAL_ERR_FATAL"}
        };
        for (const auto &[file, total] : aerFiles) {
            const QString v = rd(root + QString::fromLatin1(file), 8192);
            const QString count = advanceddiagnostics::aerTotal(v, QString::fromLatin1(total));
            if (!count.isEmpty()) row.insert(QString::fromLatin1(file), count);
        }
        links.insert(name, row);
    }
    return QString::fromUtf8(QJsonDocument(links).toJson(QJsonDocument::Compact));
}
QJsonObject linkSample() {
    QJsonObject ret;
    ret.insert(QStringLiteral("boot"), bootId());
    ret.insert(QStringLiteral("t"), QDateTime::currentSecsSinceEpoch());
    ret.insert(QStringLiteral("links"), QJsonDocument::fromJson(currentPcie().toUtf8()).object());
    return ret;
}
QString linkReport() {
    const QString path = dataDir() + QStringLiteral("/pcie-link-last.json");
    const QJsonObject before = loadObject(path);
    const QJsonObject now = linkSample();
    QString out = reportHeader(QStringLiteral("S04"), QStringLiteral("PCIe Link Negotiation Lab"));
    out += QStringLiteral("PCIe speed may downclock at idle; a lower observed speed is NOT proof of a fault.\n"
                          "Sampling cannot establish the exact timestamp of an AER counter increase.\n\n");
    out += advanceddiagnostics::linkDelta(before.value(QStringLiteral("links")).toObject(),
                                          now.value(QStringLiteral("links")).toObject(),
                                          !before.isEmpty() && !now.value(QStringLiteral("boot")).toString().isEmpty() &&
                                          before.value(QStringLiteral("boot")) == now.value(QStringLiteral("boot")));
    const QString result = saveObject(path, now);
    if (!result.isEmpty()) out += QStringLiteral("\nSample not saved: ") + result + QLatin1Char('\n');
    else out += QStringLiteral("\nThis reading becomes the next on-demand comparison point.\n");
    return out;
}
QString radioDoctor() {
    QStringList lines = {reportHeader(QStringLiteral("S07"), QStringLiteral("Wi-Fi/Bluetooth Firmware Doctor")),
        QStringLiteral("No firmware reload, rfkill change, or module insertion is performed.")};
    int radios=0;
    for (const QString &pci : entries(QStringLiteral("/sys/bus/pci/devices"))) {
        const QString base = QStringLiteral("/sys/bus/pci/devices/") + pci + QLatin1Char('/');
        const QString cls = rd(base + QStringLiteral("class"));
        if (!cls.startsWith(QStringLiteral("0x0280")) && !cls.startsWith(QStringLiteral("0x0d11"))) continue;
        const QString driver = QFileInfo(base + QStringLiteral("driver")).canonicalFilePath();
        lines << QStringLiteral("PCI %1: vendor=%2 device=%3 driver=%4")
            .arg(pci, val(base + QStringLiteral("vendor")), val(base + QStringLiteral("device")),
                 driver.isEmpty() ? QStringLiteral("unbound") : QFileInfo(driver).fileName());
        ++radios;
    }
    for (const QString &name : entries(QStringLiteral("/sys/class/rfkill"))) {
        const QString base = QStringLiteral("/sys/class/rfkill/") + name + QLatin1Char('/');
        lines << QStringLiteral("rfkill %1: type=%2 soft=%3 hard=%4")
                    .arg(name, val(base+QStringLiteral("type")),val(base+QStringLiteral("soft")),val(base+QStringLiteral("hard")));
    }
    for (const QString &name : entries(QStringLiteral("/sys/class/bluetooth")))
        lines << QStringLiteral("Bluetooth controller: %1 (HCI registered; firmware state needs logs)").arg(name);
    for (const QString &name : entries(QStringLiteral("/sys/bus/usb/devices"))) {
        const QString b = QStringLiteral("/sys/bus/usb/devices/") + name + QLatin1Char('/');
        const QString cls = rd(b + QStringLiteral("bDeviceClass"));
        if (cls == QStringLiteral("e0")) {
            lines << QStringLiteral("USB wireless %1: vendor=%2 product=%3").arg(name,val(b+QStringLiteral("idVendor")),val(b+QStringLiteral("idProduct")));
        } else if (name.contains(QLatin1Char(':')) && rd(b+QStringLiteral("bInterfaceClass"))==QStringLiteral("e0")) {
            // Many Bluetooth devices identify as a composite USB device (class 00)
            // with a wireless-controller interface (class e0). Device-level only
            // scanning silently misses those adapters.
            const QString parent=name.section(QLatin1Char(':'),0,0);
            const QString device=QStringLiteral("/sys/bus/usb/devices/")+parent+QLatin1Char('/');
            lines << QStringLiteral("USB wireless interface %1: subclass=%2 protocol=%3 vendor=%4 product=%5")
                .arg(name,val(b+QStringLiteral("bInterfaceSubClass")),val(b+QStringLiteral("bInterfaceProtocol")),
                     val(device+QStringLiteral("idVendor")),val(device+QStringLiteral("idProduct")));
        }
    }
    lines << QStringLiteral("Requested/missing firmware lines from saved health log (bounded):");
    int found=0;
    for (const QString &line : recentLog(250)) {
        const QString lower = line.toLower();
        if (!(lower.contains(QStringLiteral("firmware")) || lower.contains(QStringLiteral("hci")) || lower.contains(QStringLiteral("mt792")))) continue;
        if (!(lower.contains(QStringLiteral("bluetooth")) || lower.contains(QStringLiteral("mt79")) || lower.contains(QStringLiteral("iwlwifi")) ||
              lower.contains(QStringLiteral("btusb")) || lower.contains(QStringLiteral("btmtk")))) continue;
        if (++found > 40) break;
        lines << line.left(350);
    }
    if (!found) lines << QStringLiteral("No matching saved events. Kernel log access/retention may be limited.");
    if (!radios) lines << QStringLiteral("No supported PCI radio class exposed; not evidence that Bluetooth/Wi-Fi is healthy.");
    return join(lines);
}
QString powerMap() {
    QStringList lines = {reportHeader(QStringLiteral("S08"), QStringLiteral("Power Limit Source Map")),
        QStringLiteral("Sources below are independent. This tool never calls SMU, EC, ACPI/WMI setter or nvidia-smi.")};
    for (const QString &dir : entries(QStringLiteral("/sys/class/powercap"))) {
        const QString b = QStringLiteral("/sys/class/powercap/") + dir + QLatin1Char('/');
        const QString name = rd(b + QStringLiteral("name"));
        if (name.isEmpty()) continue;
        lines << QStringLiteral("Powercap %1 [%2]").arg(dir, name);
        for (const QString &k : {"constraint_0_power_limit_uw", "constraint_1_power_limit_uw", "constraint_0_time_window_us",
                                 "constraint_1_time_window_us", "enabled"}) {
            const QString v=rd(b+k); if (!v.isEmpty()) lines << QStringLiteral("  %1=%2 (%3)").arg(k,v,QString(k).endsWith(QStringLiteral("_uw")) ? QStringLiteral("µW") :
                QString(k).endsWith(QStringLiteral("_us")) ? QStringLiteral("µs") : QStringLiteral("boolean"));
        }
    }
    for (const QString &p : {QStringLiteral("/sys/devices/system/cpu/amd_pstate/status"), QStringLiteral("/sys/firmware/acpi/platform_profile"),
                              QStringLiteral("/sys/devices/system/cpu/cpufreq/boost")}) lines << p + QStringLiteral(" = ") + val(p);
    for (const QString &id : entries(QStringLiteral("/sys/bus/pci/devices"))) {
        const QString b=QStringLiteral("/sys/bus/pci/devices/")+id+QLatin1Char('/');
        if (rd(b+QStringLiteral("vendor")) == QStringLiteral("0x10de") && rd(b+QStringLiteral("class")).startsWith(QStringLiteral("0x03"))) {
            lines << QStringLiteral("NVIDIA %1 runtime=%2; GPU power limit unknown from passive sysfs (no wake request)")
                         .arg(id, val(b+QStringLiteral("power/runtime_status")));
        }
    }
    lines << QStringLiteral("CPU SMU/PPT wattage: unavailable unless a recognized safe exported interface is present; values are not inferred from temperatures/frequencies.");
    return join(lines);
}
QString memoryMap() {
    QStringList lines = {reportHeader(QStringLiteral("S10"), QStringLiteral("Memory Architecture Explorer"))};
    lines << QStringLiteral("NUMA nodes and CPU lists:");
    for (const QString &id : entries(QStringLiteral("/sys/devices/system/node"), {QStringLiteral("node[0-9]*")})) {
        const QString b=QStringLiteral("/sys/devices/system/node/")+id+QLatin1Char('/');
        lines << id + QStringLiteral(" cpulist=") + val(b+QStringLiteral("cpulist")) + QStringLiteral(" meminfo (sample): ")
                      + rd(b+QStringLiteral("meminfo"), 4096).section(QLatin1Char('\n'), 0, 1);
    }
    lines << QStringLiteral("CPU cache domains (shared CPUs are not guaranteed firmware CCD IDs):");
    QSet<QString> l3;
    for (const QString &id : entries(QStringLiteral("/sys/devices/system/cpu"), {QStringLiteral("cpu[0-9]*")})) {
        for (const QString &ix : entries(QStringLiteral("/sys/devices/system/cpu/")+id+QStringLiteral("/cache"), {QStringLiteral("index*")})) {
            const QString b=QStringLiteral("/sys/devices/system/cpu/")+id+QStringLiteral("/cache/")+ix+QLatin1Char('/');
            if (rd(b+QStringLiteral("level")) == QStringLiteral("3")) { const QString group=rd(b+QStringLiteral("shared_cpu_list")); if (!group.isEmpty()) l3.insert(group); }
        }
    }
    QStringList ordered=l3.values(); ordered.sort(); for (const QString &l : ordered) lines << QStringLiteral("L3 sharing: ")+l;
    for (const QString &p : {QStringLiteral("/sys/kernel/mm/transparent_hugepage/enabled"),
                              QStringLiteral("/sys/kernel/mm/transparent_hugepage/defrag"),
                              QStringLiteral("/sys/kernel/mm/transparent_hugepage/shmem_enabled"),
                              QStringLiteral("/proc/pressure/memory"),QStringLiteral("/proc/swaps")})
        lines << p + QStringLiteral("\n") + val(p);
    for (const QString &dev : entries(QStringLiteral("/sys/block"), {QStringLiteral("zram*")})) {
        const QString b=QStringLiteral("/sys/block/")+dev+QStringLiteral("/mm_stat");
        lines << dev + QStringLiteral(" mm_stat=")+val(b);
    }
    for (const QString &p : {QStringLiteral("/sys/fs/cgroup/memory.low"), QStringLiteral("/sys/fs/cgroup/memory.high"),
                              QStringLiteral("/sys/fs/cgroup/memory.max")}) lines << p + QStringLiteral(" = ") + val(p);
    return join(lines);
}
QString firmwareBrowser() {
    QStringList lines = {reportHeader(QStringLiteral("S11"), QStringLiteral("ACPI/WMI Firmware Browser")),
                         QStringLiteral("Metadata ONLY. No ACPI method evaluation, WMI call, attribute write or firmware transaction.")};
    const QString root = QStringLiteral("/sys/bus/wmi/devices");
    int num=0;
    for (const QString &id : entries(root)) {
        const QString b=root+QLatin1Char('/')+id+QLatin1Char('/');
        lines << QStringLiteral("WMI GUID %1: modalias=%2").arg(id, val(b+QStringLiteral("modalias")));
        if (++num >= 48) break;
    }
    if (!num) lines << QStringLiteral("No WMI device metadata exposed.");
    const QString fw = QStringLiteral("/sys/class/firmware-attributes");
    for (const QString &driver : entries(fw)) {
        const QString base=fw+QLatin1Char('/')+driver+QStringLiteral("/attributes");
        lines << QStringLiteral("Firmware attributes provider: ")+driver;
        for (const QString &attr : entries(base)) {
            const QString b=base+QLatin1Char('/')+attr+QLatin1Char('/');
            lines << QStringLiteral("  %1: type=%2 read-only-metadata=%3 possible=%4")
                         .arg(attr, val(b+QStringLiteral("type")), val(b+QStringLiteral("display_name")),
                              val(b+QStringLiteral("possible_values")).left(200));
            // Never read 'current_value' (may contain sensitive auth metadata).
        }
    }
    if (entries(fw).isEmpty()) lines << QStringLiteral("No firmware-attributes provider exported.");
    return join(lines);
}
QString chargerInspector() {
    QStringList lines = {reportHeader(QStringLiteral("S13"), QStringLiteral("USB-C PD / Charger Inspector")),
                         QStringLiteral("Values are direct kernel exports; voltage/current units are normalized only when explicitly documented.")};
    for (const QString &id : entries(QStringLiteral("/sys/class/typec"))) {
        const QString b=QStringLiteral("/sys/class/typec/")+id+QLatin1Char('/');
        lines << QStringLiteral("Type-C %1: data_role=%2 power_role=%3 preferred_role=%4 orientation=%5")
                    .arg(id, val(b+QStringLiteral("data_role")),val(b+QStringLiteral("power_role")),
                         val(b+QStringLiteral("preferred_role")),val(b+QStringLiteral("orientation")));
    }
    for (const QString &id : entries(QStringLiteral("/sys/class/power_supply"))) {
        const QString b=QStringLiteral("/sys/class/power_supply/")+id+QLatin1Char('/');
        const QString kind=rd(b+QStringLiteral("type"));
        if (!QStringList{QStringLiteral("USB"),QStringLiteral("USB_C"),QStringLiteral("USB_PD"),QStringLiteral("Mains"),QStringLiteral("Battery")}.contains(kind)) continue;
        lines << QStringLiteral("%1 [%2] online=%3 status=%4")
                .arg(id, kind, val(b+QStringLiteral("online")), val(b+QStringLiteral("status")));
        const QString v=rd(b+QStringLiteral("voltage_now")), i=rd(b+QStringLiteral("current_now")), p=rd(b+QStringLiteral("power_now"));
        if (!p.isEmpty()) {
            bool ok=false; const double w=p.toDouble(&ok)/1e6;
            if (ok && std::isfinite(w)) lines << QStringLiteral("  measured power_now=%1 W").arg(w,0,'f',2);
        } else if (!v.isEmpty() && !i.isEmpty()) {
            bool a=false,c=false; const double volts=v.toDouble(&a)/1e6, amps=i.toDouble(&c)/1e6;
            if (a && c && std::isfinite(volts*amps)) lines << QStringLiteral("  measured voltage×current=%1 W (not negotiated PD capability)").arg(volts*amps,0,'f',2);
        } else lines << QStringLiteral("  measured power: unavailable (no wattage inferred)");
        for (const QString &k : {"usb_type", "voltage_max", "current_max", "input_current_limit"}) {
            const QString value=rd(b+k); if (!value.isEmpty()) lines << QStringLiteral("  %1=%2 (raw sysfs unit)").arg(k,value);
        }
    }
    return join(lines);
}
QString audioDoctor() {
    QStringList lines = {reportHeader(QStringLiteral("S15"), QStringLiteral("PipeWire Audio Doctor")),
        QStringLiteral("Read-only server metadata. External status commands are started asynchronously by System Tools; no restart, routing change or pw-top.")};
    lines << QStringLiteral("ALSA cards:\n") + val(QStringLiteral("/proc/asound/cards"));
    lines << QStringLiteral("PipeWire user socket (absence is not proof the service is stopped):");
    QString rt=qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!rt.startsWith(QLatin1Char('/'))) rt=QStringLiteral("(unknown runtime dir)");
    lines << QStringLiteral("  %1/pipewire-0: %2").arg(rt,QFile::exists(rt+QStringLiteral("/pipewire-0")) ? QStringLiteral("exists") : QStringLiteral("unavailable"));
    // The ToolPage runs wpctl/pactl as async QProcess steps after this report.
    // Do not call waitForFinished() from the UI thread here.
    lines << QStringLiteral("wpctl status and pactl info follow asynchronously where installed.");
    lines << QStringLiteral("Xruns may require an active PipeWire monitor; this passive report does not claim zero xruns.");
    return join(lines);
}
QString kernelDependencies() {
    QStringList lines = {reportHeader(QStringLiteral("S18"), QStringLiteral("Kernel Feature & Firmware Dependency Check"))};
    const QString rel=rd(QStringLiteral("/proc/sys/kernel/osrelease"));
    lines << QStringLiteral("Running kernel: ") + rel;
    const QString config=QStringLiteral("/boot/config-")+rel;
    QString blob=rd(config, 2 << 20);
    if (blob.isEmpty() && QFile::exists(QStringLiteral("/proc/config.gz"))) {
        // No zcat at startup: the user explicitly requested the tool. Bounded
        // decompression output; unavailable zcat is not treated as CONFIG=n.
        const QString zcat=QStandardPaths::findExecutable(QStringLiteral("zcat"));
        if (!zcat.isEmpty()) {
            QProcess p;
            p.setProcessChannelMode(QProcess::MergedChannels);
            p.start(zcat,{QStringLiteral("/proc/config.gz")});
            if (p.waitForStarted(1000)) {
                QElapsedTimer timer; timer.start();
                QByteArray out;
                const qsizetype limit=2 << 20;
                while (p.state()!=QProcess::NotRunning && timer.elapsed()<2500 && out.size()<=limit) {
                    p.waitForReadyRead(100);
                    out += p.readAll();  // drain while child is running: no pipe deadlock
                }
                out += p.readAll();
                if (p.state()!=QProcess::NotRunning) { p.kill(); p.waitForFinished(1000); }
                else if (p.exitStatus()==QProcess::NormalExit && p.exitCode()==0 && out.size()<=limit)
                    blob=QString::fromUtf8(out);
            }
        }
    }
    lines << QStringLiteral("Config source: ") + (!blob.isEmpty() ? QFile::exists(config) ? config : QStringLiteral("/proc/config.gz") :
                                                QStringLiteral("unavailable — no feature assumed disabled"));
    const QStringList wants={QStringLiteral("CONFIG_NVME_CORE"),QStringLiteral("CONFIG_BLK_DEV_NVME"),
        QStringLiteral("CONFIG_F2FS_FS"),QStringLiteral("CONFIG_PCI_AER"),QStringLiteral("CONFIG_EDAC"),
        QStringLiteral("CONFIG_PSTORE"),QStringLiteral("CONFIG_ACPI_WMI"),QStringLiteral("CONFIG_BT"),
        QStringLiteral("CONFIG_BT_HCIBTUSB"),QStringLiteral("CONFIG_MT7925E"),QStringLiteral("CONFIG_FW_LOADER")};
    for (const QString &key : wants) {
        const QRegularExpression re(QStringLiteral("(?:^|\\n)")+QRegularExpression::escape(key)+QStringLiteral(R"(=([ymn]))"));
        const auto m=re.match(blob);
        lines << key + QStringLiteral(" = ") + (blob.isEmpty() ? QStringLiteral("Unknown") :
            m.hasMatch() ? m.captured(1) : blob.contains(QStringLiteral("# ")+key+QStringLiteral(" is not set")) ? QStringLiteral("n") : QStringLiteral("not shown"));
    }
    const QStringList fw={QStringLiteral("mediatek/mt7927"),QStringLiteral("mediatek/mt7925"),
                           QStringLiteral("mediatek/BT_RAM_CODE_MT6639_2_1_hdr.bin"),
                           QStringLiteral("mediatek/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin")};
    for (const QString &file : fw) {
        // Directory visibility is not proof that the driver requested a specific file.
        lines << QStringLiteral("Firmware candidate %1 : %2").arg(file,
              QFile::exists(QStringLiteral("/lib/firmware/")+file) || QFile::exists(QStringLiteral("/usr/lib/firmware/")+file) ?
                QStringLiteral("present on disk (not proof of load)") : QStringLiteral("not found at checked paths (may be embedded/renamed)"));
    }
    lines << QStringLiteral("CONFIG=m can require an installed module and a matching firmware asset. No modules are loaded here.");
    return join(lines);
}
QString nvmeHealth(const QJsonObject &v) {
    QStringList lines={reportHeader(QStringLiteral("H06"),QStringLiteral("NVMe Deep Health")),
        QStringLiteral("SMART and error log are read-only NVMe admin queries via health-reader; no self-test/format/reset is executed.")};
    if (!v.value(QStringLiteral("ok")).toBool()) {
        lines << QStringLiteral("Data unavailable: ") + v.value(QStringLiteral("error")).toString(QStringLiteral("missing helper or permission denied"));
        return join(lines);
    }
    for (const QJsonValue &device : v.value(QStringLiteral("controllers")).toArray()) {
        const QJsonObject d=device.toObject();
        lines << QStringLiteral("Controller %1 state=%2 firmware=%3")
             .arg(d.value(QStringLiteral("name")).toString(),d.value(QStringLiteral("state")).toString(),d.value(QStringLiteral("firmware")).toString());
        for (const QString &k : {"critical_warning", "temperature", "available_spare", "percentage_used", "media_errors",
                                 "num_err_log_entries", "unsafe_shutdowns", "warning_temp_time", "critical_comp_time",
                                 "thermal_mgmt_temp1_transition_count", "thermal_mgmt_temp2_transition_count"}) {
            const QJsonValue x=d.value(QStringLiteral("smart")).toObject().value(k);
            if (!x.isUndefined()) lines << QStringLiteral("  SMART %1=%2").arg(k, x.toVariant().toString());
        }
        const QJsonObject error=d.value(QStringLiteral("error_log")).toObject();
        if (!error.isEmpty()) lines << QStringLiteral("  Error log available; non-empty entries=%1").arg(error.value(QStringLiteral("entries")).toInt());
        lines << QStringLiteral("  SMART status: %1; Error log: %2")
            .arg(d.value(QStringLiteral("smart_status")).toString(),d.value(QStringLiteral("error_status")).toString());
    }
    if (v.value(QStringLiteral("controllers")).toArray().isEmpty()) lines << QStringLiteral("No NVMe controllers enumerated (not a clean-health result).");
    return join(lines);
}
QString gpuMemory(const QJsonObject &diag) {
    QStringList lines={reportHeader(QStringLiteral("H16"), QStringLiteral("GPU Memory Reliability View")),
        QStringLiteral("GPU ECC counters need an explicit active NVIDIA query on some hardware. This passive scan NEVER wakes the dGPU.")};
    int n=0;
    for (const QString &id : entries(QStringLiteral("/sys/bus/pci/devices"))) {
        const QString b=QStringLiteral("/sys/bus/pci/devices/")+id+QLatin1Char('/');
        if (rd(b+QStringLiteral("vendor")) != QStringLiteral("0x10de") || !rd(b+QStringLiteral("class")).startsWith(QStringLiteral("0x03"))) continue;
        ++n;
        lines << QStringLiteral("GPU PCI %1 power/runtime_status=%2; passive ECC counter=Unknown (not exported)")
                     .arg(id,val(b+QStringLiteral("power/runtime_status")));
    }
    if (!n) lines << QStringLiteral("No NVIDIA PCI graphics device discovered.");
    for (const QJsonValue &v : diag.value(QStringLiteral("xid_cases")).toArray()) {
        const QJsonObject o=v.toObject();
        lines << QStringLiteral("Xid 154 GPU %1 at kernel time %2 μs; not proof of VRAM failure")
                     .arg(o.value(QStringLiteral("pci")).toString(),o.value(QStringLiteral("ts_us")).toVariant().toString());
    }
    lines << QStringLiteral("For ECC detail use the optional active query button; on consumer GPUs ECC may be N/A.");
    return join(lines);
}
QString sleepMatrix(const QJsonArray &boots) {
    QStringList lines={reportHeader(QStringLiteral("H12"), QStringLiteral("Sleep/Resume Failure Matrix")),
        QStringLiteral("Sources: retained warning/error kernel.log and Health history; normal successful suspend/resume messages may not be retained."),
        QStringLiteral("Counts are classified observations grouped by boot, NOT a proven wake-failure rate or causal attribution.")};
    struct Summary { int sleep=0, failure=0, d3cold=0, usb=0; };
    QMap<QString,Summary> grouped;
    const QRegularExpression bootRe(QStringLiteral(R"(boot=([0-9a-fA-F]{8})\s+seq=([0-9]+))"));
    QSet<QString> seen;
    QStringList excerpts;
    for (const QString &line : recentLog(500)) {
        const QString l=line.toLower();
        const bool isUsb=l.contains(QStringLiteral("usb")) && l.contains(QStringLiteral("reset"));
        const bool isD3=l.contains(QStringLiteral("d3cold"));
        const bool isSleep=l.contains(QStringLiteral("suspend")) || l.contains(QStringLiteral("resume")) ||
                           l.contains(QStringLiteral("wakeup")) || l.contains(QStringLiteral("pm: suspend"));
        if (!isUsb && !isD3 && !isSleep) continue;
        // Health and kernel log copies of one record share the same stable key.
        const QRegularExpressionMatch m=bootRe.match(line);
        const QString key=m.hasMatch() ? m.captured(1).toLower()+QLatin1Char('/')+m.captured(2) : line;
        if (seen.contains(key)) continue;
        seen.insert(key);
        const QString id=m.hasMatch() ? m.captured(1).toLower() : QStringLiteral("unattributed");
        Summary &count=grouped[id];
        if (isSleep) ++count.sleep;
        if (isD3) ++count.d3cold;
        if (isUsb) ++count.usb;
        if (l.contains(QStringLiteral("error")) || l.contains(QStringLiteral("failed")) ||
            l.contains(QStringLiteral("timeout")) || l.contains(QStringLiteral("abort"))) ++count.failure;
        if (excerpts.size()<50) excerpts << line.left(350);
    }
    lines << QStringLiteral("\nBoot | sleep/resume mentions | failure hints | D3cold | USB reset | ledger state");
    QSet<QString> listed;
    for (const QJsonValue &v : boots) {
        const QJsonObject b=v.toObject();
        const QString id=b.value(QStringLiteral("boot")).toString().left(8).toLower();
        if (id.isEmpty() || listed.contains(id)) continue;
        listed.insert(id);
        const Summary c=grouped.value(id);
        lines << QStringLiteral("%1 | %2 | %3 | %4 | %5 | %6")
               .arg(id).arg(c.sleep).arg(c.failure).arg(c.d3cold).arg(c.usb)
               .arg(b.value(QStringLiteral("state")).toString(QStringLiteral("Unknown")));
    }
    for (auto it=grouped.cbegin();it!=grouped.cend();++it) {
        if (listed.contains(it.key())) continue;
        const Summary &c=it.value();
        lines << QStringLiteral("%1 | %2 | %3 | %4 | %5 | no matching boot record")
               .arg(it.key()).arg(c.sleep).arg(c.failure).arg(c.d3cold).arg(c.usb);
    }
    if (grouped.isEmpty()) lines << QStringLiteral("No matching retained events; this does NOT prove successful suspend/resume.");
    lines << QStringLiteral("\nBounded matching evidence:");
    lines += excerpts;
    lines << QStringLiteral("Use existing Sleep Diagnostics for residency and power counters; an unclean boot alone is not a resume failure.");
    return join(lines);
}
QString h18Wizard() {
    QStringList s={reportHeader(QStringLiteral("H18"), QStringLiteral("Guided Diagnostic Wizard")),
        QStringLiteral("Choose an incident category below. All suggested steps are observational; no command is executed.")};
    s << QStringLiteral("Xid/GSP: Health > Evidence Lab > NVIDIA Xid Cases; compare S04 PCIe link and S06 module parameters. Xid 154 itself is a recovery action.");
    s << QStringLiteral("MCE/CO: Evidence Lab > Ryzen MCE; examine H13 trial history. An APIC ID alone is not a CO core number; do not repeat guarded offsets.");
    s << QStringLiteral("NVMe/F2FS: H06 NVMe Deep Health and H05 F2FS Integrity. Back up user data before any offline filesystem repair.");
    s << QStringLiteral("Wi-Fi/BT: H09 failure ledger and S07 Firmware Doctor; check firmware file paths and driver error records. No forced reload.");
    s << QStringLiteral("Sleep: Sleep Diagnostics and H12 Failure Matrix; compare dGPU D3cold and USB logs. No automatic reset of devices.");
    s << QStringLiteral("USB: H10 USB Stability Watch; repeat with another port/cable before attributing the problem to a controller.");
    return join(s);
}
} // namespace

namespace advanceddiagnostics {
QString aerTotal(const QString &text, const QString &counter) {
    // Linux AER sysfs files are multiple human-readable lines, e.g.
    // "Receiver Error 1\nTOTAL_ERR_COR: 2", not bare integers.
    const QRegularExpression re(QStringLiteral("(?:^|\\n)\\s*")+QRegularExpression::escape(counter)+
                                QStringLiteral(R"(\s*:\s*([0-9]+)\s*(?:\n|$))"));
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch()) return {};
    bool ok=false;
    m.captured(1).toULongLong(&ok);
    return ok ? m.captured(1) : QString();
}
QString linkDelta(const QJsonObject &before, const QJsonObject &after, bool sameBoot) {
    QStringList lines;
    if (!sameBoot) lines << QStringLiteral("No comparable same-boot previous reading (first scan or boot changed).");
    if (after.isEmpty()) lines << QStringLiteral("No PCI devices export current link information.");
    for (auto it=after.begin(); it!=after.end(); ++it) {
        const QJsonObject now=it.value().toObject(), old=before.value(it.key()).toObject();
        lines << QStringLiteral("%1: now %2 ×%3 (cap %4 ×%5)")
             .arg(it.key(),now.value(QStringLiteral("current_link_speed")).toString(QStringLiteral("?")),
                  now.value(QStringLiteral("current_link_width")).toString(QStringLiteral("?")),
                  now.value(QStringLiteral("max_link_speed")).toString(QStringLiteral("?")),
                  now.value(QStringLiteral("max_link_width")).toString(QStringLiteral("?")));
        if (sameBoot && !old.isEmpty()) {
            // Hotplug can re-use a BDF during one boot. Do not compare unrelated endpoints.
            const QString vendor0=old.value(QStringLiteral("vendor")).toString();
            const QString vendor1=now.value(QStringLiteral("vendor")).toString();
            const QString dev0=old.value(QStringLiteral("device")).toString();
            const QString dev1=now.value(QStringLiteral("device")).toString();
            if (!vendor0.isEmpty() && !vendor1.isEmpty() && !dev0.isEmpty() && !dev1.isEmpty() &&
                (vendor0 != vendor1 || dev0 != dev1)) {
                lines << QStringLiteral("  Endpoint identity changed at this PCI address; link/AER delta UNKNOWN.");
                continue;
            }
            for (const QString &k : {"current_link_speed","current_link_width"}) {
                const QString a=old.value(k).toString(), b=now.value(k).toString();
                if (!a.isEmpty() && !b.isEmpty() && a!=b) lines << QStringLiteral("  %1 changed: %2 → %3 (may be normal power management)").arg(k,a,b);
            }
            for (const QString &k : {"aer_dev_correctable","aer_dev_nonfatal","aer_dev_fatal"}) {
                bool ok1=false, ok2=false;
                const auto a=old.value(k).toString().toULongLong(&ok1), b=now.value(k).toString().toULongLong(&ok2);
                lines << QStringLiteral("  %1 delta: %2").arg(k, (!ok1 || !ok2) ? QStringLiteral("Unknown") : b<a ?
                    QStringLiteral("Unknown (counter reset/decrease)") : QString::number(b-a));
            }
        }
    }
    return join(lines);
}
QString usbLedger(const QStringList &events) {
    QStringList out;
    QSet<QString> seen;
    const QRegularExpression fault(QStringLiteral(R"((usb|xhci|hub |port\d+).*?(error -(?:71|110|32)|reset (?:high|full|super)-speed|device descriptor read.*error|unable to enumerate|device not accepting address|HC died|host controller not responding|USB disconnect))"),
        QRegularExpression::CaseInsensitiveOption);
    for (const QString &line : events) {
        const QString low=line.toLower();
        if (!fault.match(line).hasMatch() && !(low.contains(QStringLiteral("usb disconnect"))&&low.contains(QStringLiteral("usb")))) continue;
        // The exact boot+sequence key is stable when the saved health and
        // kernel logs contain the same occurrence. No guessed physical port.
        const auto m=QRegularExpression(QStringLiteral(R"(boot=([0-9a-f]+) seq=(\d+))")).match(line);
        const QString key=m.hasMatch() ? m.captured(1)+QLatin1Char('/')+m.captured(2) : line;
        if (seen.contains(key)) continue;
        seen.insert(key);
        if (out.size() < 100) out << line.left(350);
    }
    return out.isEmpty() ? QStringLiteral("No classified USB failures in retained local logs; coverage is limited.") : join(out);
}
QString compareBoots(const QJsonArray &boots, const QJsonObject &contexts) {
    // Strictly compare completed, sufficiently long CLEAN boots; an unclean
    // boot may lack its final kernel event counts, so reporting zero is false.
    QList<QJsonObject> clean;
    QStringList lines;
    for (const QJsonValue &v : boots) {
        const QJsonObject b=v.toObject();
        if (b.value(QStringLiteral("state")) == QStringLiteral("clean") && b.value(QStringLiteral("duration")).toDouble() >= 60)
            clean << b;
        else if (b.value(QStringLiteral("state")) == QStringLiteral("unclean"))
            lines << QStringLiteral("Unclean boot %1: excluded from event-rate comparison (counters may be incomplete).")
                         .arg(b.value(QStringLiteral("boot")).toString().left(8));
    }
    if (clean.size()<2) {
        lines << QStringLiteral("At least TWO completed clean boots with >=60s coverage are needed. No regression verdict.");
        return join(lines);
    }
    const QJsonObject now=clean.at(0), prev=clean.at(1);
    const QString id0=now.value(QStringLiteral("boot")).toString(), id1=prev.value(QStringLiteral("boot")).toString();
    const QJsonObject contextsBoots=contexts.value(QStringLiteral("boots")).toObject();
    lines << QStringLiteral("Latest completed CLEAN boots %1 (newer) vs %2 (older), event counts/hour:")
                 .arg(id0.left(8),id1.left(8));
    for (const QString &field : {"kernel","nvidia","bios"})
        lines << changeText(contextsBoots.value(id1).toObject(), contextsBoots.value(id0).toObject(), field);
    QSet<QString> names;
    const QJsonObject a=prev.value(QStringLiteral("events")).toObject(), b=now.value(QStringLiteral("events")).toObject();
    for (const QString &n : a.keys()) names.insert(n);
    for (const QString &n : b.keys()) names.insert(n);
    QStringList sorted=names.values(); sorted.sort();
    for (const QString &n : sorted) {
        const double n0=a.value(n).toDouble(), n1=b.value(n).toDouble();
        const double r0=n0*3600/prev.value(QStringLiteral("duration")).toDouble(), r1=n1*3600/now.value(QStringLiteral("duration")).toDouble();
        lines << QStringLiteral("%1: %2/h → %3/h (samples %4 → %5; observational, no causality)")
                    .arg(n).arg(r0,0,'f',2).arg(r1,0,'f',2).arg(n0).arg(n1);
    }
    if (sorted.isEmpty()) lines << QStringLiteral("Both clean boots have no saved event counters; coverage is not guaranteed.");
    return join(lines);
}
QString flightCoverage(const QJsonArray &boots) {
    QStringList lines={QStringLiteral("Previous-boot flight ring contains only retained samples (often last 120), not the whole boot.\n")};
    bool found=false;
    for (const QJsonValue &v : boots) {
        const QJsonObject boot=v.toObject();
        const QJsonArray samples=boot.value(QStringLiteral("flight")).toArray();
        if (samples.isEmpty()) continue;
        found=true;
        qint64 first=0,last=0,prior=0;
        int gaps=0,invalid=0;
        qint64 maxGap=0;
        QList<qint64> intervals;
        for (const QJsonValue &s : samples) {
            const QJsonObject o=s.toObject();
            const qint64 t=qint64(o.value(QStringLiteral("t")).toDouble());
            if (t<=0) {++invalid;continue;}
            if (!first) first=t;
            if (prior && t>prior) intervals << t-prior;
            if (prior && t<=prior) ++invalid;
            prior=t;last=t;
        }
        // Recorder cadence can change within a boot while the ring header
        // retains its initial value. Use a conservative threshold derived from
        // the header AND observed median: avoid falsely accusing normal slow
        // sampling of data loss. In this case true missing samples may go
        // undetected, so the report must acknowledge uncertain coverage.
        const qint64 declared=qint64(boot.value(QStringLiteral("flight_interval")).toDouble());
        qint64 observed=0;
        if (!intervals.isEmpty()) {
            QList<qint64> ordered=intervals;
            std::sort(ordered.begin(),ordered.end());
            observed=ordered.at(ordered.size()/2);
        }
        const bool validDeclared=declared>=1 && declared<=120;
        const qint64 expected=qMax(qint64(1),qMax(validDeclared ? declared : 0,observed));
        const qint64 threshold=qMax(qint64(15),expected*5/2);
        for (qint64 dt : intervals) { maxGap=qMax(maxGap,dt); if (dt>threshold) ++gaps; }
        lines << QStringLiteral("Boot %1 [%2]: %3 samples, first=%4 last=%5, longest gap=%6s, gaps >%7s=%8, bad timestamps=%9")
            .arg(boot.value(QStringLiteral("boot")).toString().left(8),boot.value(QStringLiteral("state")).toString())
            .arg(samples.size()).arg(when(first),when(last)).arg(maxGap).arg(threshold).arg(gaps).arg(invalid);
        if (validDeclared && observed>0 && declared!=observed)
            lines << QStringLiteral("  Cadence differs between ring header (%1s) and retained median (%2s); gaps may reflect an interval change, so loss estimates are conservative.").arg(declared).arg(observed);
        if (!validDeclared && observed==0)
            lines << QStringLiteral("  Cadence not recoverable from saved samples; gap confidence UNKNOWN.");
        const qint64 end=qint64(boot.value(QStringLiteral("end")).toDouble());
        if (end>0 && last>0) lines << QStringLiteral("  Last persisted sample was %1 s before last reported boot activity; boot-end timestamp is not necessarily crash time.")
                                           .arg(end-last >= 0 ? QString::number(end-last) : QStringLiteral("unknown (clock skew)"));
        if (boot.value(QStringLiteral("state"))==QStringLiteral("unclean"))
            lines << QStringLiteral("  This boot ended uncleanly; samples may have stopped BEFORE the actual crash.");
    }
    if (!found) lines << QStringLiteral("No previous-boot flight samples attached. Cannot claim sensor coverage.");
    return join(lines);
}
QString coCrossLink(const QJsonArray &boots, const QJsonObject &diag) {
    QStringList lines={QStringLiteral("CO attempts are evidence of attempted settings, not proof that a particular offset caused a crash.")};
    const QString root=stateDir();
    const QString sessionPath=root+QStringLiteral("/co-search-session.json");
    const QString guardPath=root+QStringLiteral("/co-search-crash-guards.json");
    const QJsonObject session=loadObject(sessionPath, 1 << 20);
    const QJsonObject guards=loadObject(guardPath, 1 << 20);
    const QJsonArray history=loadObject(root+QStringLiteral("/co-search-runs.json"), 1 << 20).value(QStringLiteral("history")).toArray();
    if (QFile::exists(sessionPath) && (session.value(QStringLiteral("version")).toInt()!=3 || session.isEmpty()))
        lines << QStringLiteral("WARNING: CO recovery journal exists but is invalid/unreadable; trial state UNKNOWN. Do not ignore CO Search recovery warnings.");
    else if (!session.isEmpty()) {
        const QString sid=session.value(QStringLiteral("boot")).toString();
        lines << QStringLiteral("Recovery journal present: session boot=%1, recorded attempts=%2, pending=%3")
                  .arg(sid,session.value(QStringLiteral("attempts")).toVariant().toString(),
                       session.value(QStringLiteral("pending")).toBool() ? QStringLiteral("yes") : QStringLiteral("no"));
        if (session.value(QStringLiteral("pending")).toBool()) {
            const QJsonObject trial=session.value(QStringLiteral("trial")).toObject();
            lines << QStringLiteral("  Interrupted/pending candidate: %1, all-core=%2, per-core=%3 (not proof of failure)")
                     .arg(trial.value(QStringLiteral("what")).toString(),trial.value(QStringLiteral("all")).toVariant().toString(),
                          QString::fromUtf8(QJsonDocument(trial.value(QStringLiteral("per")).toArray()).toJson(QJsonDocument::Compact)).left(250));
        }
    }
    const bool guardValid=!QFile::exists(guardPath) ||
             (guards.value(QStringLiteral("version")).toInt()==1 && guards.value(QStringLiteral("guards")).isArray());
    const QJsonArray gs=guardValid ? guards.value(QStringLiteral("guards")).toArray() : QJsonArray{};
    if (!guardValid) lines << QStringLiteral("WARNING: Persistent CO crash guard file invalid/unreadable: guard count UNKNOWN. CO Search must refuse unsafe retries.");
    else lines << QStringLiteral("Persistent crash guards: %1 candidate(s). Guarded values must not be automatically retried.").arg(gs.size());
    for (int idx = 0; idx < qMin(gs.size(), 32); ++idx) {
        const QJsonObject g=gs.at(idx).toObject();
        lines << QStringLiteral("  CCD%1 slot%2 at CO %3 (reported CPUs %4)")
                     .arg(g.value(QStringLiteral("ccd")).toInt()).arg(g.value(QStringLiteral("slot")).toInt())
                     .arg(g.value(QStringLiteral("failed")).toInt()).arg(QJsonDocument(g.value(QStringLiteral("cpus")).toArray()).toJson(QJsonDocument::Compact).trimmed());
    }
    for (const QJsonValue &v : history) {
        const QJsonObject h=v.toObject();
        lines << QStringLiteral("Run %1 boot=%2 mode=%3 completed=%4 attempts=%5 notes=%6")
                    .arg(when(qint64(h.value(QStringLiteral("t")).toDouble())),h.value(QStringLiteral("boot")).toString().left(8),
                         h.value(QStringLiteral("mode")).toString(),h.value(QStringLiteral("complete")).toBool() ? QStringLiteral("yes") : QStringLiteral("no"),
                         h.value(QStringLiteral("attempts")).toVariant().toString(),h.value(QStringLiteral("notes")).toString().left(120));
    }
    if (history.isEmpty()) lines << QStringLiteral("No completed search history available.");
    const QString current=diag.value(QStringLiteral("boot_id")).toString();
    if (!session.isEmpty() && !current.isEmpty())
        lines << (session.value(QStringLiteral("boot")).toString()==current ?
            QStringLiteral("Recovery journal and MCE scan refer to the SAME boot (trial-time relationship still unverified).") :
            QStringLiteral("Recovery journal belongs to a DIFFERENT boot; current MCE cannot be assigned to its trials."));
    if (!current.isEmpty()) {
        lines << QStringLiteral("Current-boot classified MCE evidence (not necessarily during a CO trial):");
        for (const QJsonValue &v : diag.value(QStringLiteral("mce")).toArray()) {
            const QJsonObject e=v.toObject();
            lines << QStringLiteral("  CPU %1, MCA bank %2; %3")
                  .arg(e.value(QStringLiteral("cpu")).toVariant().toString(),e.value(QStringLiteral("bank")).toVariant().toString(),
                       e.value(QStringLiteral("confidence")).toString());
        }
    }
    for (const QJsonValue &v : boots) {
        const QJsonObject b=v.toObject();
        if (b.value(QStringLiteral("state"))==QStringLiteral("unclean"))
            lines << QStringLiteral("Unclean boot %1: curve settings=%2; insufficient evidence to assign cause to one trial.")
                 .arg(b.value(QStringLiteral("boot")).toString().left(8),
                      QString::fromUtf8(QJsonDocument(b.value(QStringLiteral("curves")).toObject()).toJson(QJsonDocument::Compact)).left(200));
    }
    return join(lines);
}
QString systemReport(const QString &code) {
    if (code==QStringLiteral("S04")) return linkReport();
    if (code==QStringLiteral("S07")) return radioDoctor();
    if (code==QStringLiteral("S08")) return powerMap();
    if (code==QStringLiteral("S10")) return memoryMap();
    if (code==QStringLiteral("S11")) return firmwareBrowser();
    if (code==QStringLiteral("S13")) return chargerInspector();
    if (code==QStringLiteral("S15")) return audioDoctor();
    if (code==QStringLiteral("S18")) return kernelDependencies();
    return QStringLiteral("Unknown read-only system diagnostic");
}
QString healthReport(const QString &code, const QJsonObject &boots, const QJsonObject &evidence, const QJsonObject &nvme) {
    if (code==QStringLiteral("H01")) {
        const QJsonObject ctx=contextsWithCurrent();
        const QString err=saveObject(contextPath(),ctx);
        return reportHeader(code,QStringLiteral("Boot Regression Detective")) +
            (err.isEmpty() ? QStringLiteral("This boot's versions sampled by request; earlier versions must have been sampled in their own boots.\n") :
                             QStringLiteral("Context save failed: ")+err+QLatin1Char('\n')) +
            compareBoots(boots.value(QStringLiteral("boots")).toArray(),ctx);
    }
    if (code==QStringLiteral("H06")) return nvmeHealth(nvme);
    if (code==QStringLiteral("H10")) return reportHeader(code,QStringLiteral("USB Stability Watch")) +
        QStringLiteral("Kernel USB/xHCI failures and resets in retained logs; generic disconnects can be intentional.\n\n") + usbLedger(recentLog(600));
    if (code==QStringLiteral("H12")) return sleepMatrix(boots.value(QStringLiteral("boots")).toArray());
    if (code==QStringLiteral("H13")) return reportHeader(code,QStringLiteral("CO Evidence Cross-Link"))+
        coCrossLink(boots.value(QStringLiteral("boots")).toArray(),evidence);
    if (code==QStringLiteral("H15")) return reportHeader(code,QStringLiteral("Pre-crash Coverage Inspector"))+
        flightCoverage(boots.value(QStringLiteral("boots")).toArray());
    if (code==QStringLiteral("H16")) return gpuMemory(evidence);
    if (code==QStringLiteral("H18")) return h18Wizard();
    return QStringLiteral("Unknown read-only health diagnostic");
}
QStringList incidentKeys() {
    QStringList keys;
    QSet<QString> seen;
    for (const QString &line : recentLog(1000)) {
        const auto m=QRegularExpression(QStringLiteral(R"(boot=([0-9a-f]{8}) seq=(\d+))")).match(line);
        if (!m.hasMatch()) continue;
        const QString key=m.captured(1)+QLatin1Char('/')+m.captured(2);
        if (seen.contains(key)) continue;
        seen.insert(key); keys << key;
        if (keys.size()>100) keys.removeFirst();
    }
    return keys;
}
QJsonObject incidentNote(const QString &key) {
    if (!QRegularExpression(QStringLiteral(R"(^[0-9a-f]{8}/[0-9]{1,20}$)")).match(key).hasMatch()) return {};
    return loadObject(dataDir()+QStringLiteral("/incident-notes.json"))
        .value(QStringLiteral("items")).toObject().value(key).toObject();
}
QString incidentText(const QString &key) {
    const QRegularExpression re(QStringLiteral(R"(^[0-9a-f]{8}/[0-9]{1,20}$)"));
    if (!re.match(key).hasMatch()) return QStringLiteral("Invalid event key");
    const QJsonObject e=incidentNote(key);
    QString raw;
    for (const QString &line : recentLog(1000)) {
        if (line.contains(QStringLiteral("boot=")+key.section('/',0,0)+QStringLiteral(" seq=")+key.section('/',1,1)+QStringLiteral(" ")))
            { raw=line.left(650); break; }
    }
    if (raw.isEmpty()) raw=QStringLiteral("Historical event line no longer retained (notes remain private).");
    return QStringLiteral("%1\n\nStatus: %2\nNote: %3\nEdited: %4")
             .arg(raw,e.value(QStringLiteral("status")).toString(QStringLiteral("New")),
                  e.value(QStringLiteral("note")).toString(),e.value(QStringLiteral("edited")).toString());
}
QString saveIncident(const QString &key, const QString &status, const QString &note) {
    if (!QRegularExpression(QStringLiteral(R"(^[0-9a-f]{8}/[0-9]{1,20}$)")).match(key).hasMatch()) return QStringLiteral("Invalid event identity");
    const QStringList valid={QStringLiteral("New"),QStringLiteral("Investigating"),QStringLiteral("Monitoring"),
                            QStringLiteral("Resolved"),QStringLiteral("Reopened")};
    if (!valid.contains(status) || note.size()>2000) return QStringLiteral("Invalid status or note too long");
    const QString path=dataDir()+QStringLiteral("/incident-notes.json");
    QJsonObject all=loadObject(path), items=all.value(QStringLiteral("items")).toObject();
    // Avoid erasing a larger/inaccessible record. No wildcard note deletion.
    if (QFile::exists(path) && all.isEmpty()) return QStringLiteral("Existing note store invalid or unreadable; refusing overwrite");
    if (items.size() >= 400 && !items.contains(key)) return QStringLiteral("Note store full (400 incidents); cannot add another");
    items.insert(key,QJsonObject{{QStringLiteral("status"),status},{QStringLiteral("note"),note},
                                  {QStringLiteral("edited"),when(QDateTime::currentSecsSinceEpoch())}});
    all.insert(QStringLiteral("schema"),1);
    all.insert(QStringLiteral("items"),items);
    return saveObject(path,all);
}
} // namespace advanceddiagnostics
