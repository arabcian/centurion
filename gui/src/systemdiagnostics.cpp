#include "hardwarefields.h"
#include "memoryfields.h"
#include "deviceidentity.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDateTime>
#include "topologyfields.h"
#include "systemdiagnostics.h"

#include "advanceddiagnostics.h"

#include <QCoreApplication>
#include <QDirIterator>
#include <QDir>
#include <optional>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

#include <unistd.h>
#include <sys/stat.h>

using diag::Level;
using diag::Report;
using diag::Table;

namespace systemdiagnostics {

static QString tr(const char *s) { return QCoreApplication::translate("systemdiagnostics", s); }
static QString rd(const QString &root, const QString &p, qint64 limit = 64 * 1024) { return diagsrc::read(root + p, limit); }
static QStringList ls(const QString &root, const QString &p, const QStringList &f = {QStringLiteral("*")}) { return diagsrc::dir(root + p, f); }
/// The bracketed choice of a sysfs list ("always [madvise] never" → "madvise"), else the text.
static QString chosen(const QString &s) {
    const auto m = QRegularExpression(QStringLiteral(R"(\[([^\]]+)\])")).match(s);
    return m.hasMatch() ? m.captured(1) : s;
}
static QString driverOf(const QString &root, const QString &devDir) {
    const QFileInfo d(root + devDir + QStringLiteral("/driver"));
    return d.exists() ? QFileInfo(d.canonicalFilePath()).fileName() : QString();
}
static QString watts(double w) { return QStringLiteral("%1 W").arg(w, 0, 'f', w < 10 ? 1 : 0); }

// A sysfs link is useful as identity only if its canonical target is inside the
// selected fixture/host devices tree. Never follow a broken or escaping link.
static QString devicePath(const QString &root, const QString &path) {
    const QString base = QFileInfo(root + QStringLiteral("/sys/devices")).canonicalFilePath();
    const QString target = QFileInfo(root + path).canonicalFilePath();
    return !base.isEmpty() && target.startsWith(base + QLatin1Char('/')) ? target : QString();
}
// Boot-scoped, transient fingerprint: no serial, MAC or persistent machine identity.
static QString deviceKey(const QString &root, const QString &path, const QString &bus, const QString &boot) {
    if (boot.isEmpty()) return {};
    const auto stamp = deviceidentity::capture((root + QStringLiteral("/sys/devices")).toStdString(),
                                               (root + path).toStdString());
    if (!stamp) return {};
    QJsonArray fields{boot, bus, QString::fromStdString(stamp->path), QString::fromStdString(stamp->incarnation)};
    const QString canonical = QStringLiteral("/sys/devices/") + QString::fromStdString(stamp->path);
    const auto append = [&](const QString &name) { fields.append(rd(root, canonical + QLatin1Char('/') + name, 256)); };
    if (bus == QStringLiteral("PCI")) {
        for (const auto &name : {QStringLiteral("vendor"), QStringLiteral("device"), QStringLiteral("subsystem_vendor"),
                                 QStringLiteral("subsystem_device"), QStringLiteral("class"), QStringLiteral("revision")}) append(name);
        if (fields.at(4).toString().isEmpty() || fields.at(5).toString().isEmpty()) return {};
    } else if (bus == QStringLiteral("USB")) {
        append(QStringLiteral("idVendor")); append(QStringLiteral("idProduct"));
    } else if (bus == QStringLiteral("Network")) append(QStringLiteral("ifindex"));
    // Check the directory incarnation again after reading attributes.
    if (deviceidentity::capture((root + QStringLiteral("/sys/devices")).toStdString(),
                                (root + path).toStdString()) != stamp) return {};
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(fields).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
static QString known(const QString &v) { return v.isEmpty() ? tr("unknown") : v; }
static std::optional<double> number(const QString &v) {
    bool ok = false; const double n = v.toDouble(&ok);
    return ok && std::isfinite(n) ? std::optional<double>(n) : std::nullopt;
}

Table deviceTopology(const QString &root, const QJsonObject &sample) {
    struct Device { QString bus, id, path, target; };
    QList<Device> devices;
    const QString boot = rd(root, QStringLiteral("/proc/sys/kernel/random/boot_id"));
    const QList<QPair<QString, QString>> buses{
        {QStringLiteral("PCI"), QStringLiteral("/sys/bus/pci/devices")},
        {QStringLiteral("USB"), QStringLiteral("/sys/bus/usb/devices")},
        {QStringLiteral("NVMe"), QStringLiteral("/sys/class/nvme")},
        {QStringLiteral("Network"), QStringLiteral("/sys/class/net")}};
    for (const auto &bus : buses) for (const auto &id : ls(root, bus.second)) {
        const QString path = bus.second + QLatin1Char('/') + id;
        devices << Device{bus.first, id, path, devicePath(root, path)};
    }
    std::stable_sort(devices.begin(), devices.end(), [](const Device &a, const Device &b) { return a.target.size() < b.target.size(); });
    Table t; t.title = tr("Hardware Device Tree"); t.viewKind = QStringLiteral("hardware-device-tree");
    t.headers = {tr("Device"), tr("Bus"), tr("Parent"), tr("Vendor / device"), tr("Driver"), tr("Link now"), tr("Endpoint maximum"), tr("NUMA node"), tr("Runtime state"), tr("AER (corr / non-fatal / fatal)"), tr("Link / AER capture (UTC)")};
    t.note = tr("Relationships use canonical sysfs paths only. Link and AER readings are reused from the existing PCIe scan; missing relationships and readings are unknown.") + QLatin1Char('\n') + tr("Identity is boot-scoped sysfs continuity, not proof of physical identity. NUMA -1 means no assigned node. Link reads are skipped for non-active devices; runtime PM may still race an active read.");
    QHash<QString, int> index;
    for (const auto &d : devices) {
        int parent = -1;
        if (!d.target.isEmpty()) {
            QString up = QFileInfo(d.target).path();
            // Strictly shorten every iteration, with a depth bound.
            for (int depth = 0; depth < 64 && up.size() > 1; ++depth) {
                if (index.contains(up)) { parent = index.value(up); break; }
                const QString next = QFileInfo(up).path(); if (next == up) break; up = next;
            }
        }
        const auto value = [&](const QString &key) { return d.target.isEmpty() ? QString() : diagsrc::read(d.target + QLatin1Char('/') + key); };
        QString runtime = value(QStringLiteral("power/runtime_status"));
        QString key = deviceKey(root, d.path, d.bus, boot);
        QJsonObject link = d.bus == QStringLiteral("PCI") ? sample.value(d.id).toObject() : QJsonObject{};
        QString vendor, device;
        if (d.bus == QStringLiteral("PCI")) { vendor = value(QStringLiteral("vendor")); device = value(QStringLiteral("device")); }
        if (d.bus == QStringLiteral("USB")) { vendor = value(QStringLiteral("idVendor")); device = value(QStringLiteral("idProduct")); }
        if (key.isEmpty() || link.value(QStringLiteral("identity")).toString() != key) link = {};
        QString driver = d.target.isEmpty() ? QString() : driverOf(root, d.path);
        QString numa = value(QStringLiteral("numa_node"));
        const bool attached = !d.target.isEmpty() && devicePath(root, d.path) == d.target &&
            (key.isEmpty() || deviceKey(root, d.path, d.bus, boot) == key);
        if (!attached) { parent = -1; key.clear(); vendor.clear(); device.clear(); driver.clear(); numa.clear(); runtime.clear(); link = {}; }
        const QString identity = vendor.isEmpty() || device.isEmpty() ? tr("unknown") : vendor + QStringLiteral(" / ") + device;
        t.parents << parent;
        t.rowKeys << key;
        t.add({d.id, d.bus, parent >= 0 ? t.rows.at(parent).value(0) : tr("unknown"), identity,
               known(driver), known(link.value(QStringLiteral("link")).toString()), known(link.value(QStringLiteral("maximum")).toString()),
               known(numa), known(runtime), known(link.value(QStringLiteral("aer")).toString()), known(link.value(QStringLiteral("capturedAt")).toString())}, !attached || key.isEmpty() ? Level::Unknown : Level::Info);
        if (attached && !index.contains(d.target)) index.insert(d.target, int(t.rows.size()) - 1);
    }
    return t;
}

double linkSpeed(const QString &s) { return QRegularExpression(QStringLiteral(R"(([\d.]+)\s*GT/s)")).match(s).captured(1).toDouble(); }
static int pcieGen(double gts) { return gts >= 60 ? 6 : gts >= 30 ? 5 : gts >= 15 ? 4 : gts >= 7 ? 3 : gts >= 4 ? 2 : gts > 0 ? 1 : 0; }

// ── S04 PCIe Link Negotiation Lab ─────────────────────────────────────────────

Report pcieLinks(const QString &root, const QJsonObject &previous, QJsonObject *sample) {
    const QString boot = rd(root, QStringLiteral("/proc/sys/kernel/random/boot_id"));
    const bool sameBoot = !boot.isEmpty() && previous.value(QStringLiteral("boot")).toString() == boot;
    const QJsonObject before = sameBoot ? previous.value(QStringLiteral("links")).toObject() : QJsonObject{};
    QJsonObject captured;
    Table linkDelta;
    linkDelta.title = tr("Link Changes Since Previous Scan");
    linkDelta.headers = {tr("PCI address"), tr("Previous link"), tr("Current link"), tr("Comparison")};
    Report r;
    r.code = QStringLiteral("S04");
    r.title = tr("PCIe Link Check");
    r.about = tr("The PCIe link every device trained at, against what both ends of the link can do. A narrower link than possible is a "
                 "hardware or BIOS limit; a slower one at idle is normal power saving.");
    r.limits << tr("Speed drops to save power when a device is idle (GPUs go to PCIe 1.0 at the desktop); check under load before judging the speed.")
             << tr("Non-active or unknown runtime state is not probed for PCIe link registers. Runtime PM may race an active read.")
             << tr("Same-boot sysfs identity is best-effort continuity; identical physical replacement may not be detectable.");
    Table t;
    t.title = tr("Device Links");
    t.headers = {tr("Device"), tr("Driver"), tr("Link now"), tr("Possible"), tr("Power"), tr("AER (corr / non-fatal / fatal)"), tr("Status")};
    Table topology;
    topology.title = tr("PCIe Upstream Relationships");
    topology.headers = {tr("Device"), tr("Upstream PCI address"), tr("Device maximum"), tr("Upstream maximum")};
    topology.note = tr("Parent relationships come from resolved sysfs device paths. Unknown or root-bus parents are not inferred.");
    static const QRegularExpression pciAddress(QStringLiteral(R"(^[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]$)"));
    int bridges = 0;
    for (const QString &id : ls(root, QStringLiteral("/sys/bus/pci/devices"))) {
        const QString base = QStringLiteral("/sys/bus/pci/devices/") + id;
        const QString target = devicePath(root, base);
        if (target.isEmpty() || !QFileInfo::exists(target + QStringLiteral("/current_link_speed"))) continue;
        const QString cls = rd(root, base + QStringLiteral("/class"));
        if (cls.startsWith(QStringLiteral("0x0604"))) { ++bridges; continue; }  // ports: their downstream device is listed
        const QString identityKey = deviceKey(root, base, QStringLiteral("PCI"), boot);
        const QString power = rd(root, base + QStringLiteral("/power/runtime_status"));
        if (!deviceidentity::mayReadLink(power.toStdString())) {
            t.add({id, driverOf(root, base), QStringLiteral("—"), QStringLiteral("—"), known(power), QStringLiteral("—"), tr("Link not probed (runtime state)")}, Level::Unknown);
            r.add(Level::Unknown, tr("%1: link not probed (runtime state)").arg(id));
            continue;
        }
        const QString cur = rd(root, base + QStringLiteral("/current_link_speed")), max = rd(root, base + QStringLiteral("/max_link_speed"));
        if (cur.isEmpty() || cur.startsWith(QStringLiteral("Unknown"))) {
            r.add(Level::Unknown, tr("%1: link reading unavailable").arg(id));
            continue;
        }
        const int w = rd(root, base + QStringLiteral("/current_link_width")).toInt(), mw = rd(root, base + QStringLiteral("/max_link_width")).toInt();
        // The link can only be as fast / wide as the port above it.
        const QString up = QFileInfo(target).path();
        const QString parent = QFileInfo(up).fileName();
        const bool parentKnown = pciAddress.match(parent).hasMatch();
        const bool portActive = parentKnown && deviceidentity::mayReadLink(diagsrc::read(up + QStringLiteral("/power/runtime_status")).toStdString());
        const double portMax = portActive ? linkSpeed(diagsrc::read(up + QStringLiteral("/max_link_speed"))) : 0;
        const int portW = portActive ? diagsrc::read(up + QStringLiteral("/max_link_width")).toInt() : 0;
        auto link = [](double rate, int width) {
            return rate > 0 && width > 0 ? QStringLiteral("%1 GT/s ×%2").arg(rate).arg(width) : QStringLiteral("—");
        };
        topology.add({id, parentKnown ? parent : QStringLiteral("—"), link(linkSpeed(max), mw),
                      parentKnown ? link(portMax, portW) : QStringLiteral("—")});
        const double s = linkSpeed(cur), ms = std::min(linkSpeed(max), portMax > 0 ? portMax : 1e9);
        const int mw2 = portW > 0 ? std::min(mw, portW) : mw;
        static const std::pair<const char *, const char *> aerFiles[] = {
            {"aer_dev_correctable", "TOTAL_ERR_COR"}, {"aer_dev_nonfatal", "TOTAL_ERR_NONFATAL"}, {"aer_dev_fatal", "TOTAL_ERR_FATAL"}};
        QStringList aer;
        for (const auto &[f, k] : aerFiles) aer << advanceddiagnostics::aerTotal(rd(root, base + QLatin1Char('/') + QLatin1String(f)), QLatin1String(k));
        const bool hasAer = std::any_of(aer.begin(), aer.end(), [](const QString &x) { return !x.isEmpty(); });
        const QString vendor = rd(root, base + QStringLiteral("/vendor")), device = rd(root, base + QStringLiteral("/device"));
        const QString name = diagsrc::pciName(vendor, device, cls);
        const QString current = link(s,w);
        if (devicePath(root, base) != target || (!identityKey.isEmpty() && deviceKey(root, base, QStringLiteral("PCI"), boot) != identityKey) ||
            rd(root, base + QStringLiteral("/power/runtime_status")) != power) {
            r.add(Level::Unknown, tr("%1: device or runtime state changed during capture").arg(id));
            continue;
        }
        captured.insert(id, QJsonObject{{"identity", identityKey}, {"capturedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}, {"vendor", vendor}, {"device", device}, {"link", current}, {"maximum", link(linkSpeed(max), mw)}, {"aer", hasAer ? aer.join(QStringLiteral(" / ")) : QString()}});
        const QJsonObject old = before.value(id).toObject();
        if (!old.isEmpty()) {
            const QString oldLink = old.value(QStringLiteral("link")).toString();
            const bool identity = !vendor.isEmpty() && !device.isEmpty() && !identityKey.isEmpty() &&
                old.value(QStringLiteral("vendor")).toString() == vendor && old.value(QStringLiteral("device")).toString() == device &&
                old.value(QStringLiteral("identity")).toString() == identityKey;
            if (!identity) linkDelta.add({id, oldLink, current, tr("Endpoint identity changed at this PCI address or is unverified")}, Level::Unknown);
            else if (oldLink.isEmpty() || oldLink == QStringLiteral("—") || current == QStringLiteral("—")) linkDelta.add({id, oldLink, current, tr("Not readable in this scan")}, Level::Unknown);
            else if (oldLink != current) linkDelta.add({id, oldLink, current, tr("Link reading changed; power state may differ")});
        } else if (sameBoot) linkDelta.add({id, QStringLiteral("—"), current, tr("Newly readable or added endpoint")}, Level::Info);
        Level l = Level::Ok;
        QString status = tr("full link");
        if (mw2 > 0 && w > 0 && w < mw2) {
            l = Level::Review;
            status = tr("narrower: x%1 of x%2").arg(w).arg(mw2);
            r.add(Level::Review, tr("%1 (%2) runs at x%3 of a possible x%4").arg(name, id).arg(w).arg(mw2),
                  tr("Width does not drop for power saving the way speed does: a narrower link usually means a seating, riser or BIOS lane-split limit, "
                     "or lanes the board does not wire (check the laptop's specification)."),
                  tr("Compare after a cold boot; if it stays narrow on a desktop, reseat the card."));
        } else if (ms > 0 && s > 0 && s < ms) {
            l = Level::Info;
            status = tr("slower now (PCIe %1 of %2)").arg(pcieGen(s)).arg(pcieGen(ms));
        }
        if (mw2 <= 0 || w <= 0 || ms <= 0 || s <= 0) {
            l = Level::Unknown; status = tr("Link speed or width unknown");
            r.add(Level::Unknown, tr("%1 (%2): link speed or width could not be verified").arg(name,id));
        }
        if (aer.size() == 3 && (aer[1].toULongLong() > 0 || aer[2].toULongLong() > 0)) {
            l = Level::Attention;
            r.add(Level::Attention, tr("%1 (%2): uncorrectable PCIe errors recorded").arg(name,id));
        }
        const auto displayLink = [&](double rate, int width) {
            return rate > 0 && width > 0 ? QStringLiteral("PCIe %1 x%2 (%3 GT/s)").arg(pcieGen(rate)).arg(width).arg(rate) : QStringLiteral("—");
        };
        t.add({QStringLiteral("%1 · %2").arg(id, name), driverOf(root, base), displayLink(s,w),
               displayLink(ms,mw2), power.isEmpty() ? QStringLiteral("—") : power,
               hasAer ? aer.join(QStringLiteral(" / ")) : QStringLiteral("—"), status}, l);
    }
    t.note = tr("%1 PCIe port(s) / bridge(s) not listed. Possible = the lower of the device's and its port's maximum.").arg(bridges) + QLatin1Char('\n') + tr("If upstream capability is unavailable, Possible shows the endpoint maximum only.");
    r.tables << t;
    if (!topology.rows.isEmpty()) r.tables << topology;
    r.tables << deviceTopology(root, captured);
    if (sameBoot) {
        for (auto it = before.begin(); it != before.end(); ++it)
            if (!captured.contains(it.key())) linkDelta.add({it.key(), it.value().toObject().value(QStringLiteral("link")).toString(), QStringLiteral("—"), tr("Not readable in this scan")}, Level::Unknown);
        if (!linkDelta.rows.isEmpty()) r.tables << linkDelta;
        else r.limits << tr("No link reading changed among the sampled devices.");
    } else r.limits << tr("Link comparison starts with the next scan of this boot.");
    if (sample) *sample = QJsonObject{{"boot", boot}, {"links", captured}};
    if (t.rows.isEmpty()) r.add(Level::Unknown, tr("No device reports a PCIe link"));
    else if (!r.count(Level::Review) && !r.count(Level::Attention) && !r.count(Level::Unknown)) r.add(Level::Ok, tr("Every device trained at its full link width"),
                                            tr("Lower speeds shown are power saving unless they stay low under load."));
    return r;
}

// ── S07 Wi-Fi / Bluetooth Firmware Doctor ─────────────────────────────────────

static QString firmwarePresent(const QString &root, const QString &file) {
    for (const QString &base : {QStringLiteral("/lib/firmware/"), QStringLiteral("/usr/lib/firmware/")})
        for (const QString &ext : {QString(), QStringLiteral(".zst"), QStringLiteral(".xz")})
            if (QFile::exists(root + base + file + ext)) return base + file + ext;
    return {};
}

/// "Direct firmware load for X failed with error -2" (and "firmware: failed to load X"), newest last.
static QMap<QString, QString> missingFirmware(const QList<diagsrc::LogLine> &kernel) {
    static const QRegularExpression direct(QStringLiteral(R"(Direct firmware load for (\S+) failed with error (-?\d+))"));
    static const QRegularExpression failed(QStringLiteral(R"(firmware: failed to load (\S+) \((-?\d+)\))"));
    QMap<QString, QString> out;  // file → "driver message"
    for (const diagsrc::LogLine &l : kernel) {
        auto m = direct.match(l.text);
        if (!m.hasMatch()) m = failed.match(l.text);
        if (m.hasMatch()) out.insert(m.captured(1), l.text.section(QLatin1Char(' '), 0, 1));
    }
    return out;
}

Report radios(const QList<diagsrc::LogLine> &kernel, const QString &root) {
    Report r;
    r.code = QStringLiteral("S07");
    r.title = tr("Wi-Fi & Bluetooth Check");
    r.about = tr("Each wireless adapter with its driver and state, radio kill switches, and firmware files the kernel asked for and did not find.");
    r.limits << tr("A firmware file present on disk is not proof that it is the version the driver wants; the kernel log says what it loaded.");
    Table a;
    a.title = tr("Adapters");
    a.headers = {tr("Interface"), tr("Device"), tr("Driver"), tr("State")};
    for (const QString &ifc : ls(root, QStringLiteral("/sys/class/net"))) {
        const QString b = QStringLiteral("/sys/class/net/") + ifc;
        if (!QFile::exists(root + b + QStringLiteral("/wireless")) && !QFile::exists(root + b + QStringLiteral("/phy80211"))) continue;
        const QString dev = b + QStringLiteral("/device");
        const QString name = diagsrc::pciName(rd(root, dev + QStringLiteral("/vendor")), rd(root, dev + QStringLiteral("/device")), rd(root, dev + QStringLiteral("/class")));
        const QString state = rd(root, b + QStringLiteral("/operstate"));
        a.add({ifc, name, driverOf(root, dev), state}, state == QStringLiteral("up") ? Level::Ok : Level::Info);
    }
    for (const QString &hci : ls(root, QStringLiteral("/sys/class/bluetooth"), {QStringLiteral("hci*")})) {
        const QString dev = QStringLiteral("/sys/class/bluetooth/") + hci + QStringLiteral("/device");
        QString name = rd(root, dev + QStringLiteral("/../product"));
        if (name.isEmpty()) name = QStringLiteral("USB %1:%2").arg(rd(root, dev + QStringLiteral("/../idVendor")), rd(root, dev + QStringLiteral("/../idProduct")));
        a.add({hci, name, driverOf(root, dev), tr("registered")});
    }
    if (a.rows.isEmpty()) r.add(Level::Unknown, tr("No wireless adapter registered"), tr("Either there is none, or its driver did not load (S18 lists devices without a driver)."));
    r.tables << a;
    // Kill switches.
    Table k;
    k.title = tr("Radio Switches (rfkill)");
    k.headers = {tr("Name"), tr("Type"), tr("Software"), tr("Hardware")};
    for (const QString &rf : ls(root, QStringLiteral("/sys/class/rfkill"), {QStringLiteral("rfkill*")})) {
        const QString b = QStringLiteral("/sys/class/rfkill/") + rf;
        const bool soft = rd(root, b + QStringLiteral("/soft")) == QStringLiteral("1"), hard = rd(root, b + QStringLiteral("/hard")) == QStringLiteral("1");
        const QString name = rd(root, b + QStringLiteral("/name")), type = rd(root, b + QStringLiteral("/type"));
        k.add({name, type, soft ? tr("blocked") : tr("on"), hard ? tr("blocked") : tr("on")}, hard || soft ? Level::Review : Level::Ok);
        if (hard) r.add(Level::Review, tr("%1 (%2) is blocked in hardware").arg(name, type), tr("A switch, key combination or BIOS setting turned it off; software cannot undo it."));
        else if (soft) {
            r.add(Level::Info, tr("%1 (%2) is turned off in software").arg(name, type), tr("Airplane mode or the desktop's switch; rfkill unblock turns it back on."))
                .setFix(tr("Turn %1 on").arg(type), QJsonObject{{QStringLiteral("op"), QStringLiteral("rfkill")},
                                                               {QStringLiteral("index"), rf.mid(6).toInt()}, {QStringLiteral("block"), false}});
        }
    }
    if (!k.rows.isEmpty()) r.tables << k;
    // Missing firmware (any driver: the same evidence matters for GPU and audio firmware too).
    const QMap<QString, QString> miss = missingFirmware(kernel);
    if (!miss.isEmpty()) {
        Table f;
        f.title = tr("Firmware the Kernel Could Not Load (Saved Kernel Log)");
        f.headers = {tr("File"), tr("Requested by"), tr("On disk now")};
        for (auto it = miss.cbegin(); it != miss.cend(); ++it) {
            const QString found = firmwarePresent(root, it.key());
            f.add({it.key(), it.value(), found.isEmpty() ? tr("missing") : found}, found.isEmpty() ? Level::Review : Level::Info);
            if (found.isEmpty() && (it.key().contains(QStringLiteral("mediatek")) || it.key().contains(QStringLiteral("iwlwifi")) || it.key().contains(QStringLiteral("rtw")) ||
                                    it.key().contains(QStringLiteral("ath")) || it.key().contains(QStringLiteral("brcm")) || it.key().contains(QStringLiteral("BT_RAM"))))
                r.add(Level::Attention, tr("Radio firmware %1 is missing").arg(it.key()),
                      tr("%1 asked for it and the file is not installed. Drivers sometimes try several versions; it matters if the adapter does not work.").arg(it.value()),
                      tr("Install / update the firmware package (Gentoo: sys-kernel/linux-firmware; check its USE / savedconfig selection)."));
        }
        f.note = tr("Some drivers ask for optional or newer files first and fall back quietly; a missing file matters when its device does not work.");
        r.tables << f;
    }
    if (!r.count(Level::Attention) && !r.count(Level::Review) && !a.rows.isEmpty())
        r.add(Level::Ok, tr("Adapters registered, no radio blocked, no radio firmware missing"));
    return r;
}

// ── S08 Power Limit Source Map ────────────────────────────────────────────────

Report powerLimits(const QString &root) {
    Report r;
    r.code = QStringLiteral("S08");
    r.title = tr("Power Limits");
    r.about = tr("Every place a CPU / platform power limit is visible without waking anything: firmware power settings (PPT), the platform "
                 "profile, cpufreq caps, RAPL, and the power source.");
    r.limits << tr("The SMU's live limits (STAPM, fast / slow PPT in effect) are not exported by the kernel; the firmware settings here are the targets it was given.")
             << tr("The NVIDIA GPU's power limit needs nvidia-smi, which wakes the GPU: see the NVIDIA tab.");
    Table t;
    t.title = tr("Limits and Their Sources");
    t.headers = {tr("Source"), tr("Setting"), tr("Value"), tr("Range")};
    const QString profile = rd(root, QStringLiteral("/sys/firmware/acpi/platform_profile"));
    if (!profile.isEmpty()) t.add({tr("ACPI platform profile"), QStringLiteral("platform_profile"), profile, rd(root, QStringLiteral("/sys/firmware/acpi/platform_profile_choices"))});
    // Firmware power attributes (lenovo-wmi-other, asus-armoury, …): integers only, never authentication.
    for (const QString &prov : ls(root, QStringLiteral("/sys/class/firmware-attributes"))) {
        const QString base = QStringLiteral("/sys/class/firmware-attributes/") + prov + QStringLiteral("/attributes/");
        for (const QString &a : ls(root, base)) {
            const QString low = a.toLower();
            if (!(low.contains(QStringLiteral("ppt")) || low.contains(QStringLiteral("spl")) || low.contains(QStringLiteral("tdp")) ||
                  low.contains(QStringLiteral("power_limit")) || low.contains(QStringLiteral("nv_")) || low.contains(QStringLiteral("dynamic_boost"))))
                continue;
            if (rd(root, base + a + QStringLiteral("/type")) != QStringLiteral("integer")) continue;
            const QString v = rd(root, base + a + QStringLiteral("/current_value"), 64);
            const QString mn = rd(root, base + a + QStringLiteral("/min_value"), 64), mx = rd(root, base + a + QStringLiteral("/max_value"), 64);
            const QString disp = rd(root, base + a + QStringLiteral("/display_name"), 200);
            t.add({tr("Firmware (%1)").arg(prov), disp.isEmpty() ? a : QStringLiteral("%1 (%2)").arg(disp, a), v.isEmpty() ? tr("unreadable") : v,
                   mn.isEmpty() || mx.isEmpty() ? QString() : QStringLiteral("%1 … %2").arg(mn, mx)});
        }
    }
    // cpufreq: a scaling cap below the hardware maximum.
    const qint64 hwMax = rd(root, QStringLiteral("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq")).toLongLong();
    qint64 capMin = 0;
    int capped = 0, cpus = 0, readableCpus = 0;
    for (const QString &c : ls(root, QStringLiteral("/sys/devices/system/cpu/cpufreq"), {QStringLiteral("policy*")})) {
        const qint64 m = rd(root, QStringLiteral("/sys/devices/system/cpu/cpufreq/") + c + QStringLiteral("/scaling_max_freq")).toLongLong();
        const qint64 hm = rd(root, QStringLiteral("/sys/devices/system/cpu/cpufreq/") + c + QStringLiteral("/cpuinfo_max_freq")).toLongLong();
        ++cpus;
        if (m > 0 && hm > 0) ++readableCpus;
        if (m > 0 && hm > 0 && m < hm * 98 / 100) { ++capped; capMin = capMin ? std::min(capMin, m) : m; }
    }
    if (hwMax) t.add({tr("cpufreq"), tr("highest clock (hardware)"), QStringLiteral("%1 MHz").arg(hwMax / 1000), QString()});
    if (capped) {
        t.add({tr("cpufreq"), tr("scaling_max_freq cap"), QStringLiteral("%1 MHz").arg(capMin / 1000), tr("%1 of %2 policies").arg(capped).arg(cpus)}, Level::Review);
        r.add(Level::Review, tr("%1 CPU policies are capped below their top clock (lowest cap %2 MHz)").arg(capped).arg(capMin / 1000),
              tr("Something set scaling_max_freq: a power daemon (TLP, power-profiles-daemon, auto-cpufreq), a thermal tool or a script."),
              tr("If it is not deliberate, find the tool that writes it; Optimizations shows Centurion's own settings."))
            .setFix(tr("Raise the cap to the hardware maximum"), QJsonObject{{QStringLiteral("op"), QStringLiteral("sysfs_set")}, {QStringLiteral("key"), QStringLiteral("cpu_uncap")}});
    }
    const QString boost = rd(root, QStringLiteral("/sys/devices/system/cpu/cpufreq/boost"));
    if (boost == QStringLiteral("0") || boost == QStringLiteral("1")) {
        t.add({tr("cpufreq"), QStringLiteral("boost"), boost == QStringLiteral("1") ? tr("on") : tr("off")}, boost == QStringLiteral("1") ? Level::Info : Level::Review);
        if (boost != QStringLiteral("1"))
            r.add(Level::Review, tr("CPU boost is off"), tr("The CPU stays at its base clock: much slower, cooler, less power."))
                .setFix(tr("Turn CPU boost on"), QJsonObject{{QStringLiteral("op"), QStringLiteral("sysfs_set")}, {QStringLiteral("key"), QStringLiteral("cpu_boost")}, {QStringLiteral("value"), QStringLiteral("1")}});
    }
    const QString pstate = rd(root, QStringLiteral("/sys/devices/system/cpu/amd_pstate/status"));
    if (!pstate.isEmpty()) t.add({tr("amd-pstate"), QStringLiteral("status"), pstate, QString()});
    const QString epp = rd(root, QStringLiteral("/sys/devices/system/cpu/cpufreq/policy0/energy_performance_preference"));
    if (!epp.isEmpty()) t.add({tr("amd-pstate"), tr("energy preference (EPP)"), epp, rd(root, QStringLiteral("/sys/devices/system/cpu/cpufreq/policy0/energy_performance_available_preferences"))});
    for (const QString &z : ls(root, QStringLiteral("/sys/class/powercap"))) {
        const QString b = QStringLiteral("/sys/class/powercap/") + z + QLatin1Char('/');
        const QString name = rd(root, b + QStringLiteral("name"));
        if (name.isEmpty()) continue;
        for (int c = 0; c < 3; ++c) {
            const QString lim = rd(root, b + QStringLiteral("constraint_%1_power_limit_uw").arg(c));
            if (lim.isEmpty()) continue;
            t.add({QStringLiteral("RAPL %1").arg(z), QStringLiteral("%1 / %2").arg(name, rd(root, b + QStringLiteral("constraint_%1_name").arg(c))),
                   number(lim) && *number(lim) >= 0 ? watts(*number(lim) / 1e6) : tr("unknown"), known(rd(root, b + QStringLiteral("enabled")))});
        }
    }
    // Power source.
    bool ac = false, onBattery = false;
    for (const QString &ps : ls(root, QStringLiteral("/sys/class/power_supply"))) {
        const QString b = QStringLiteral("/sys/class/power_supply/") + ps + QLatin1Char('/');
        const QString type = rd(root, b + QStringLiteral("type"));
        if (type == QStringLiteral("Mains") && rd(root, b + QStringLiteral("online")) == QStringLiteral("1")) ac = true;
        if (type == QStringLiteral("Battery") && rd(root, b + QStringLiteral("status")) == QStringLiteral("Discharging")) onBattery = true;
    }
    t.add({tr("Power source"), tr("now"), ac ? tr("AC adapter") : onBattery ? tr("battery") : tr("unknown"), QString()}, onBattery ? Level::Review : Level::Info);
    if (onBattery) r.add(Level::Info, tr("Running on battery"), tr("Firmware applies its battery power limits (often less than half the AC limits); measurements taken now do not hold on AC."));
    // NVIDIA Dynamic Boost moves power between CPU and GPU.
    bool powerd = false;
    for (const QString &pid : ls(root, QStringLiteral("/proc"), {QStringLiteral("[0-9]*")}))
        if (rd(root, QStringLiteral("/proc/") + pid + QStringLiteral("/comm"), 64) == QStringLiteral("nvidia-powerd")) { powerd = true; break; }
    t.add({tr("NVIDIA Dynamic Boost"), QStringLiteral("nvidia-powerd"), powerd ? tr("running") : tr("not running"), QString()});
    if (powerd) r.add(Level::Info, tr("NVIDIA Dynamic Boost is active"),
                      tr("nvidia-powerd shifts the shared power budget between CPU and GPU: CPU limits measured under a GPU load are lower than at a CPU-only load."));
    r.tables << t;
    Table hierarchy; hierarchy.title = tr("Power Limit Sources");
    hierarchy.headers = t.headers;
    hierarchy.note = tr("Grouped by reported source, not a causal enforcement order. Firmware attribute units are not inferred; live SMU limits may differ.");
    QMap<QString, int> sourceRows;
    for (const auto &row : t.rows) {
        const QString source = row.value(0);
        if (!sourceRows.contains(source)) {
            sourceRows.insert(source, int(hierarchy.rows.size()));
            hierarchy.parents << -1; hierarchy.add({source});
        }
        hierarchy.parents << sourceRows.value(source); hierarchy.add(row);
    }
    r.tables << hierarchy;
    if (cpus == 0 || readableCpus != cpus || (boost != QStringLiteral("0") && boost != QStringLiteral("1"))) r.add(Level::Unknown, tr("CPU cap or boost state is incomplete"));
    else if (!r.count(Level::Review) && !r.count(Level::Attention)) r.add(Level::Ok, tr("No unexpected cap on CPU clocks or boost"));
    return r;
}

Table cpuTopology(const QString &root) {
    Table cpu; cpu.title = tr("CPU Package and Core Topology");
    cpu.headers = {tr("Domain"), tr("Logical CPU"), tr("Online"), tr("Thread siblings"), tr("Core type"), tr("Die ID"), tr("Topology evidence")};
    cpu.note = tr("Core groups require mutually consistent sibling lists. Core type is the kernel's raw value; frequency and SMT do not establish P/E type.");
    struct Entry { QString name, base, siblings; topologyfields::Cpu data; };
    QList<Entry> entries;
    QHash<int, int> byId;
    auto parseInt = [](const QString &v) { return topologyfields::integer(v.toStdString()); };
    const auto online = topologyfields::cpuList(rd(root, QStringLiteral("/sys/devices/system/cpu/online")).toStdString());
    for (const auto &name : ls(root, QStringLiteral("/sys/devices/system/cpu"), {QStringLiteral("cpu[0-9]*")})) {
        const auto id = parseInt(name.mid(3));
        if (!id || byId.contains(*id) || entries.size() >= 65536) continue;
        const QString base = QStringLiteral("/sys/devices/system/cpu/") + name + QLatin1Char('/');
        const auto siblings = rd(root, base + QStringLiteral("topology/thread_siblings_list"));
        topologyfields::Cpu data{*id, parseInt(rd(root, base + QStringLiteral("topology/physical_package_id"))),
            parseInt(rd(root, base + QStringLiteral("topology/core_id"))), parseInt(rd(root, base + QStringLiteral("topology/die_id"))),
            topologyfields::cpuList(siblings.toStdString(), 256)};
        byId.insert(*id, int(entries.size())); entries << Entry{name, base, siblings, std::move(data)};
    }
    QMap<int, int> packages, cores;
    for (const auto &entry : entries) {
        const auto &d = entry.data;
        int parent = -1;
        if (d.package) {
            if (!packages.contains(*d.package)) { packages.insert(*d.package, int(cpu.rows.size())); cpu.parents << -1; cpu.add({tr("Package %1").arg(*d.package)}); }
            parent = packages.value(*d.package);
        }
        // Limit comparisons even for corrupt input claiming tens of thousands of SMT siblings.
        bool verified = d.siblings && !d.siblings->empty() && d.siblings->size() <= 256 && topologyfields::sameCore(d, d);
        if (verified) for (int sibling : *d.siblings) {
            const auto found = byId.constFind(sibling);
            if (found == byId.cend() || !topologyfields::sameCore(d, entries.at(found.value()).data)) { verified = false; break; }
        }
        if (verified) {
            const int key = d.siblings->front();
            if (!cores.contains(key)) { cores.insert(key, int(cpu.rows.size())); cpu.parents << parent; cpu.add({tr("Core %1").arg(*d.core)}); }
            parent = cores.value(key);
        }
        QString state = rd(root, entry.base + QStringLiteral("online"));
        if (state != QStringLiteral("0") && state != QStringLiteral("1")) state.clear();
        if (online) {
            const QString global = std::binary_search(online->begin(), online->end(), d.id) ? QStringLiteral("1") : QStringLiteral("0");
            if (state.isEmpty()) state = global;
            else if (state != global) state.clear(); // conflicting reads, possibly CPU hotplug
        }
        cpu.parents << parent;
        cpu.add({entry.name, entry.name, known(state), known(entry.siblings), known(rd(root, entry.base + QStringLiteral("topology/core_type"))),
                 d.die ? QString::number(*d.die) : tr("unknown"), verified ? tr("Consistent sibling declarations") : tr("Unverified core relationship")},
                 verified && !state.isEmpty() ? Level::Info : Level::Unknown);
    }
    return cpu;
}

// ── S10 Memory Architecture Explorer ──────────────────────────────────────────

Report memory(const QString &root) {
    Report r;
    r.code = QStringLiteral("S10");
    r.title = tr("Memory Layout");
    r.about = tr("How RAM, swap, compressed swap (zram / zswap), huge pages and memory pressure are set up and behaving right now.");
    r.limits << tr("Pressure (PSI) is an average over the last seconds / minutes, not since boot.")
             << tr("DIMM details (speed, timings, part numbers) need root: System tools → DMI.");
    r.limits << tr("RAM and swap totals are the kernel /proc view, not necessarily the current cgroup limit or physical DIMM capacity. Separate snapshots may differ; do not add zram logical capacity to RAM or swap totals.");
    // Reject truncated input rather than accept the first part of a snapshot.
    const auto snapshot = [&root](const QString &path, qint64 limit) {
        QFile f(root + path);
        if (!f.open(QIODevice::ReadOnly)) return QByteArray{};
        QByteArray bytes;
        while (bytes.size() <= limit) {
            const auto chunk = f.read(std::min<qint64>(4096, limit+1-bytes.size()));
            if (chunk.isEmpty()) break;
            bytes += chunk;
        }
        return f.error() == QFileDevice::NoError && bytes.size() <= limit && bytes.endsWith('\n') ? bytes : QByteArray{};
    };
    const auto view = [](const QByteArray &bytes) { return std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())); };
    const auto rawMemory = snapshot(QStringLiteral("/proc/meminfo"), 65536);
    const auto mi = memoryfields::meminfo(view(rawMemory));
    auto gib = [](double g) { return QStringLiteral("%1 GiB").arg(g, 0, 'f', g < 10 ? 1 : 0); };
    const auto bytesText = [&gib](std::uint64_t n) {
        if (n < 1048576) return QStringLiteral("%1 B").arg(qulonglong(n));
        if (n < 1073741824) return QStringLiteral("%1 MiB").arg(double(n) / 1048576.0, 0, 'f', 2);
        return gib(double(n) / 1073741824.0);
    };
    Table t;
    t.title = tr("Memory Now");
    t.headers = {tr("Item"), tr("Value"), tr("Note")};
    if (mi.total && *mi.total > 0 && mi.available && *mi.available <= *mi.total) {
        const double fraction = double(*mi.available) / double(*mi.total);
        t.add({tr("RAM"), bytesText(*mi.total), tr("%1 available (%2 %)").arg(bytesText(*mi.available)).arg(fraction * 100, 0, 'f', 0)}, fraction < 0.1 ? Level::Review : Level::Info);
        if (fraction < 0.1) r.add(Level::Review, tr("Less than 10 % of RAM is available"), tr("The kernel is close to reclaiming hard or swapping."), tr("Close something large or add swap."));
    } else {
        t.add({tr("RAM"), mi.total && *mi.total > 0 ? bytesText(*mi.total) : tr("unknown"), tr("RAM availability is incomplete")}, Level::Unknown);
        r.add(Level::Unknown, tr("RAM availability is incomplete"));
    }
    const bool swapKnown = mi.swapTotal && mi.swapFree && *mi.swapFree <= *mi.swapTotal;
    t.add({tr("Swap"), !swapKnown ? tr("unknown") : *mi.swapTotal > 0 ? tr("%1 of %2 used").arg(bytesText(*mi.swapTotal - *mi.swapFree), bytesText(*mi.swapTotal)) : tr("none"), QString()}, swapKnown ? Level::Info : Level::Unknown);
    if (!swapKnown) r.add(Level::Unknown, tr("Swap accounting is incomplete"));
    // Identify active swap entries from the captured /proc/swaps list, not the
    // existence or configured size of a zram block device.
    const auto rawSwaps = snapshot(QStringLiteral("/proc/swaps"), 65536);
    const auto swaps = memoryfields::swaps(view(rawSwaps));
    Table swapDevices; swapDevices.title = tr("Active Swap Entries");
    swapDevices.headers = {tr("Device"), tr("Type"), tr("Capacity (KiB)"), tr("Used (KiB)")};
    swapDevices.note = tr("Kernel swap entries only. Canonical zram names are recognized; other paths are omitted for privacy. Aliases are not resolved and no device is opened.");
    QSet<QString> activeZramNames;
    if (swaps) for (const auto &entry : *swaps) {
        const auto path = QString::fromUtf8(entry.path.data(), qsizetype(entry.path.size()));
        const bool zramEntry = entry.type == "partition" && memoryfields::zramDevice(entry.path);
        if (zramEntry) activeZramNames.insert(path.mid(5));
        swapDevices.add({zramEntry ? path : tr("Swap entry %1").arg(swapDevices.rows.size()+1), zramEntry ? QStringLiteral("zram") : QString::fromLatin1(entry.type.data(), qsizetype(entry.type.size())), QString::number(qulonglong(entry.sizeKiB)), QString::number(qulonglong(entry.usedKiB))});
    }
    if (!swaps) r.limits << tr("Active swap entries are unavailable or incomplete.");
    else if (swaps->empty()) swapDevices.note += QLatin1Char('\n') + tr("No active swap entries in this snapshot.");
    // A configured zram device need not be active swap (it may be a filesystem).
    bool activeZram = false;
    for (const QString &z : ls(root, QStringLiteral("/sys/block"), {QStringLiteral("zram*")})) {
        const QString b = QStringLiteral("/sys/block/") + z + QLatin1Char('/');
        const auto sizeText = rd(root, b + QStringLiteral("disksize"), 64).toLatin1();
        const auto size = memoryfields::unsignedInt(view(sizeText));
        if (size && *size == 0 && !activeZramNames.contains(z)) continue;
        if (size && *size > 0 && activeZramNames.contains(z)) activeZram = true;
        const auto rawStats = snapshot(b + QStringLiteral("mm_stat"), 4096);
        const auto stats = memoryfields::zramStats(view(rawStats));
        const QString activity = activeZramNames.contains(z) ? tr("Active swap") : swaps ? tr("Not listed as canonical active swap") : tr("Swap activity unknown");
        const QString ratio = stats && stats->compressed > 0 ? QStringLiteral("%1:1").arg(double(stats->stored)/double(stats->compressed), 0, 'f', 1) : tr("unknown");
        const auto detail = tr("%1 · %2 · stored: %3 · compressed: %4 · resident RAM: %5 · ratio: %6")
            .arg(activity, known(chosen(rd(root, b + QStringLiteral("comp_algorithm")))), stats ? bytesText(stats->stored) : tr("unknown"),
                 stats ? bytesText(stats->compressed) : tr("unknown"), stats ? bytesText(stats->resident) : tr("unknown"), ratio);
        t.add({QStringLiteral("zram (%1)").arg(z), size ? bytesText(*size) : tr("unknown"), detail}, size && stats ? Level::Info : Level::Unknown);
    }
    if (!activeZram && !activeZramNames.isEmpty()) r.limits << tr("Canonical zram swap names could not be corroborated with a configured sysfs device.");
    const auto zswapState = rd(root, QStringLiteral("/sys/module/zswap/parameters/enabled"));
    const bool zswap = zswapState == QStringLiteral("Y");
    if (!zswapState.isEmpty())
        t.add({QStringLiteral("zswap"), zswap ? tr("on") : zswapState == QStringLiteral("N") ? tr("off") : tr("unknown"),
               zswap ? tr("%1, pool up to %2 % of RAM").arg(known(rd(root, QStringLiteral("/sys/module/zswap/parameters/compressor"))),
                                                           known(rd(root, QStringLiteral("/sys/module/zswap/parameters/max_pool_percent")))) : QString()},
              zswap && activeZram ? Level::Review : zswap || zswapState == QStringLiteral("N") ? Level::Info : Level::Unknown);
    if (zswap && activeZram)
        r.add(Level::Review, tr("zswap and zram are both on"),
              tr("zswap is enabled and a canonical zram swap entry is active. Pages evicted from zswap to zram may be compressed again; this snapshot does not prove actual double compression."),
              tr("Use one of them: turn zswap off (zswap.enabled=0) when the swap is zram."))
            .setFix(tr("Turn zswap off (until reboot)"), QJsonObject{{QStringLiteral("op"), QStringLiteral("sysfs_set")}, {QStringLiteral("key"), QStringLiteral("zswap")}, {QStringLiteral("value"), QStringLiteral("N")}});
    const QString swappiness = rd(root, QStringLiteral("/proc/sys/vm/swappiness"));
    if (!swappiness.isEmpty()) t.add({QStringLiteral("vm.swappiness"), swappiness, activeZram ? tr("with zram, 100–180 is common") : tr("default 60")});
    const QString thp = chosen(rd(root, QStringLiteral("/sys/kernel/mm/transparent_hugepage/enabled")));
    const QString defrag = chosen(rd(root, QStringLiteral("/sys/kernel/mm/transparent_hugepage/defrag")));
    if (!thp.isEmpty()) {
        t.add({tr("Transparent huge pages"), thp, tr("defrag: %1").arg(defrag)}, thp == QStringLiteral("always") && defrag == QStringLiteral("always") ? Level::Review : Level::Info);
        if (thp == QStringLiteral("always") && defrag == QStringLiteral("always"))
            r.add(Level::Info, tr("Huge pages: always, with synchronous defragmentation"),
                  tr("Allocations can stall while the kernel compacts memory (stutter in games). defrag=madvise or defer keeps the gain without the stalls."))
                .setFix(tr("Set defrag to madvise (until reboot)"), QJsonObject{{QStringLiteral("op"), QStringLiteral("sysfs_set")}, {QStringLiteral("key"), QStringLiteral("thp_defrag")}, {QStringLiteral("value"), QStringLiteral("madvise")}});
    }
    const QString lru = rd(root, QStringLiteral("/sys/kernel/mm/lru_gen/enabled"));
    if (!lru.isEmpty()) t.add({tr("Multi-gen LRU"), lru == QStringLiteral("0x0000") ? tr("off") : tr("on (%1)").arg(lru), QString()});
    // Pressure.
    const QString psi = rd(root, QStringLiteral("/proc/pressure/memory"));
    if (!psi.isEmpty()) {
        const auto avg = [&psi](const QString &kind, const QString &win) {
            return number(QRegularExpression(kind + QStringLiteral(R"(.*?)") + win + QStringLiteral(R"(=([\d.]+))")).match(psi).captured(1));
        };
        const auto some = avg(QStringLiteral("some"), QStringLiteral("avg60")), full = avg(QStringLiteral("full"), QStringLiteral("avg60"));
        if (!some || !full || *some < 0 || *full < 0 || *some > 100 || *full > 100) r.add(Level::Unknown, tr("Memory pressure reading is incomplete"));
        else {
        const double some60 = *some, full60 = *full;
        t.add({tr("Memory pressure (last minute)"), tr("some %1 % · full %2 %").arg(some60).arg(full60),
               tr("share of time tasks waited for memory")}, full60 > 5 || some60 > 20 ? Level::Review : Level::Ok);
        if (full60 > 5 || some60 > 20)
            r.add(Level::Review, tr("Programs are waiting for memory (%1 % of the last minute)").arg(std::max(some60, full60)),
                  tr("The kernel is reclaiming or swapping to keep up; the desktop feels sluggish."), tr("Free memory or add swap / zram."));
        }
    } else {
        r.add(Level::Unknown, tr("Memory pressure reading is incomplete"));
        r.limits << tr("Memory pressure (PSI) is not available: kernel without CONFIG_PSI or psi=0.");
    }
    r.tables << t;
    if (swaps) r.tables << swapDevices;
    // Topology.
    Table topo;
    topo.title = tr("NUMA and Cache Domains");
    topo.headers = {tr("Domain"), tr("CPUs")};
    for (const QString &n : ls(root, QStringLiteral("/sys/devices/system/node"), {QStringLiteral("node[0-9]*")}))
        topo.add({tr("NUMA %1").arg(n), rd(root, QStringLiteral("/sys/devices/system/node/") + n + QStringLiteral("/cpulist"))});
    QSet<QString> l3;
    for (const QString &c : ls(root, QStringLiteral("/sys/devices/system/cpu"), {QStringLiteral("cpu[0-9]*")}))
        for (const QString &ix : ls(root, QStringLiteral("/sys/devices/system/cpu/") + c + QStringLiteral("/cache"), {QStringLiteral("index*")})) {
            const QString b = QStringLiteral("/sys/devices/system/cpu/") + c + QStringLiteral("/cache/") + ix + QLatin1Char('/');
            const QString shared = rd(root, b + QStringLiteral("shared_cpu_list"));
            if (rd(root, b + QStringLiteral("level")) == QStringLiteral("3") && !shared.isEmpty())
                l3.insert(rd(root, b + QStringLiteral("size")) + QLatin1Char('|') + shared);
        }
    QStringList l3s = l3.values();
    std::sort(l3s.begin(), l3s.end());
    for (const QString &g : l3s) {
        const QString size = g.section(QLatin1Char('|'), 0, 0);  // "32768K"
        const int kib = size.endsWith(QLatin1Char('K')) ? size.chopped(1).toInt() : 0;
        topo.add({tr("L3 %1").arg(kib ? tr("%1 MB").arg(kib / 1024) : size), g.section(QLatin1Char('|'), 1)});
    }
    if (l3s.size() > 1) topo.note = tr("%1 reported L3 sharing domains; CCD identity is not inferred from cache sharing.").arg(l3s.size());
    r.tables << topo;
    const Table cpu = cpuTopology(root);
    if (!cpu.rows.isEmpty()) r.tables << cpu;
    Table caches; caches.title = tr("CPU Cache Sharing Domains");
    caches.headers = {tr("Level"), tr("Type"), tr("Size"), tr("Shared CPUs"), tr("Source")};
    QSet<QString> cacheKeys;
    for (const auto &id : ls(root, QStringLiteral("/sys/devices/system/cpu"), {QStringLiteral("cpu[0-9]*")})) {
        const QString base=QStringLiteral("/sys/devices/system/cpu/")+id+QStringLiteral("/cache/");
        for (const auto &index : ls(root,base,{QStringLiteral("index[0-9]*")})) {
            const auto path=base+index+QLatin1Char('/');
            const auto level=rd(root,path+QStringLiteral("level"),16), type=rd(root,path+QStringLiteral("type"),32), size=rd(root,path+QStringLiteral("size"),32);
            const auto list=rd(root,path+QStringLiteral("shared_cpu_list"),65536).toLatin1();
            const auto parsed=hardwarefields::cpuList(std::string_view(list.constData(),std::size_t(list.size())));
            QString shared; if(parsed) { QStringList ids; for(std::size_t i=0;i<parsed->size();) {auto first=(*parsed)[i],last=first; ++i; while(i<parsed->size()&&(*parsed)[i]==last+1)last=(*parsed)[i++]; ids<<(first==last?QString::number(first):QStringLiteral("%1-%2").arg(first).arg(last));} shared=ids.join(QLatin1Char(',')); }
            const QString key=level+QLatin1Char('/')+type+QLatin1Char('/')+size+QLatin1Char('/')+shared;
            if(!shared.isEmpty() && cacheKeys.contains(key))continue;
            if(!shared.isEmpty())cacheKeys.insert(key);
            caches.add({known(level),known(type),known(size),known(shared),id+QLatin1Char('/')+index}, shared.isEmpty()?Level::Unknown:Level::Info);
            if(caches.rows.size()>=4096)break;
        }
        if(caches.rows.size()>=4096)break;
    }
    caches.note=tr("Cache sharing is kernel-reported topology, not proof of physical CCD identity.");
    if(!caches.rows.isEmpty())r.tables<<caches;
    Table dimms; dimms.title=tr("SMBIOS Memory Devices");
    dimms.headers={tr("Entry"),tr("State"),tr("Capacity (MiB)"),tr("Rated transfer rate (MT/s)"),tr("Configured transfer rate (MT/s)")};
    for(const auto &id:ls(root,QStringLiteral("/sys/firmware/dmi/entries"),{QStringLiteral("17-*")})){
        if(dimms.rows.size()>=256)break;
        QFile f(root+QStringLiteral("/sys/firmware/dmi/entries/")+id+QStringLiteral("/raw"));
        if(!f.open(QIODevice::ReadOnly)){dimms.add({id,tr("Unreadable"),tr("unknown"),tr("unknown"),tr("unknown")},Level::Unknown);continue;}
        const auto raw=f.read(0x22); // formatted numeric fields only; never read serial or locator strings
        const auto parsed=hardwarefields::memoryDevice({reinterpret_cast<const std::uint8_t*>(raw.constData()),std::size_t(raw.size())});
        if(!parsed){dimms.add({id,tr("Invalid or incomplete record"),tr("unknown"),tr("unknown"),tr("unknown")},Level::Unknown);continue;}
        auto rate=[](const std::optional<unsigned>& value){return value?QString::number(*value):tr("unknown");};
        dimms.add({id,parsed->empty?tr("Empty slot"):parsed->bytes?tr("Reported device"):tr("unknown"),parsed->bytes?QString::number(qulonglong(*parsed->bytes/1048576)):tr("unknown"),rate(parsed->speed),rate(parsed->configured)},parsed->empty||parsed->bytes?Level::Info:Level::Unknown);
    }
    dimms.note=tr("Firmware-reported records, not measured RAM speed. Missing or inaccessible SMBIOS data is unknown; no privilege escalation.");
    if(!dimms.rows.isEmpty())r.tables<<dimms; else r.limits<<tr("SMBIOS memory device entries are unavailable.");
    Table nodes; nodes.title = tr("NUMA Memory and Distances");
    nodes.headers = {tr("Node"), tr("CPUs"), tr("Memory total (KiB)"), tr("Distance vector")};
    for (const auto &id : ls(root, QStringLiteral("/sys/devices/system/node"), {QStringLiteral("node[0-9]*")})) {
        const QString base = QStringLiteral("/sys/devices/system/node/") + id + QLatin1Char('/');
        const auto m = QRegularExpression(QStringLiteral(R"(MemTotal:\s+(\d+)\s+kB)")).match(rd(root, base + QStringLiteral("meminfo")));
        nodes.add({id, known(rd(root, base + QStringLiteral("cpulist"))), known(m.captured(1)), known(rd(root, base + QStringLiteral("distance")))});
    }
    if (!nodes.rows.isEmpty()) r.tables << nodes;
    if (!r.count(Level::Review) && !r.count(Level::Unknown)) r.add(Level::Ok, tr("Memory set up sensibly and not under pressure"));
    return r;
}

// ── S11 ACPI / WMI Firmware Browser ───────────────────────────────────────────

Report firmware(const QString &root) {
    Report r;
    r.code = QStringLiteral("S11");
    r.title = tr("ACPI & WMI Firmware");
    r.about = tr("The firmware interfaces the kernel found: WMI objects with the driver that claims each, firmware settings exposed through "
                 "firmware-attributes, and the ACPI tables present. Nothing is called or changed.");
    r.limits << tr("Passwords and authentication attributes are never read.");
    static const QHash<QString, const char *> knownGuids = {
        {"887B54E3-DDDC-4B2C-8B88-68A26A8835D0", QT_TRANSLATE_NOOP("systemdiagnostics", "Lenovo GameZone (profiles, GPU, thermal) — used by Centurion")},
        {"DC2A8805-3A8C-41BA-A6F7-092E0089CD3B", QT_TRANSLATE_NOOP("systemdiagnostics", "Lenovo Other Method (power limits) — used by Centurion")},
        {"92549549-4BDE-4F06-AC04-CE8BF898DBAA", QT_TRANSLATE_NOOP("systemdiagnostics", "Lenovo Fan Method (fan tables) — used by Centurion")},
        {"362A3AFE-3D96-4665-8530-96DAD5BB300E", QT_TRANSLATE_NOOP("systemdiagnostics", "Lenovo Capability Data 00 — used by Centurion")},
        {"7A8F5407-CB67-4D6E-B547-39B3BE018154", QT_TRANSLATE_NOOP("systemdiagnostics", "Lenovo Capability Data 01")}};
    Table w;
    w.title = tr("WMI Objects");
    w.headers = {tr("GUID"), tr("Driver"), tr("Known as"), tr("Instance")};
    int unbound = 0;
    for (const QString &id : ls(root, QStringLiteral("/sys/bus/wmi/devices"))) {
        const QString guid = id.section(QLatin1Char('-'), 0, 4).toUpper();
        const QString drv = driverOf(root, QStringLiteral("/sys/bus/wmi/devices/") + id);
        QString name = knownGuids.contains(guid) ? tr(knownGuids.value(guid)) : QString();
        // A GUID prefix alone does not identify a WMI method.
        if (drv.isEmpty()) ++unbound;
        const bool valid = QRegularExpression(QStringLiteral(R"(^[0-9A-F]{8}-(?:[0-9A-F]{4}-){3}[0-9A-F]{12}$)")).match(guid).hasMatch();
        w.add({valid ? guid : tr("unknown"), drv.isEmpty() ? tr("none") : drv, valid ? name : QString(), id}, valid ? Level::Info : Level::Unknown);
    }
    if (w.rows.isEmpty()) r.add(Level::Info, tr("No WMI object exported (no ACPI-WMI on this machine, or CONFIG_ACPI_WMI off)"));
    else {
        w.note = tr("%1 object(s) have no driver: normal for objects no Linux driver knows; Centurion talks to the Lenovo ones through ACPI directly.").arg(unbound);
        r.tables << w;
    }
    Table acpi; acpi.title = tr("Exposed ACPI / WMI Device Hierarchy");
    acpi.headers = {tr("Device"), tr("Bus"), tr("HID / GUID"), tr("UID"), tr("Status"), tr("Driver"), tr("Modalias")};
    acpi.note = tr("Only kernel-exposed device objects are shown, not the complete AML namespace. No method is executed.");
    struct AcpiEntry { QString bus, id, path, target; };
    QList<AcpiEntry> entries;
    for (const auto &bus : {QStringLiteral("acpi"), QStringLiteral("wmi")}) {
        const QString base = QStringLiteral("/sys/bus/") + bus + QStringLiteral("/devices/");
        for (const auto &id : ls(root, base)) entries << AcpiEntry{bus, id, base + id, devicePath(root, base + id)};
    }
    std::stable_sort(entries.begin(), entries.end(), [](const AcpiEntry &a, const AcpiEntry &b) { return a.target.size() < b.target.size(); });
    QHash<QString, int> seen;
    for (const auto &e : entries) {
        int parent = -1;
        if (!e.target.isEmpty()) {
            QString up = QFileInfo(e.target).path();
            for (int depth = 0; depth < 64 && up.size() > 1; ++depth) {
                if (seen.contains(up)) { parent = seen.value(up); break; }
                const QString next = QFileInfo(up).path(); if (next == up) break; up = next;
            }
        }
        auto val = [&](const QString &name) { return e.target.isEmpty() ? tr("unknown") : known(rd(root, e.path + QLatin1Char('/') + name, 512)); };
        acpi.parents << parent;
        acpi.add({e.id, e.bus, e.bus == QStringLiteral("wmi") ? e.id.section(QLatin1Char('-'), 0, 4) : val(QStringLiteral("hid")), val(QStringLiteral("uid")), val(QStringLiteral("status")),
                  e.target.isEmpty() ? tr("unknown") : known(driverOf(root, e.path)), val(QStringLiteral("modalias"))}, e.target.isEmpty() ? Level::Unknown : Level::Info);
        if (!e.target.isEmpty() && !seen.contains(e.target)) seen.insert(e.target, int(acpi.rows.size()) - 1);
    }
    if (!acpi.rows.isEmpty()) r.tables << acpi;
    else r.add(Level::Unknown, tr("No exposed ACPI or WMI device hierarchy could be read"));
    Table fa;
    fa.title = tr("Firmware Settings (firmware-attributes)");
    fa.headers = {tr("Provider"), tr("Setting"), tr("Value"), tr("Allowed")};
    for (const QString &prov : ls(root, QStringLiteral("/sys/class/firmware-attributes"))) {
        const QString base = QStringLiteral("/sys/class/firmware-attributes/") + prov + QStringLiteral("/attributes/");
        for (const QString &a : ls(root, base)) {
            if (a == QStringLiteral("pending_reboot")) {
                if (rd(root, base + a) == QStringLiteral("1"))
                    r.add(Level::Review, tr("A firmware setting waits for a reboot (%1)").arg(prov), tr("A changed BIOS / firmware value takes effect only after restarting."));
                continue;
            }
            const QString type = rd(root, base + a + QStringLiteral("/type"));
            const QString disp = rd(root, base + a + QStringLiteral("/display_name"), 200);
            // Values of integer and enumeration settings only; strings can hold asset tags.
            const QString val = type == QStringLiteral("integer") || type == QStringLiteral("enumeration") ? rd(root, base + a + QStringLiteral("/current_value"), 128) : tr("(not read)");
            const QString allowed = type == QStringLiteral("enumeration") ? rd(root, base + a + QStringLiteral("/possible_values"), 300)
                                  : type == QStringLiteral("integer") ? QStringLiteral("%1 … %2").arg(rd(root, base + a + QStringLiteral("/min_value")), rd(root, base + a + QStringLiteral("/max_value")))
                                                                      : type;
            fa.add({prov, disp.isEmpty() ? a : QStringLiteral("%1 (%2)").arg(disp, a), val, allowed});
        }
    }
    if (!fa.rows.isEmpty()) r.tables << fa;
    Table at; at.title = tr("ACPI Tables Present");
    at.headers = {tr("Table"),tr("Signature"),tr("Length (bytes)"),tr("Revision"),tr("Read status")};
    at.note=tr("Only fixed-size table headers are inspected. AML is not interpreted or executed; checksum verification is not claimed.");
    for(const auto &suffix:{QString(),QStringLiteral("/dynamic")}){
        const auto dir=QStringLiteral("/sys/firmware/acpi/tables")+suffix;
        for(const auto &name:ls(root,dir)){
            if(at.rows.size()>=256)break;
            const auto path=root+dir+QLatin1Char('/')+name;
            if(!QFileInfo(path).isFile() || QFileInfo(path).isSymLink())continue;
            QFile f(path); const auto label=suffix.isEmpty()?name:QStringLiteral("dynamic/")+name;
            if(!f.open(QIODevice::ReadOnly)){at.add({label,tr("unknown"),tr("unknown"),tr("unknown"),tr("Unreadable")},Level::Unknown);continue;}
            const auto raw=f.read(36);
            // FACS and RSDP have different header layouts; do not decode them as SDT headers.
            const auto h=(raw.startsWith("FACS")||raw.startsWith("RSD PTR "))?std::optional<hardwarefields::AcpiHeader>{}:hardwarefields::acpiHeader({reinterpret_cast<const std::uint8_t*>(raw.constData()),std::size_t(raw.size())});
            if(!h){at.add({label,tr("unknown"),tr("unknown"),tr("unknown"),tr("Unsupported or incomplete header")},Level::Unknown);continue;}
            at.add({label,QString::fromLatin1(raw.left(4)),QString::number(h->length),QString::number(h->revision),tr("Header read")});
        }
    }
    if(!at.rows.isEmpty())r.tables<<at; else r.limits<<tr("ACPI table listing is unavailable or inaccessible.");
    r.add(Level::Info, tr("%1 WMI object(s), %2 firmware setting(s)").arg(w.rows.size()).arg(fa.rows.size()));
    return r;
}

// ── S13 USB-C PD / Charger Inspector ──────────────────────────────────────────

Report charging(const QString &root) {
    Report r;
    r.code = QStringLiteral("S13");
    r.title = tr("USB-C Charger");
    r.about = tr("What the connected charger offers (its USB Power Delivery profiles), what the laptop is drawing, and the battery's condition.");
    r.limits << tr("USB-PD profiles depend on kernel and driver exposure; barrel and proprietary chargers may show only as an online AC adapter.")
             << tr("Battery power figures are the battery's own estimate.");
    // Type-C ports and their partners' source capabilities.
    Table pd;
    pd.title = tr("USB-C Ports");
    pd.viewKind = QStringLiteral("pd-ports");
    pd.note = tr("Negotiated contract is not exposed by these sources. Source profiles and supply measurements are separate.");
    pd.headers = {tr("Port"), tr("Role"), tr("Partner"), tr("Partner source capabilities"), tr("Best capacity")};
    Table profiles; profiles.title=tr("USB Power Delivery Source Profiles"); profiles.viewKind=QStringLiteral("pd-profiles");
    profiles.headers={tr("Port"),tr("Partner profile"),tr("Minimum voltage (V)"),tr("Maximum voltage (V)"),tr("Maximum current (A)"),tr("Capacity (W)")};
    profiles.note=tr("Source capability is not a negotiated contract or measured draw. No contract is inferred from voltage or current limits.");
    double bestW = 0;
    bool incompleteSink = false;
    // Compare the target inode as well as its path. A detach/reattach can
    // reuse the same sysfs name; the second sample must not inherit the old PDOs.
    auto identity=[](const QString &path,const QString &sysRoot)->std::optional<QPair<quint64,quint64>> {
        const auto canonical=QFileInfo(path).canonicalFilePath();
        if(sysRoot.isEmpty() || !canonical.startsWith(sysRoot+QLatin1Char('/'))) return std::nullopt;
        struct stat st{};
        const auto native=QFile::encodeName(canonical);
        if(::stat(native.constData(),&st)!=0 || !S_ISDIR(st.st_mode)) return std::nullopt;
        return QPair<quint64,quint64>{quint64(st.st_dev),quint64(st.st_ino)};
    };
    for (const QString &port : ls(root, QStringLiteral("/sys/class/typec"), {QStringLiteral("port[0-9]*")})) {
        if (!QRegularExpression(QStringLiteral(R"(^port[0-9]+$)")).match(port).hasMatch()) continue;
        const QString b = QStringLiteral("/sys/class/typec/") + port;
        const QString partner = b + QStringLiteral("-partner");
        const bool connected = QFile::exists(root + partner);
        QStringList pdos;
        double best = 0;
        const QString pdDir = QFileInfo(root + partner + QStringLiteral("/usb_power_delivery")).canonicalFilePath();
        const QString sysRoot=QFileInfo(root+QStringLiteral("/sys")).canonicalFilePath();
        const auto portBefore=identity(root+b,sysRoot);
        const auto partnerBefore=identity(root+partner,sysRoot);
        const auto pdBefore=identity(pdDir,sysRoot);
        const auto roleBefore=chosen(rd(root,b+QStringLiteral("/power_role")));
        const auto dataBefore=chosen(rd(root,b+QStringLiteral("/data_role")));
        const auto revisionBefore=partnerBefore ? rd(root,partner+QStringLiteral("/usb_power_delivery_revision")) : QString();
        const int profileStart=int(profiles.rows.size());
        bool completeProfiles=false;
        if (connected && partnerBefore && pdBefore) {
            const auto caps=diagsrc::dir(pdDir + QStringLiteral("/source-capabilities"),{QStringLiteral("*")},513);
            completeProfiles=!caps.isEmpty() && caps.size()<=512;
            for (const QString &cap : caps) {
                const QString resolved=QFileInfo(pdDir + QStringLiteral("/source-capabilities/") + cap).canonicalFilePath();
                if(!resolved.startsWith(sysRoot+QLatin1Char('/'))){completeProfiles=false;continue;}
                const QString c = resolved + QLatin1Char('/');
                if(profiles.rows.size()>=1024){completeProfiles=false;break;}
                auto milli=[&](const QString &name,std::string_view unit){ const auto text=diagsrc::read(c+name,64).toLatin1(); return hardwarefields::milli(std::string_view(text.constData(),std::size_t(text.size())),unit); };
                const auto i=milli(QStringLiteral("maximum_current"),"mA");
                const auto fixed=milli(QStringLiteral("voltage"),"mV");
                const auto low=fixed?fixed:milli(QStringLiteral("minimum_voltage"),"mV");
                const auto high=fixed?fixed:milli(QStringLiteral("maximum_voltage"),"mV");
                const bool valid=i&&low&&high&&*low<=*high;
                if(!valid)completeProfiles=false;
                auto show=[](const std::optional<double> &v){return v?QString::number(*v,'g',8):tr("unknown");};
                profiles.add({port,cap,show(low),show(high),show(i),valid?QString::number(*high * *i,'g',8):tr("unknown")},valid?Level::Info:Level::Unknown);
                if(valid){pdos<<QStringLiteral("%1–%2 V %3 A").arg(*low).arg(*high).arg(*i);best=std::max(best,*high * *i);}
            }
        }
        const auto roleAfter=chosen(rd(root,b+QStringLiteral("/power_role")));
        const auto dataAfter=chosen(rd(root,b+QStringLiteral("/data_role")));
        const auto partnerAfter=identity(root+partner,sysRoot);
        const auto pdAfterPath=QFileInfo(root+partner+QStringLiteral("/usb_power_delivery")).canonicalFilePath();
        const auto revisionAfter=partnerAfter ? rd(root,partner+QStringLiteral("/usb_power_delivery_revision")) : QString();
        const bool stable=portBefore && portBefore==identity(root+b,sysRoot)
            && connected==QFileInfo::exists(root+partner) && roleBefore==roleAfter && dataBefore==dataAfter
            && (!connected || partnerBefore.has_value()) && partnerBefore==partnerAfter
            && pdDir==pdAfterPath && pdBefore==identity(pdAfterPath,sysRoot) && revisionBefore==revisionAfter;
        if(!stable){while(profiles.rows.size()>profileStart){profiles.rows.removeLast();profiles.levels.removeLast();}pdos.clear();best=0;completeProfiles=false;}
        if(connected && (roleBefore==QStringLiteral("sink") || roleAfter==QStringLiteral("sink"))) {
            if(stable && completeProfiles)bestW=std::max(bestW,best);else incompleteSink=true;
        }
        pd.add({port, stable ? QStringLiteral("%1 / %2").arg(known(roleAfter),known(dataAfter)) : tr("unknown"),
                !stable ? tr("Changed during capture") : connected ? tr("connected (PD %1)").arg(known(revisionAfter)) : tr("nothing"),
                pdos.isEmpty() ? (connected ? tr("not reported") : QStringLiteral("—")) : pdos.join(QStringLiteral(", ")),
                best > 0 && completeProfiles ? watts(best) : connected && !completeProfiles ? tr("Maximum unknown (incomplete profiles)") : QStringLiteral("—")}, stable && (!connected || completeProfiles) ? Level::Info : Level::Unknown);
    }
    if (!pd.rows.isEmpty()) r.tables << pd;
    if (!profiles.rows.isEmpty())r.tables<<profiles;
    if (bestW > 0 && bestW < 65 && !incompleteSink)
        r.add(Level::Review, tr("The USB-C charger offers at most %1").arg(watts(bestW)),
              tr("Gaming laptops draw far more under load: on this charger the battery can drain while plugged in, and the firmware lowers power limits."),
              tr("Use the original adapter for gaming; a USB-C charger is fine for light use."));
    // Power supplies.
    Table ps;
    ps.title = tr("Power Supplies");
    ps.headers = {tr("Supply"), tr("Type"), tr("State"), tr("Measured power"), tr("Detail")};
    Table readings; readings.title = tr("Supply Measurements and Reported Limits"); readings.viewKind=QStringLiteral("pd-supplies");
    readings.headers = {tr("Supply"), tr("Voltage now (V)"), tr("Current now (A)"), tr("Voltage max (V)"), tr("Current max (A)"), tr("Measured power (W)")};
    readings.note = tr("Reported maxima are capabilities, not a negotiated USB-PD contract. Signed measurements are preserved; direction follows the driver convention.");
    for (const QString &s : ls(root, QStringLiteral("/sys/class/power_supply"))) {
        const QString b = QStringLiteral("/sys/class/power_supply/") + s + QLatin1Char('/');
        const QString type = rd(root, b + QStringLiteral("type"));
        if (type == QStringLiteral("Battery") && rd(root, b + QStringLiteral("scope")) == QStringLiteral("Device")) continue;
        const auto vNow = number(rd(root, b + QStringLiteral("voltage_now"))), iNow = number(rd(root, b + QStringLiteral("current_now")));
        const auto vMax = number(rd(root, b + QStringLiteral("voltage_max"))), iMax = number(rd(root, b + QStringLiteral("current_max")));
        const auto reportedPower = number(rd(root, b + QStringLiteral("power_now")));
        std::optional<double> measured;
        if (reportedPower) measured = *reportedPower / 1e6;
        else if (vNow && iNow && *vNow > 0 && std::isfinite((*vNow / 1e6) * (*iNow / 1e6))) measured = (*vNow / 1e6) * (*iNow / 1e6);
        auto scaled = [](const std::optional<double> &n) { return n ? QString::number(*n / 1e6, 'g', 8) : tr("unknown"); };
        readings.add({s, scaled(vNow), scaled(iNow), scaled(vMax), scaled(iMax), measured ? QString::number(*measured, 'g', 8) : tr("unknown")});
        if (type == QStringLiteral("Battery")) {
            const double full = rd(root, b + QStringLiteral("energy_full")).toDouble(), design = rd(root, b + QStringLiteral("energy_full_design")).toDouble();

            const QString status = rd(root, b + QStringLiteral("status"));
            const QString cycles = rd(root, b + QStringLiteral("cycle_count"));
            const QString end = rd(root, b + QStringLiteral("charge_control_end_threshold"));
            const double health = design > 0 ? full * 100 / design : 0;
            ps.add({s, tr("battery"), QStringLiteral("%1, %2 %").arg(status, rd(root, b + QStringLiteral("capacity"))), measured ? watts(*measured) : QStringLiteral("—"),
                    QStringList{health > 0 ? tr("health %1 % of design").arg(health, 0, 'f', 0) : QString(),
                                cycles.isEmpty() || cycles == QStringLiteral("0") ? QString() : tr("%1 cycles").arg(cycles),
                                end.isEmpty() ? QString() : tr("charges to %1 %").arg(end)}.filter(QRegularExpression(QStringLiteral("."))).join(QStringLiteral(" · "))},
                   health > 0 && health < 80 ? Level::Review : Level::Info);
            if (health > 0 && health < 80)
                r.add(Level::Review, tr("Battery holds %1 % of its design capacity").arg(health, 0, 'f', 0),
                      tr("Below 80 % most makers consider a battery worn."), tr("Health → Battery use shows how fast it drains."));
            if (status == QStringLiteral("Not charging") && !end.isEmpty())
                r.add(Level::Info, tr("Battery not charging: charge limit %1 %").arg(end), tr("Charging stops at the limit to slow battery wear (conservation mode)."));
        } else if (type == QStringLiteral("Mains") || type.startsWith(QStringLiteral("USB"))) {
            const QString online = rd(root, b + QStringLiteral("online"));
            ps.add({s, type, online == QStringLiteral("1") ? tr("online") : online == QStringLiteral("0") ? tr("offline") : tr("unknown"),
                    online == QStringLiteral("1") && measured ? watts(*measured) : QStringLiteral("—"), chosen(rd(root, b + QStringLiteral("usb_type")))});
        }
    }
    r.tables << ps;
    if (!readings.rows.isEmpty()) r.tables << readings;
    bool acOnline = false, discharging = false;
    for (const QStringList &row : ps.rows) {
        if (row.value(1) != tr("battery") && row.value(2) == tr("online")) acOnline = true;
        if (row.value(1) == tr("battery") && row.value(2).startsWith(QStringLiteral("Discharging"))) discharging = true;
    }
    if (acOnline && discharging)
        r.add(Level::Review, tr("Plugged in, yet the battery is discharging"), tr("The load is above what the adapter supplies (or the adapter is not recognised)."),
              tr("Use a stronger adapter, or lower the load; check that the barrel plug is fully seated."));
    if (ps.rows.isEmpty() && pd.rows.isEmpty()) r.add(Level::Unknown, tr("No battery, charger or USB-C port is reported by the kernel"));
    else if (!r.count(Level::Review) && !r.count(Level::Attention)) r.add(Level::Info, tr("Charging information is limited to reported readings"));
    return r;
}

// ── S15 PipeWire Audio Doctor ─────────────────────────────────────────────────

Report audio(const QHash<QString, QString> &out, const QString &root) {
    Report r;
    r.code = QStringLiteral("S15");
    r.title = tr("Audio (PipeWire)");
    r.about = tr("Sound cards and codecs, which sound servers are running (and whether two fight over the devices), and PipeWire's own view.");
    r.limits << tr("Only your own user's sound servers are seen; nothing is restarted or re-routed.");
    Table c;
    c.title = tr("Sound Cards");
    c.headers = {tr("Card"), tr("Name"), tr("Codec")};
    const QString cards = rd(root, QStringLiteral("/proc/asound/cards"));
    static const QRegularExpression cardRe(QStringLiteral(R"(^\s*(\d+)\s+\[(\S+)\s*\]:\s*(.*)$)"), QRegularExpression::MultilineOption);
    for (auto it = cardRe.globalMatch(cards); it.hasNext();) {
        const auto m = it.next();
        QStringList codecs;
        for (const QString &cf : ls(root, QStringLiteral("/proc/asound/card%1").arg(m.captured(1)), {QStringLiteral("codec#*")}))
            codecs << rd(root, QStringLiteral("/proc/asound/card%1/%2").arg(m.captured(1), cf), 300).section(QLatin1Char('\n'), 0, 0).remove(QStringLiteral("Codec: "));
        c.add({m.captured(1) + QStringLiteral(" · ") + m.captured(2), m.captured(3).trimmed(), codecs.join(QStringLiteral(", "))});
    }
    if (c.rows.isEmpty()) r.add(Level::Attention, tr("No sound card registered"), tr("The audio driver did not load (S18 lists devices without a driver; SOF laptops also need their firmware)."));
    r.tables << c;
    QSet<QString> running;
    for (const QString &pid : ls(root, QStringLiteral("/proc"), {QStringLiteral("[0-9]*")})) {
        if (root.isEmpty() && QFileInfo(QStringLiteral("/proc/") + pid).ownerId() != ::getuid()) continue;  // another user's session
        const QString comm = rd(root, QStringLiteral("/proc/") + pid + QStringLiteral("/comm"), 64);
        if (QStringList{QStringLiteral("pipewire"), QStringLiteral("pipewire-pulse"), QStringLiteral("wireplumber"), QStringLiteral("pulseaudio"),
                        QStringLiteral("pipewire-media-session"), QStringLiteral("jackd")}.contains(comm))
            running.insert(comm);
    }
    Table s;
    s.title = tr("Sound Servers Running");
    s.headers = {tr("Process")};
    QStringList names = running.values();
    names.sort();
    for (const QString &n : names) s.add({n});
    r.tables << s;
    if (running.contains(QStringLiteral("pulseaudio")) && running.contains(QStringLiteral("pipewire-pulse")))
        r.add(Level::Attention, tr("PulseAudio and pipewire-pulse are both running"), tr("Both try to own the sound devices: missing devices, no sound, or sound on one app only."),
              tr("Keep one: with PipeWire, disable PulseAudio's autospawn (autospawn = no) and remove it from the session start."));
    if (running.contains(QStringLiteral("pipewire")) && !running.contains(QStringLiteral("wireplumber")) && !running.contains(QStringLiteral("pipewire-media-session")))
        r.add(Level::Attention, tr("PipeWire runs without a session manager"), tr("Without WirePlumber nothing connects devices to programs: there is no sound."),
              tr("Start wireplumber with the session (Gentoo: media-video/wireplumber, started by the desktop or gentoo-pipewire-launcher)."))
            .setFix(tr("Restart PipeWire and WirePlumber"), QJsonObject{{QStringLiteral("op"), QStringLiteral("pipewire_restart")}});
    if (running.isEmpty()) r.add(Level::Review, tr("No sound server is running for this user"), tr("Programs that use PulseAudio or PipeWire get no sound."));
    const QString ps = rd(root, QStringLiteral("/sys/module/snd_hda_intel/parameters/power_save"));
    if (!ps.isEmpty() && ps != QStringLiteral("0"))
        r.add(Level::Info, tr("HDA codec power saving after %1 s of silence").arg(ps), tr("If sound starts with a pop or its first moment is cut off, this is why (snd_hda_intel power_save=0 stops it at a small power cost)."))
            .setFix(tr("Turn codec power saving off (until reboot)"), QJsonObject{{QStringLiteral("op"), QStringLiteral("sysfs_set")}, {QStringLiteral("key"), QStringLiteral("hda_power_save")}, {QStringLiteral("value"), QStringLiteral("0")}});
    // pactl info: which server answers, default devices.
    const QString pactl = out.value(QStringLiteral("pactl info"));
    if (!pactl.isEmpty()) {
        Table p;
        p.title = tr("Server");
        p.headers = {tr("Item"), tr("Value")};
        for (const QString &l : pactl.split(QLatin1Char('\n'))) {
            const QString k = l.section(QLatin1Char(':'), 0, 0).trimmed();
            if (QStringList{QStringLiteral("Server Name"), QStringLiteral("Server Version"), QStringLiteral("Default Sink"), QStringLiteral("Default Source"), QStringLiteral("Default Sample Specification")}.contains(k))
                p.add({k, l.section(QLatin1Char(':'), 1).trimmed()});
            if (k == QStringLiteral("Default Sink") && l.contains(QStringLiteral("auto_null")))
                r.add(Level::Attention, tr("The default output is the dummy output"), tr("No real sound device is available to the server."), tr("Check the cards above and the session manager."));
        }
        r.tables << p;
    }
    r.raw = QStringLiteral("$ wpctl status\n") + out.value(QStringLiteral("wpctl status"), tr("(not installed or no answer)")) +
            QStringLiteral("\n$ pactl info\n") + (pactl.isEmpty() ? tr("(not installed or no answer)") : pactl);
    if (!r.count(Level::Attention) && !r.count(Level::Review)) r.add(Level::Ok, tr("Sound cards present and one sound server stack running"));
    return r;
}

// ── S18 Kernel Feature & Firmware Dependency Check ────────────────────────────

QString kernelConfig() {
    const QString rel = diagsrc::read(QStringLiteral("/proc/sys/kernel/osrelease"));
    QString blob = diagsrc::read(QStringLiteral("/boot/config-") + rel, 4 << 20);
    if (!blob.isEmpty() || !QFile::exists(QStringLiteral("/proc/config.gz"))) return blob;
    const QString zcat = QStandardPaths::findExecutable(QStringLiteral("zcat"));
    if (zcat.isEmpty()) return {};
    QProcess p;
    p.start(zcat, {QStringLiteral("/proc/config.gz")});
    if (!p.waitForStarted(1000)) return {};
    QByteArray out;
    QElapsedTimer t;
    t.start();
    while (p.state() != QProcess::NotRunning && t.elapsed() < 3000 && out.size() < (4 << 20)) {
        p.waitForReadyRead(100);
        out += p.readAllStandardOutput();  // drained while it runs: a full pipe would block zcat
    }
    if (p.state() != QProcess::NotRunning) { p.kill(); p.waitForFinished(500); return {}; }
    out += p.readAllStandardOutput();
    return p.exitCode() == 0 ? QString::fromUtf8(out) : QString();
}

QString configValue(const QString &config, const QString &option) {
    const auto m = QRegularExpression(QStringLiteral("(?:^|\\n)") + QRegularExpression::escape(option) + QStringLiteral(R"(=(\S+))")).match(config);
    if (m.hasMatch()) return m.captured(1);
    return config.contains(QStringLiteral("# ") + option + QStringLiteral(" is not set")) ? QStringLiteral("n") : QString();
}

Report kernelDeps(const QString &config, const QList<diagsrc::LogLine> &kernel, const QString &root) {
    Report r;
    r.code = QStringLiteral("S18");
    r.title = tr("Kernel Features & Firmware");
    r.about = tr("Kernel options Centurion's features rely on, firmware files the kernel could not load, the firmware compression the kernel can read, "
                 "and devices no driver has claimed.");
    r.limits << tr("A built-in option (y) or a module (m) both work; a module must also be installed for this kernel.");
    // Options → the feature that needs them.
    struct Need { const char *opt; const char *feature; bool important; };
    static const Need needs[] = {
        {"CONFIG_PCIEAER", QT_TRANSLATE_NOOP("systemdiagnostics", "PCIe error counters (Evidence Lab H03, PCIe Link Lab)"), true},
        {"CONFIG_PSTORE", QT_TRANSLATE_NOOP("systemdiagnostics", "crash records kept across a reboot (Boot history)"), true},
        {"CONFIG_EFI_VARS_PSTORE", QT_TRANSLATE_NOOP("systemdiagnostics", "pstore storage in EFI variables"), false},
        {"CONFIG_EDAC_DECODE_MCE", QT_TRANSLATE_NOOP("systemdiagnostics", "readable machine-check decoding (H02, CO Search crash watcher)"), true},
        {"CONFIG_X86_MCE_AMD", QT_TRANSLATE_NOOP("systemdiagnostics", "AMD machine-check threshold counters (CO Search crash watcher)"), true},
        {"CONFIG_AMD_PMC", QT_TRANSLATE_NOOP("systemdiagnostics", "hardware sleep residency on AMD (Sleep & wake, H12)"), false},
        {"CONFIG_PM_SLEEP", QT_TRANSLATE_NOOP("systemdiagnostics", "suspend and its counters (H12)"), true},
        {"CONFIG_PSI", QT_TRANSLATE_NOOP("systemdiagnostics", "memory pressure (S10)"), false},
        {"CONFIG_ACPI_PLATFORM_PROFILE", QT_TRANSLATE_NOOP("systemdiagnostics", "platform power profiles"), true},
        {"CONFIG_ACPI_WMI", QT_TRANSLATE_NOOP("systemdiagnostics", "Lenovo WMI controls (fans, power limits, battery info)"), true},
        {"CONFIG_X86_AMD_PSTATE", QT_TRANSLATE_NOOP("systemdiagnostics", "amd-pstate CPU frequency driver"), false},
        {"CONFIG_INTEL_RAPL", QT_TRANSLATE_NOOP("systemdiagnostics", "package power readings (flight recorder, Power Limit map)"), false},
        {"CONFIG_NVME_HWMON", QT_TRANSLATE_NOOP("systemdiagnostics", "NVMe temperatures in hwmon"), false},
        {"CONFIG_TYPEC_UCSI", QT_TRANSLATE_NOOP("systemdiagnostics", "USB-C PD details (S13)"), false},
        {"CONFIG_NFT_QUEUE", QT_TRANSLATE_NOOP("systemdiagnostics", "Network protection's per-program guard (nftables queue)"), true},
        {"CONFIG_NETFILTER_NETLINK_QUEUE", QT_TRANSLATE_NOOP("systemdiagnostics", "Network protection's per-program guard (netlink queue)"), true},
        {"CONFIG_ZRAM", QT_TRANSLATE_NOOP("systemdiagnostics", "compressed swap in RAM"), false},
        {"CONFIG_F2FS_FS", QT_TRANSLATE_NOOP("systemdiagnostics", "F2FS file systems (H05)"), false}};
    Table t;
    t.title = tr("Kernel Options");
    t.headers = {tr("Option"), tr("Value"), tr("Needed for")};
    if (config.isEmpty()) {
        r.add(Level::Unknown, tr("The running kernel's configuration is not readable"),
              tr("Neither /boot/config-%1 nor /proc/config.gz (CONFIG_IKCONFIG_PROC) is available, so no option is judged.").arg(rd(root, QStringLiteral("/proc/sys/kernel/osrelease"))),
              tr("Gentoo: keep the config in /boot (make install does), or enable CONFIG_IKCONFIG_PROC."));
    } else {
        QStringList absent;
        for (const Need &n : needs) {
            const QString v = configValue(config, QLatin1String(n.opt));
            const bool missing = v == QStringLiteral("n");
            if (v.isEmpty() && n.important) absent << QLatin1String(n.opt);
            t.add({QLatin1String(n.opt), v.isEmpty() ? tr("not in config") : v, tr(n.feature)},
                  missing ? (n.important ? Level::Review : Level::Info) : v.isEmpty() ? Level::Info : Level::Ok);
            if (missing && n.important)
                r.add(Level::Review, tr("%1 is off: %2 cannot work").arg(QLatin1String(n.opt), tr(n.feature)), QString(), tr("Enable it in the kernel configuration and rebuild."));
        }
        if (!absent.isEmpty())
            r.add(Level::Info, tr("Not in this kernel's configuration: %1").arg(absent.join(QStringLiteral(", "))),
                  tr("An option missing from the config is usually unavailable because an option it depends on is off; a few have other names in some kernel versions."));
        r.tables << t;
    }
    // Firmware compression the kernel can read.
    int zst = 0, xz = 0;
    QDirIterator it(root + QStringLiteral("/lib/firmware"), QDir::Files, QDirIterator::Subdirectories);
    for (int seen = 0; it.hasNext() && seen < 4000; ++seen) {
        const QString f = it.next();
        zst += f.endsWith(QStringLiteral(".zst"));
        xz += f.endsWith(QStringLiteral(".xz"));
    }
    if (!config.isEmpty()) {
        if (zst && configValue(config, QStringLiteral("CONFIG_FW_LOADER_COMPRESS_ZSTD")) != QStringLiteral("y"))
            r.add(Level::Attention, tr("Firmware files are zstd-compressed but the kernel cannot read them"),
                  tr("%1+ .zst files in /lib/firmware and CONFIG_FW_LOADER_COMPRESS_ZSTD is not set: every driver that needs one of them fails to load its firmware.").arg(zst),
                  tr("Enable CONFIG_FW_LOADER_COMPRESS_ZSTD, or install linux-firmware without compression (Gentoo: USE=-compress-zstd)."));
        if (xz && configValue(config, QStringLiteral("CONFIG_FW_LOADER_COMPRESS_XZ")) != QStringLiteral("y"))
            r.add(Level::Attention, tr("Firmware files are xz-compressed but the kernel cannot read them"),
                  tr("%1+ .xz files and CONFIG_FW_LOADER_COMPRESS_XZ is not set.").arg(xz), tr("Enable CONFIG_FW_LOADER_COMPRESS_XZ, or install the firmware uncompressed."));
    }
    // Firmware the kernel asked for and did not get.
    static const QRegularExpression direct(QStringLiteral(R"(Direct firmware load for (\S+) failed with error (-?\d+))"));
    QMap<QString, QPair<QString, QString>> miss;
    for (const diagsrc::LogLine &l : kernel)
        if (const auto m = direct.match(l.text); m.hasMatch()) miss.insert(m.captured(1), {l.text.section(QLatin1Char(' '), 0, 1), l.boot});
    if (!miss.isEmpty()) {
        Table f;
        f.title = tr("Firmware Files the Kernel Could Not Load");
        f.headers = {tr("File"), tr("Requested by"), tr("Last boot"), tr("On disk now")};
        int still = 0;
        for (auto m = miss.cbegin(); m != miss.cend(); ++m) {
            const QString found = firmwarePresent(root, m.key());
            if (found.isEmpty()) ++still;
            f.add({m.key(), m.value().first, m.value().second, found.isEmpty() ? tr("missing") : found}, found.isEmpty() ? Level::Review : Level::Info);
        }
        f.note = tr("Drivers often try newer or optional files first and fall back without a word; a missing file matters when its device misbehaves.");
        r.tables << f;
        if (still) r.add(Level::Review, tr("%1 firmware file(s) requested by drivers are not installed").arg(still), tr("See the table: the requesting driver says which device needs it."));
    }
    // PCI devices no driver has claimed (bridges and host parts excepted).
    Table u;
    u.title = tr("Devices Without a Driver");
    u.headers = {tr("Device"), tr("Class"), tr("Kernel would match")};
    for (const QString &id : ls(root, QStringLiteral("/sys/bus/pci/devices"))) {
        const QString b = QStringLiteral("/sys/bus/pci/devices/") + id;
        if (!driverOf(root, b).isEmpty()) continue;
        const QString cls = rd(root, b + QStringLiteral("/class"));
        if (cls.startsWith(QStringLiteral("0x0600")) || cls.startsWith(QStringLiteral("0x0601")) || cls.startsWith(QStringLiteral("0x0604")) ||
            cls.startsWith(QStringLiteral("0x1300")) || cls.startsWith(QStringLiteral("0x0880")) || cls.startsWith(QStringLiteral("0x0500")))
            continue;  // host / ISA / PCI bridges, instrumentation, system peripherals: usually driverless
        u.add({QStringLiteral("%1 · %2").arg(id, diagsrc::pciName(rd(root, b + QStringLiteral("/vendor")), rd(root, b + QStringLiteral("/device")), cls)),
               diagsrc::pciClassName(cls), rd(root, b + QStringLiteral("/modalias"), 200)}, Level::Review);
    }
    if (!u.rows.isEmpty()) {
        u.note = tr("The modalias is what a module must declare to claim the device: `modprobe --resolve-alias <modalias>` names one if it is installed.");
        r.tables << u;
        r.add(Level::Review, tr("%1 PCI device(s) have no driver").arg(u.rows.size()), tr("They do not work until a driver claims them (or they are deliberately unused)."));
    }
    if (!r.count(Level::Attention) && !r.count(Level::Review) && !config.isEmpty())
        r.add(Level::Ok, tr("Every option Centurion relies on is enabled, no firmware is missing, every device has a driver"));
    return r;
}

Report byCode(const QString &code) {
    if (code == QStringLiteral("S04")) return pcieLinks();
    if (code == QStringLiteral("S07")) return radios(diagsrc::savedKernel());
    if (code == QStringLiteral("S08")) return powerLimits();
    if (code == QStringLiteral("S10")) return memory();
    if (code == QStringLiteral("S11")) return firmware();
    if (code == QStringLiteral("S13")) return charging();
    if (code == QStringLiteral("S18")) return kernelDeps(kernelConfig(), diagsrc::savedKernel());
    Report r;
    r.code = code;
    r.add(Level::Unknown, tr("Unknown diagnostic %1").arg(code));
    return r;
}

}  // namespace systemdiagnostics
