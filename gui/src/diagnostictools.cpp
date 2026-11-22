#include "diagnostictools.h"

#include "diagsources.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>

using diag::Level;
using diag::Report;
using diag::Table;

namespace diagnostictools {

static QString tr(const char *s) { return QCoreApplication::translate("diagnostictools", s); }
static QString rd(const QString &p, qint64 limit = 128 * 1024) { return diagsrc::read(p, limit); }
static QString baselinePath() { return diagsrc::dataDir() + QStringLiteral("/hardware-baseline.json"); }

// ── S01 Hardware snapshot ─────────────────────────────────────────────────────

static QString cmdline() {
    // Values are kept only for an allowlist of hardware settings; every other parameter keeps its name only
    // (cryptdevice=, root=UUID=… and the like never reach the file).
    static const QSet<QString> allow = {"pcie_aspm", "iommu", "amd_iommu", "intel_iommu", "amd_pstate", "nvme_core.default_ps_max_latency_us",
        "amdgpu.dc", "nvidia-drm.modeset", "nvidia_drm.modeset", "nvidia-drm.fbdev", "nvidia_drm.fbdev", "mem_sleep_default", "zswap.enabled",
        "transparent_hugepage", "mitigations", "preempt", "processor.max_cstate", "pcie_port_pm", "acpi_osi", "acpi_backlight"};
    QStringList flags;
    for (const QString &token : rd(QStringLiteral("/proc/cmdline")).split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        const QString name = token.section(QLatin1Char('='), 0, 0);
        if (!QRegularExpression(QStringLiteral("^[a-zA-Z0-9_.-]{1,80}$")).match(name).hasMatch()) continue;
        flags << (allow.contains(name) || !token.contains(QLatin1Char('=')) ? token.left(120) : name + QStringLiteral("=…"));
    }
    flags.sort();
    return flags.join(QLatin1Char(' '));
}

QJsonObject snapshot() {
    QJsonObject o;
    o.insert(QStringLiteral("host/kernel"), rd(QStringLiteral("/proc/sys/kernel/osrelease")));
    o.insert(QStringLiteral("host/cmdline"), cmdline());
    o.insert(QStringLiteral("host/bios_version"), rd(QStringLiteral("/sys/class/dmi/id/bios_version")));
    o.insert(QStringLiteral("host/bios_date"), rd(QStringLiteral("/sys/class/dmi/id/bios_date")));
    QSet<QString> mc;
    const QRegularExpression mcRe(QStringLiteral(R"(^microcode\s*:\s*(\S+))"), QRegularExpression::MultilineOption);
    for (auto it = mcRe.globalMatch(rd(QStringLiteral("/proc/cpuinfo"), 1 << 20)); it.hasNext();) mc.insert(it.next().captured(1));
    QStringList m = mc.values();
    m.sort();
    o.insert(QStringLiteral("host/microcode"), m.join(QStringLiteral(", ")));
    for (const QString &dev : diagsrc::dir(QStringLiteral("/sys/bus/pci/devices"))) {
        const QString b = QStringLiteral("/sys/bus/pci/devices/") + dev + QLatin1Char('/');
        const QString k = QStringLiteral("pci/") + dev + QLatin1Char('/');
        for (const char *name : {"vendor", "device", "class", "subsystem_vendor", "subsystem_device", "max_link_speed", "max_link_width"}) {
            const QString v = rd(b + QLatin1String(name));
            if (!v.isEmpty()) o.insert(k + QLatin1String(name), v);
        }
        const QFileInfo drv(b + QStringLiteral("driver"));
        if (drv.exists()) o.insert(k + QStringLiteral("driver"), QFileInfo(drv.canonicalFilePath()).fileName());
    }
    for (const QString &line : rd(QStringLiteral("/proc/modules"), 1 << 20).split(QLatin1Char('\n'))) {
        const QString mod = line.section(QLatin1Char(' '), 0, 0);
        if (mod.isEmpty() || !QRegularExpression(QStringLiteral("^[a-zA-Z0-9_]+$")).match(mod).hasMatch()) continue;
        const QString v = rd(QStringLiteral("/sys/module/") + mod + QStringLiteral("/version"));
        o.insert(QStringLiteral("module/") + mod, v.isEmpty() ? QStringLiteral("loaded") : v);
    }
    return o;
}

QString saveHardwareBaseline() {
    return diagsrc::saveJson(baselinePath(), QJsonObject{{"schema", 1}, {"captured_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                                                         {"items", snapshot()}});
}

QJsonObject loadHardwareBaseline(QString *capturedAt, QString *error) {
    if (!QFile::exists(baselinePath())) { *error = tr("No baseline saved yet."); return {}; }
    const QJsonObject doc = diagsrc::loadJson(baselinePath());
    if (doc.value(QStringLiteral("schema")).toInt() != 1 || !doc.value(QStringLiteral("items")).isObject()) {
        *error = tr("The saved baseline is unreadable or from an unknown version.");
        return {};
    }
    *capturedAt = doc.value(QStringLiteral("captured_at")).toString();
    return doc.value(QStringLiteral("items")).toObject();
}

Report hardwareSnapshot(const QJsonObject &base, const QString &capturedAt, const QJsonObject &now) {
    Report r;
    r.code = QStringLiteral("S01");
    r.title = tr("Hardware snapshot");
    r.about = tr("Compares the PCI devices, their drivers, loaded modules, kernel, BIOS, microcode and boot parameters with a baseline you saved, "
                 "grouped by what changed.");
    r.limits << tr("A device that is powered off in hybrid graphics mode, or disabled in the BIOS, disappears from the PCI bus; that is not a fault by itself.")
             << tr("Boot parameter values are compared only for hardware settings; others are kept by name.");
    // Dynamic fields of older baselines are ignored.
    auto dynamic = [](const QString &k) { return k.endsWith(QStringLiteral("current_link_speed")) || k.endsWith(QStringLiteral("current_link_width")); };
    if (base.isEmpty()) {
        int pci = 0, mods = 0;
        for (auto it = now.begin(); it != now.end(); ++it) {
            pci += it.key().startsWith(QStringLiteral("pci/")) && it.key().endsWith(QStringLiteral("/vendor"));
            mods += it.key().startsWith(QStringLiteral("module/"));
        }
        r.add(Level::Info, tr("No baseline saved yet"), tr("This machine has %1 PCI devices and %2 loaded modules now.").arg(pci).arg(mods),
              tr("Press Save baseline while everything works; later, Compare shows exactly what changed."));
    }
    Table host;
    host.title = tr("System");
    host.headers = {tr("Item"), tr("Baseline"), tr("Now")};
    const QList<std::pair<const char *, QString>> hostKeys = {{"host/kernel", tr("Kernel")}, {"host/bios_version", tr("BIOS")}, {"host/bios_date", tr("BIOS date")},
                                                             {"host/microcode", tr("CPU microcode")}, {"host/cmdline", tr("Boot parameters")}};
    for (const auto &[k, label] : hostKeys) {
        const QString a = base.value(QLatin1String(k)).toString(), b = now.value(QLatin1String(k)).toString();
        const bool changed = !base.isEmpty() && a != b;
        host.add({label, base.isEmpty() ? QStringLiteral("—") : a, b}, changed ? Level::Review : Level::Info);
        if (changed) r.add(Level::Info, tr("%1 changed").arg(label), QStringLiteral("%1 → %2").arg(a.left(160), b.left(160)));
    }
    r.tables << host;
    if (base.isEmpty()) return r;

    // PCI devices by address.
    auto devices = [&dynamic](const QJsonObject &o) {
        QMap<QString, QJsonObject> d;
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (!it.key().startsWith(QStringLiteral("pci/")) || dynamic(it.key())) continue;
            d[it.key().section(QLatin1Char('/'), 1, 1)].insert(it.key().section(QLatin1Char('/'), 2), it.value());
        }
        return d;
    };
    const QMap<QString, QJsonObject> a = devices(base), b = devices(now);
    QSet<QString> bdfs;
    for (const QString &k : a.keys()) bdfs.insert(k);
    for (const QString &k : b.keys()) bdfs.insert(k);
    QStringList sorted = bdfs.values();
    sorted.sort();
    Table pci;
    pci.title = tr("PCI device changes");
    pci.headers = {tr("Address"), tr("Device"), tr("Change"), tr("Baseline"), tr("Now")};
    auto name = [](const QJsonObject &d) { return diagsrc::pciName(d.value(QStringLiteral("vendor")).toString(), d.value(QStringLiteral("device")).toString(), d.value(QStringLiteral("class")).toString()); };
    for (const QString &bdf : sorted) {
        const QJsonObject x = a.value(bdf), y = b.value(bdf);
        if (x == y) continue;
        if (y.isEmpty()) {
            pci.add({bdf, name(x), tr("gone"), x.value(QStringLiteral("driver")).toString(), QStringLiteral("—")}, Level::Review);
            r.add(Level::Review, tr("%1 is no longer on the PCI bus (%2)").arg(name(x), bdf),
                  tr("Removed, disabled in the BIOS, powered off by a graphics-mode switch, or failed to come up."),
                  tr("If it should be there: cold boot, then check the BIOS and Events & alerts."));
        } else if (x.isEmpty()) {
            pci.add({bdf, name(y), tr("new"), QStringLiteral("—"), y.value(QStringLiteral("driver")).toString()});
        } else if (x.value(QStringLiteral("vendor")) != y.value(QStringLiteral("vendor")) || x.value(QStringLiteral("device")) != y.value(QStringLiteral("device"))) {
            pci.add({bdf, name(y), tr("other device at this address"), name(x), name(y)}, Level::Info);
        } else if (x.value(QStringLiteral("driver")) != y.value(QStringLiteral("driver"))) {
            const QString d0 = x.value(QStringLiteral("driver")).toString(), d1 = y.value(QStringLiteral("driver")).toString();
            pci.add({bdf, name(y), tr("driver"), d0.isEmpty() ? tr("none") : d0, d1.isEmpty() ? tr("none") : d1}, d1.isEmpty() ? Level::Review : Level::Info);
            if (d1.isEmpty())
                r.add(Level::Review, tr("%1 lost its driver (%2 before)").arg(name(y), d0), tr("The device is present but nothing drives it now."),
                      tr("A module that failed to build or load after a kernel update is the usual cause (S18)."));
        } else {
            pci.add({bdf, name(y), tr("capability"), x.value(QStringLiteral("max_link_speed")).toString() + QStringLiteral(" x") + x.value(QStringLiteral("max_link_width")).toString(),
                     y.value(QStringLiteral("max_link_speed")).toString() + QStringLiteral(" x") + y.value(QStringLiteral("max_link_width")).toString()});
        }
    }
    if (!pci.rows.isEmpty()) r.tables << pci;
    // Modules.
    Table mod;
    mod.title = tr("Module changes");
    mod.headers = {tr("Module"), tr("Change"), tr("Baseline"), tr("Now")};
    QSet<QString> mk;
    for (auto it = base.begin(); it != base.end(); ++it) if (it.key().startsWith(QStringLiteral("module/"))) mk.insert(it.key());
    for (auto it = now.begin(); it != now.end(); ++it) if (it.key().startsWith(QStringLiteral("module/"))) mk.insert(it.key());
    QStringList mods = mk.values();
    mods.sort();
    for (const QString &k : mods) {
        const QString x = base.value(k).toString(), y = now.value(k).toString();
        if (x == y) continue;
        mod.add({k.mid(7), x.isEmpty() ? tr("loaded now") : y.isEmpty() ? tr("not loaded now") : tr("version"), x.isEmpty() ? QStringLiteral("—") : x, y.isEmpty() ? QStringLiteral("—") : y});
    }
    if (!mod.rows.isEmpty()) {
        mod.note = tr("Modules load on demand (a USB stick, a VPN); a module not loaded now is not necessarily missing.");
        r.tables << mod;
    }
    if (pci.rows.isEmpty() && mod.rows.isEmpty() && !r.count(Level::Info))
        r.add(Level::Ok, tr("Nothing changed since the baseline of %1").arg(capturedAt));
    else if (!r.count(Level::Review))
        r.add(Level::Ok, tr("No device disappeared or lost its driver since %1").arg(capturedAt),
              tr("%1 PCI and %2 module difference(s) listed.").arg(pci.rows.size()).arg(mod.rows.size()));
    return r;
}

// ── S03 Displays / EDID / VRR ─────────────────────────────────────────────────

static int u8(const QByteArray &b, int i) { return i < b.size() ? quint8(b.at(i)) : 0; }

Edid decodeEdid(const QByteArray &d) {
    Edid e;
    if (d.size() < 128) return e;
    e.header = d.startsWith(QByteArray::fromHex("00ffffffffffff00"));
    e.declared = u8(d, 126) + 1;
    e.readable = int(d.size() / 128);
    e.truncated = e.readable < e.declared;
    for (int blk = 0; blk < std::min(e.readable, 32); ++blk) {
        int sum = 0;
        for (int i = blk * 128; i < blk * 128 + 128; ++i) sum += u8(d, i);
        const int tag = u8(d, blk * 128);
        const QString kind = blk == 0 ? QStringLiteral("base") : tag == 0x02 ? QStringLiteral("CTA-861") : tag == 0x70 ? QStringLiteral("DisplayID") : QStringLiteral("0x%1").arg(tag, 2, 16, QLatin1Char('0'));
        if (blk) e.extensions << kind;
        if (sum % 256) e.badBlocks << QStringLiteral("%1 (%2)").arg(blk).arg(kind);
    }
    const int id = (u8(d, 8) << 8) | u8(d, 9);
    e.maker = QString(QChar('@' + ((id >> 10) & 0x1f))) + QChar('@' + ((id >> 5) & 0x1f)) + QChar('@' + (id & 0x1f));
    e.year = u8(d, 17) ? 1990 + u8(d, 17) : 0;
    for (int off = 54; off <= 108; off += 18) {
        if (u8(d, off) == 0 && u8(d, off + 1) == 0) {
            const int tag = u8(d, off + 3);
            if (tag == 0xfc || (tag == 0xfe && e.name.isEmpty()))
                e.name = QString::fromLatin1(d.mid(off + 5, 13)).section(QLatin1Char('\n'), 0, 0).trimmed();
            if (tag == 0xfd) {
                const int flags = u8(d, off + 4);
                e.rangeMin = u8(d, off + 5) + ((flags & 0x03) == 0x03 ? 255 : 0);
                e.rangeMax = u8(d, off + 6) + ((flags & 0x02) ? 255 : 0);
            }
            continue;
        }
        const int clk = u8(d, off) | (u8(d, off + 1) << 8);
        const int ha = u8(d, off + 2) | ((u8(d, off + 4) & 0xf0) << 4), hb = u8(d, off + 3) | ((u8(d, off + 4) & 0x0f) << 8);
        const int va = u8(d, off + 5) | ((u8(d, off + 7) & 0xf0) << 4), vb = u8(d, off + 6) | ((u8(d, off + 7) & 0x0f) << 8);
        if (clk && ha + hb && va + vb)
            e.timings << QStringLiteral("%1×%2 @ %3 Hz").arg(ha).arg(va).arg(clk * 10000.0 / ((ha + hb) * (va + vb)), 0, 'f', 2);
    }
    // CTA-861 data blocks: vendor-specific blocks carry the variable refresh ranges.
    for (int blk = 1; blk < std::min(e.readable, 32); ++blk) {
        const int base = blk * 128;
        if (u8(d, base) != 0x02) continue;
        const int dtd = u8(d, base + 2);
        for (int i = base + 4; i < base + std::min(dtd, 127);) {
            const int hdr = u8(d, i), tag = hdr >> 5, len = hdr & 0x1f;
            if (tag == 3 && len >= 3) {
                const int oui = u8(d, i + 1) | (u8(d, i + 2) << 8) | (u8(d, i + 3) << 16);
                if (oui == 0x00001a && len >= 7) {           // AMD FreeSync: OUI, version, caps, min, max
                    e.vrrMin = u8(d, i + 6); e.vrrMax = u8(d, i + 7); e.vrrSource = QStringLiteral("FreeSync (AMD VSDB)");
                } else if (oui == 0xc45dd8 && len >= 10) {   // HDMI Forum: VRRmin / VRRmax
                    const int mn = u8(d, i + 9) & 0x3f, mx = ((u8(d, i + 9) & 0xc0) << 2) | u8(d, i + 10);
                    if (mx > 0 && e.vrrMax == 0) { e.vrrMin = mn; e.vrrMax = mx; e.vrrSource = QStringLiteral("HDMI 2.1 VRR"); }
                }
            }
            i += len + 1;
        }
    }
    return e;
}

Report displays(const QString &root) {
    Report r;
    r.code = QStringLiteral("S03");
    r.title = tr("Displays / EDID / VRR");
    r.about = tr("Every connected display as the kernel sees it: its identity, the modes its EDID declares, and the refresh range it offers for "
                 "variable refresh (FreeSync / Adaptive-Sync / HDMI VRR). The EDID is only read.");
    r.limits << tr("Whether VRR is actually on depends on the compositor and driver (KDE: Display settings → Adaptive sync); the EDID says what the display supports.")
             << tr("An EDID can be overridden by the kernel (drm.edid_firmware); the one shown is what the driver uses.");
    Table t;
    t.title = tr("Connected displays");
    t.headers = {tr("Connector"), tr("Display"), tr("Year"), tr("Preferred mode"), tr("Refresh range"), tr("VRR range"), tr("EDID")};
    int connected = 0;
    for (const QString &conn : diagsrc::dir(root + QStringLiteral("/sys/class/drm"))) {
        if (!conn.contains(QLatin1Char('-'))) continue;
        const QString b = root + QStringLiteral("/sys/class/drm/") + conn + QLatin1Char('/');
        if (diagsrc::read(b + QStringLiteral("status")) != QStringLiteral("connected")) continue;
        ++connected;
        const QString port = conn.section(QLatin1Char('-'), 1);
        QFile f(b + QStringLiteral("edid"));
        const QByteArray raw = f.open(QIODevice::ReadOnly) ? f.read(32 * 128) : QByteArray();
        if (raw.size() < 128) {
            t.add({port, tr("(no EDID)"), QStringLiteral("—"), QStringLiteral("—"), QStringLiteral("—"), QStringLiteral("—"), tr("missing")}, Level::Unknown);
            r.add(Level::Unknown, tr("%1 is connected but gives no EDID").arg(port), tr("The driver falls back to safe modes; a cable or adapter that drops the DDC lines does this."));
            continue;
        }
        const Edid e = decodeEdid(raw);
        const bool bad = !e.header || e.truncated || !e.badBlocks.isEmpty();
        t.add({port, QStringLiteral("%1 %2").arg(e.maker, e.name).trimmed(), e.year ? QString::number(e.year) : QStringLiteral("—"),
               e.timings.value(0, QStringLiteral("—")), e.rangeMax ? QStringLiteral("%1–%2 Hz").arg(e.rangeMin).arg(e.rangeMax) : QStringLiteral("—"),
               e.vrrMax ? QStringLiteral("%1–%2 Hz · %3").arg(e.vrrMin).arg(e.vrrMax).arg(e.vrrSource) : QStringLiteral("—"),
               bad ? tr("damaged") : tr("valid, %1 block(s)").arg(e.readable)}, bad ? Level::Review : Level::Ok);
        if (bad)
            r.add(Level::Review, tr("%1: the EDID is damaged").arg(port),
                  !e.header ? tr("Its header is wrong.") : e.truncated ? tr("It declares %1 blocks but %2 were read.").arg(e.declared).arg(e.readable)
                                                                     : tr("Checksum wrong in block %1.").arg(e.badBlocks.join(QStringLiteral(", "))),
                  tr("Wrong or missing modes and no VRR follow from this. Try another cable / port; a persistent fault can be worked around with drm.edid_firmware."));
        else if (e.vrrMax)
            r.add(Level::Info, tr("%1 supports variable refresh %2–%3 Hz (%4)").arg(port).arg(e.vrrMin).arg(e.vrrMax).arg(e.vrrSource),
                  e.vrrMin && e.vrrMax < e.vrrMin * 2 ? tr("The range is narrower than 2:1, so low frame-rate compensation (frame doubling) cannot work below %1 Hz.").arg(e.vrrMin)
                                                       : tr("The range is wide enough for low frame-rate compensation."));
        else if (e.rangeMax > e.rangeMin && e.rangeMin > 0)
            r.add(Level::Info, tr("%1 declares a %2–%3 Hz refresh range").arg(port).arg(e.rangeMin).arg(e.rangeMax),
                  tr("Laptop panels and DisplayPort monitors announce Adaptive-Sync this way (or in a DisplayID block); it is the range VRR can use when the driver enables it."));
        r.raw += QStringLiteral("== %1 ==\n%2\n%3\n").arg(conn, e.timings.join(QLatin1Char('\n')), diagsrc::read(b + QStringLiteral("modes"), 8192));
    }
    r.tables << t;
    if (!connected) r.add(Level::Unknown, tr("No connected display found under /sys/class/drm"));
    else if (!r.count(Level::Review) && !r.count(Level::Unknown)) r.add(Level::Ok, tr("%1 display(s) with a valid EDID").arg(connected));
    return r;
}

// ── S06 NVIDIA module parameters ──────────────────────────────────────────────

QMap<QString, QString> parseNvidiaOptionsText(const QString &raw) {
    QMap<QString, QString> result;
    QString content = raw.left(256 * 1024);
    content.replace(QStringLiteral("\\\n"), QStringLiteral(" "));
    for (QString line : content.split(QLatin1Char('\n'))) {
        line = line.section(QLatin1Char('#'), 0, 0).trimmed();
        const QRegularExpressionMatch m = QRegularExpression(QStringLiteral(R"(^options\s+(\S+)\s+(.+)$)"), QRegularExpression::CaseInsensitiveOption).match(line);
        if (!m.hasMatch()) continue;
        const QString module = m.captured(1).replace(QLatin1Char('-'), QLatin1Char('_'));
        if (!module.startsWith(QStringLiteral("nvidia"))) continue;
        const QRegularExpression arg(QStringLiteral(R"(([^\s=]+)=("[^"]*"|'[^']*'|[^\s]+))"));
        for (auto it = arg.globalMatch(m.captured(2)); it.hasNext();) {
            const auto x = it.next();
            if (!QRegularExpression(QStringLiteral("^[a-zA-Z0-9_]+$")).match(x.captured(1)).hasMatch()) continue;
            QString v = x.captured(2);
            if (v.size() > 1 && (v.front() == QLatin1Char('"') || v.front() == QLatin1Char('\''))) v = v.mid(1, v.size() - 2);
            result.insert(module + QLatin1Char('/') + x.captured(1), v);
        }
    }
    return result;
}

bool sameValue(const QString &a0, const QString &b0) {
    auto norm = [](QString s) {
        s = s.trimmed().remove(QLatin1Char('"')).remove(QLatin1Char('\''));
        const QString l = s.toLower();
        if (l == QStringLiteral("y") || l == QStringLiteral("yes") || l == QStringLiteral("true")) return QStringLiteral("1");
        if (l == QStringLiteral("n") || l == QStringLiteral("no") || l == QStringLiteral("false")) return QStringLiteral("0");
        bool ok = false;
        const qulonglong v = l.startsWith(QStringLiteral("0x")) ? l.mid(2).toULongLong(&ok, 16) : l.toULongLong(&ok, 10);
        return ok ? QString::number(v) : s;
    };
    return norm(a0) == norm(b0);
}

Report nvidiaParams(const QString &root) {
    Report r;
    r.code = QStringLiteral("S06");
    r.title = tr("NVIDIA module parameters");
    r.about = tr("The NVIDIA driver options configured in modprobe.d and on the kernel command line, against the values the loaded driver runs with, "
                 "with what the important ones do. The GPU is not woken.");
    r.limits << tr("A changed option takes effect only when the module loads again (reboot). If the NVIDIA modules are in the initramfs, it carries its own copy of modprobe.d.");
    // Effective values: /sys/module/<m>/parameters, and /proc/driver/nvidia/params for NVreg_* (without the prefix).
    QMap<QString, QString> proc;
    for (const QString &line : diagsrc::read(root + QStringLiteral("/proc/driver/nvidia/params")).split(QLatin1Char('\n'))) {
        const int i = line.indexOf(QLatin1Char(':'));
        if (i > 0) proc.insert(line.left(i).trimmed(), line.mid(i + 1).trimmed().remove(QLatin1Char('"')));
    }
    const bool loaded = QFile::exists(root + QStringLiteral("/sys/module/nvidia"));
    auto effective = [&](const QString &key) {
        const QString mod = key.section(QLatin1Char('/'), 0, 0), name = key.section(QLatin1Char('/'), 1);
        QString v = diagsrc::read(root + QStringLiteral("/sys/module/") + mod + QStringLiteral("/parameters/") + name);
        if (v.isEmpty() && mod == QStringLiteral("nvidia")) v = proc.value(name.startsWith(QStringLiteral("NVreg_")) ? name.mid(6) : name);
        return v;
    };
    struct Conf { QString value, source; };
    QMap<QString, Conf> config;
    for (const QString &dir : {QStringLiteral("/usr/lib/modprobe.d"), QStringLiteral("/lib/modprobe.d"), QStringLiteral("/run/modprobe.d"), QStringLiteral("/etc/modprobe.d")})
        for (const QString &file : diagsrc::dir(root + dir, {QStringLiteral("*.conf")}, 128)) {
            const auto opts = parseNvidiaOptionsText(diagsrc::read(root + dir + QLatin1Char('/') + file, 256 * 1024));
            for (auto it = opts.cbegin(); it != opts.cend(); ++it) config.insert(it.key(), {it.value(), dir + QLatin1Char('/') + file});
        }
    static const QRegularExpression cmd(QStringLiteral(R"(^(nvidia(?:[_-](?:drm|uvm|modeset))?)\.([A-Za-z0-9_]+)=(\S+)$)"));
    for (const QString &tok : diagsrc::read(root + QStringLiteral("/proc/cmdline")).split(QLatin1Char(' '), Qt::SkipEmptyParts))
        if (const auto m = cmd.match(tok); m.hasMatch())
            config.insert(m.captured(1).replace(QLatin1Char('-'), QLatin1Char('_')) + QLatin1Char('/') + m.captured(2), {m.captured(3), QStringLiteral("/proc/cmdline")});

    static const QList<std::pair<const char *, const char *>> keyParams = {
        {"nvidia_drm/modeset", QT_TRANSLATE_NOOP("diagnostictools", "kernel mode setting — required for Wayland")},
        {"nvidia_drm/fbdev", QT_TRANSLATE_NOOP("diagnostictools", "NVIDIA's own console framebuffer")},
        {"nvidia/NVreg_DynamicPowerManagement", QT_TRANSLATE_NOOP("diagnostictools", "runtime power-off: 0 never, 1 coarse, 2 fine-grained, 3 automatic")},
        {"nvidia/NVreg_PreserveVideoMemoryAllocations", QT_TRANSLATE_NOOP("diagnostictools", "keep video memory over suspend (needs the nvidia-sleep hooks)")},
        {"nvidia/NVreg_EnableS0ixPowerManagement", QT_TRANSLATE_NOOP("diagnostictools", "let the GPU power off during s2idle sleep")},
        {"nvidia/NVreg_EnableGpuFirmware", QT_TRANSLATE_NOOP("diagnostictools", "GSP firmware (always on with the open kernel module)")},
        {"nvidia/NVreg_UsePageAttributeTable", QT_TRANSLATE_NOOP("diagnostictools", "PAT memory typing")},
        {"nvidia/NVreg_TemporaryFilePath", QT_TRANSLATE_NOOP("diagnostictools", "where video memory is saved on suspend")}};
    Table t;
    t.title = tr("Parameters");
    t.headers = {tr("Parameter"), tr("Configured"), tr("Source"), tr("In effect"), tr("Meaning")};
    QSet<QString> shown;
    auto row = [&](const QString &key, const QString &meaning) {
        shown.insert(key);
        const Conf c = config.value(key);
        const QString eff = effective(key);
        Level l = Level::Info;
        if (!c.value.isEmpty() && !eff.isEmpty() && !sameValue(c.value, eff)) {
            l = Level::Review;
            r.add(Level::Review, tr("%1 is configured as %2 but the driver runs with %3").arg(key, c.value, eff),
                  tr("Set in %1; the module has not been loaded with it.").arg(c.source),
                  tr("Reboot. With the NVIDIA modules in the initramfs (dracut), regenerate it first."));
        } else if (!c.value.isEmpty() && !eff.isEmpty()) {
            l = Level::Ok;
        }
        t.add({key, c.value.isEmpty() ? QStringLiteral("—") : c.value, c.value.isEmpty() ? QString() : c.source,
               eff.isEmpty() ? (loaded ? tr("not exposed") : tr("module not loaded")) : eff, meaning}, l);
    };
    for (const auto &[k, m] : keyParams) row(QLatin1String(k), tr(m));
    for (auto it = config.cbegin(); it != config.cend(); ++it) if (!shown.contains(it.key())) row(it.key(), QString());
    r.tables << t;
    if (!loaded) r.add(Level::Info, tr("The NVIDIA module is not loaded"), tr("Only the configuration can be shown."));
    // Wayland needs modeset.
    const QString modeset = effective(QStringLiteral("nvidia_drm/modeset"));
    if (qEnvironmentVariable("XDG_SESSION_TYPE") == QStringLiteral("wayland") && !modeset.isEmpty() && !sameValue(modeset, QStringLiteral("1")))
        r.add(Level::Attention, tr("Wayland session with nvidia_drm.modeset off"), tr("Without kernel mode setting the compositor cannot use the NVIDIA GPU for displays."),
              tr("Add nvidia_drm.modeset=1 to the kernel command line (or options nvidia_drm modeset=1) and reboot."))
            .setFix(tr("Add options nvidia_drm modeset=1"), QJsonObject{{QStringLiteral("op"), QStringLiteral("config_append")}, {QStringLiteral("id"), QStringLiteral("modprobe")},
                                                                     {QStringLiteral("line"), QStringLiteral("options nvidia_drm modeset=1")}});
    // Preserving video memory needs someone to call nvidia-sleep.sh, unless the driver uses kernel notifiers.
    const QString preserve = effective(QStringLiteral("nvidia/NVreg_PreserveVideoMemoryAllocations"));
    if (sameValue(preserve, QStringLiteral("1")) && !sameValue(proc.value(QStringLiteral("UseKernelSuspendNotifiers")), QStringLiteral("1"))) {
        bool hook = false;
        for (const QString &d : {QStringLiteral("/usr/lib/elogind/system-sleep"), QStringLiteral("/lib64/elogind/system-sleep"), QStringLiteral("/lib/elogind/system-sleep"),
                                 QStringLiteral("/etc/elogind/system-sleep"), QStringLiteral("/usr/lib/systemd/system-sleep")})
            for (const QString &f : diagsrc::dir(root + d)) hook = hook || f.contains(QStringLiteral("nvidia"));
        hook = hook || QFile::exists(root + QStringLiteral("/usr/lib/systemd/system/nvidia-suspend.service")) || QFile::exists(root + QStringLiteral("/lib/systemd/system/nvidia-suspend.service"));
        if (!hook)
            r.add(Level::Review, tr("Video memory preservation is on, but no NVIDIA sleep hook was found"),
                  tr("With PreserveVideoMemoryAllocations=1 the driver expects nvidia-sleep.sh to run around every suspend; without it, resume can leave a black screen or a crashed desktop."),
                  tr("On OpenRC + elogind: add an elogind system-sleep hook that runs nvidia-sleep.sh suspend / resume, or set NVreg_UseKernelSuspendNotifiers=1 on drivers that support it."));
    }
    if (!r.count(Level::Attention) && !r.count(Level::Review)) r.add(Level::Ok, tr("Every configured option is in effect"));
    return r;
}

}  // namespace diagnostictools
