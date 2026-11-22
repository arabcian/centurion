#include "systemdiagnostics.h"

#include "advanceddiagnostics.h"

#include <QCoreApplication>
#include <QDirIterator>
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

double linkSpeed(const QString &s) { return QRegularExpression(QStringLiteral(R"(([\d.]+)\s*GT/s)")).match(s).captured(1).toDouble(); }
static int pcieGen(double gts) { return gts >= 60 ? 6 : gts >= 30 ? 5 : gts >= 15 ? 4 : gts >= 7 ? 3 : gts >= 4 ? 2 : gts > 0 ? 1 : 0; }

// ── S04 PCIe Link Negotiation Lab ─────────────────────────────────────────────

Report pcieLinks(const QString &root) {
    Report r;
    r.code = QStringLiteral("S04");
    r.title = tr("PCIe Link Negotiation Lab");
    r.about = tr("The PCIe link every device trained at, against what both ends of the link can do. A narrower link than possible is a "
                 "hardware or BIOS limit; a slower one at idle is normal power saving.");
    r.limits << tr("Speed drops to save power when a device is idle (GPUs go to PCIe 1.0 at the desktop); check under load before judging the speed.")
             << tr("A device that is runtime-suspended shows the last link it had.");
    Table t;
    t.title = tr("Device links");
    t.headers = {tr("Device"), tr("Driver"), tr("Link now"), tr("Possible"), tr("Power"), tr("AER (corr / non-fatal / fatal)"), tr("Status")};
    int bridges = 0;
    for (const QString &id : ls(root, QStringLiteral("/sys/bus/pci/devices"))) {
        const QString base = QStringLiteral("/sys/bus/pci/devices/") + id;
        const QString cur = rd(root, base + QStringLiteral("/current_link_speed")), max = rd(root, base + QStringLiteral("/max_link_speed"));
        if (cur.isEmpty() || cur.startsWith(QStringLiteral("Unknown"))) continue;
        const QString cls = rd(root, base + QStringLiteral("/class"));
        if (cls.startsWith(QStringLiteral("0x0604"))) { ++bridges; continue; }  // ports: their downstream device is listed
        const int w = rd(root, base + QStringLiteral("/current_link_width")).toInt(), mw = rd(root, base + QStringLiteral("/max_link_width")).toInt();
        // The link can only be as fast / wide as the port above it.
        const QString up = QFileInfo(QFileInfo(root + base).canonicalFilePath()).path();
        const double portMax = linkSpeed(diagsrc::read(up + QStringLiteral("/max_link_speed")));
        const int portW = diagsrc::read(up + QStringLiteral("/max_link_width")).toInt();
        const double s = linkSpeed(cur), ms = std::min(linkSpeed(max), portMax > 0 ? portMax : 1e9);
        const int mw2 = portW > 0 ? std::min(mw, portW) : mw;
        const QString power = rd(root, base + QStringLiteral("/power/runtime_status"));
        static const std::pair<const char *, const char *> aerFiles[] = {
            {"aer_dev_correctable", "TOTAL_ERR_COR"}, {"aer_dev_nonfatal", "TOTAL_ERR_NONFATAL"}, {"aer_dev_fatal", "TOTAL_ERR_FATAL"}};
        QStringList aer;
        for (const auto &[f, k] : aerFiles) aer << advanceddiagnostics::aerTotal(rd(root, base + QLatin1Char('/') + QLatin1String(f)), QLatin1String(k));
        const bool hasAer = std::any_of(aer.begin(), aer.end(), [](const QString &x) { return !x.isEmpty(); });
        const QString name = diagsrc::pciName(rd(root, base + QStringLiteral("/vendor")), rd(root, base + QStringLiteral("/device")), cls);
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
        if (aer.size() == 3 && (aer[1].toLongLong() > 0 || aer[2].toLongLong() > 0)) l = Level::Attention;
        t.add({QStringLiteral("%1 · %2").arg(id, name), driverOf(root, base), QStringLiteral("PCIe %1 x%2").arg(pcieGen(s)).arg(w),
               QStringLiteral("PCIe %1 x%2").arg(pcieGen(ms)).arg(mw2), power.isEmpty() ? QStringLiteral("—") : power,
               hasAer ? aer.join(QStringLiteral(" / ")) : QStringLiteral("—"), status}, l);
    }
    t.note = tr("%1 PCIe port(s) / bridge(s) not listed. Possible = the lower of the device's and its port's maximum.").arg(bridges);
    r.tables << t;
    if (t.rows.isEmpty()) r.add(Level::Unknown, tr("No device reports a PCIe link"));
    else if (!r.count(Level::Review)) r.add(Level::Ok, tr("Every device trained at its full link width"),
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
    r.title = tr("Wi-Fi / Bluetooth Firmware Doctor");
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
    k.title = tr("Radio switches (rfkill)");
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
        f.title = tr("Firmware the kernel could not load (saved kernel log)");
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
    r.title = tr("Power Limit Source Map");
    r.about = tr("Every place a CPU / platform power limit is visible without waking anything: firmware power settings (PPT), the platform "
                 "profile, cpufreq caps, RAPL, and the power source.");
    r.limits << tr("The SMU's live limits (STAPM, fast / slow PPT in effect) are not exported by the kernel; the firmware settings here are the targets it was given.")
             << tr("The NVIDIA GPU's power limit needs nvidia-smi, which wakes the GPU: see the NVIDIA tab.");
    Table t;
    t.title = tr("Limits and their sources");
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
            t.add({tr("Firmware (%1)").arg(prov), disp.isEmpty() ? a : QStringLiteral("%1 (%2)").arg(disp, a), v.isEmpty() ? tr("unreadable") : v + QStringLiteral(" W"),
                   mn.isEmpty() ? QString() : QStringLiteral("%1 … %2 W").arg(mn, mx)});
        }
    }
    // cpufreq: a scaling cap below the hardware maximum.
    const qint64 hwMax = rd(root, QStringLiteral("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq")).toLongLong();
    qint64 capMin = 0;
    int capped = 0, cpus = 0;
    for (const QString &c : ls(root, QStringLiteral("/sys/devices/system/cpu/cpufreq"), {QStringLiteral("policy*")})) {
        const qint64 m = rd(root, QStringLiteral("/sys/devices/system/cpu/cpufreq/") + c + QStringLiteral("/scaling_max_freq")).toLongLong();
        const qint64 hm = rd(root, QStringLiteral("/sys/devices/system/cpu/cpufreq/") + c + QStringLiteral("/cpuinfo_max_freq")).toLongLong();
        ++cpus;
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
    if (!boost.isEmpty()) {
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
                   watts(lim.toDouble() / 1e6), rd(root, b + QStringLiteral("enabled")) == QStringLiteral("1") ? tr("enabled") : tr("not enforced")});
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
    if (!r.count(Level::Review) && !r.count(Level::Attention)) r.add(Level::Ok, tr("No unexpected cap on CPU clocks or boost"));
    return r;
}

// ── S10 Memory Architecture Explorer ──────────────────────────────────────────

Report memory(const QString &root) {
    Report r;
    r.code = QStringLiteral("S10");
    r.title = tr("Memory Architecture Explorer");
    r.about = tr("How RAM, swap, compressed swap (zram / zswap), huge pages and memory pressure are set up and behaving right now.");
    r.limits << tr("Pressure (PSI) is an average over the last seconds / minutes, not since boot.")
             << tr("DIMM details (speed, timings, part numbers) need root: System tools → DMI.");
    QHash<QString, double> mi;
    for (const QString &l : rd(root, QStringLiteral("/proc/meminfo")).split(QLatin1Char('\n'))) {
        const auto m = QRegularExpression(QStringLiteral(R"(^(\w+):\s+(\d+))")).match(l);
        if (m.hasMatch()) mi.insert(m.captured(1), m.captured(2).toDouble() / 1024 / 1024);  // GiB
    }
    auto gib = [](double g) { return QStringLiteral("%1 GiB").arg(g, 0, 'f', g < 10 ? 1 : 0); };
    Table t;
    t.title = tr("Memory now");
    t.headers = {tr("Item"), tr("Value"), tr("Note")};
    const double total = mi.value(QStringLiteral("MemTotal")), avail = mi.value(QStringLiteral("MemAvailable"));
    const double swapT = mi.value(QStringLiteral("SwapTotal")), swapF = mi.value(QStringLiteral("SwapFree"));
    if (total > 0) {
        t.add({tr("RAM"), gib(total), tr("%1 available (%2 %)").arg(gib(avail)).arg(avail * 100 / total, 0, 'f', 0)}, avail / total < 0.1 ? Level::Review : Level::Info);
        if (avail / total < 0.1) r.add(Level::Review, tr("Less than 10 % of RAM is available"), tr("The kernel is close to reclaiming hard or swapping."), tr("Close something large or add swap."));
    }
    t.add({tr("Swap"), swapT > 0 ? tr("%1 of %2 used").arg(gib(swapT - swapF), gib(swapT)) : tr("none"), QString()});
    // zram and zswap.
    bool zram = false;
    for (const QString &z : ls(root, QStringLiteral("/sys/block"), {QStringLiteral("zram*")})) {
        const QString b = QStringLiteral("/sys/block/") + z + QLatin1Char('/');
        const qint64 size = rd(root, b + QStringLiteral("disksize")).toLongLong();
        if (size <= 0) continue;
        zram = true;
        const QStringList mm = rd(root, b + QStringLiteral("mm_stat")).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const double orig = mm.value(0).toDouble(), compr = mm.value(1).toDouble();
        t.add({QStringLiteral("zram (%1)").arg(z), gib(size / 1073741824.0),
               tr("%1 · %2 stored, ratio %3:1").arg(chosen(rd(root, b + QStringLiteral("comp_algorithm"))), gib(orig / 1073741824.0))
                   .arg(compr > 0 ? orig / compr : 0, 0, 'f', 1)});
    }
    const bool zswap = rd(root, QStringLiteral("/sys/module/zswap/parameters/enabled")) == QStringLiteral("Y");
    if (!rd(root, QStringLiteral("/sys/module/zswap/parameters/enabled")).isEmpty())
        t.add({QStringLiteral("zswap"), zswap ? tr("on") : tr("off"),
               zswap ? tr("%1, pool up to %2 % of RAM").arg(rd(root, QStringLiteral("/sys/module/zswap/parameters/compressor")),
                                                           rd(root, QStringLiteral("/sys/module/zswap/parameters/max_pool_percent"))) : QString()},
              zswap && zram ? Level::Review : Level::Info);
    if (zswap && zram)
        r.add(Level::Review, tr("zswap and zram are both on"),
              tr("zswap compresses pages on their way to swap, and the swap is itself compressed by zram: pages are compressed twice and zswap's pool takes RAM that zram would use."),
              tr("Use one of them: turn zswap off (zswap.enabled=0) when the swap is zram."))
            .setFix(tr("Turn zswap off (until reboot)"), QJsonObject{{QStringLiteral("op"), QStringLiteral("sysfs_set")}, {QStringLiteral("key"), QStringLiteral("zswap")}, {QStringLiteral("value"), QStringLiteral("N")}});
    const QString swappiness = rd(root, QStringLiteral("/proc/sys/vm/swappiness"));
    if (!swappiness.isEmpty()) t.add({QStringLiteral("vm.swappiness"), swappiness, zram ? tr("with zram, 100–180 is common") : tr("default 60")});
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
            return QRegularExpression(kind + QStringLiteral(R"(.*?)") + win + QStringLiteral(R"(=([\d.]+))")).match(psi).captured(1).toDouble();
        };
        const double some60 = avg(QStringLiteral("some"), QStringLiteral("avg60")), full60 = avg(QStringLiteral("full"), QStringLiteral("avg60"));
        t.add({tr("Memory pressure (last minute)"), tr("some %1 % · full %2 %").arg(some60).arg(full60),
               tr("share of time tasks waited for memory")}, full60 > 5 || some60 > 20 ? Level::Review : Level::Ok);
        if (full60 > 5 || some60 > 20)
            r.add(Level::Review, tr("Programs are waiting for memory (%1 % of the last minute)").arg(std::max(some60, full60)),
                  tr("The kernel is reclaiming or swapping to keep up; the desktop feels sluggish."), tr("Free memory or add swap / zram."));
    } else {
        r.limits << tr("Memory pressure (PSI) is not available: kernel without CONFIG_PSI or psi=0.");
    }
    r.tables << t;
    // Topology.
    Table topo;
    topo.title = tr("NUMA and cache domains");
    topo.headers = {tr("Domain"), tr("CPUs")};
    for (const QString &n : ls(root, QStringLiteral("/sys/devices/system/node"), {QStringLiteral("node[0-9]*")}))
        topo.add({tr("NUMA %1").arg(n), rd(root, QStringLiteral("/sys/devices/system/node/") + n + QStringLiteral("/cpulist"))});
    QSet<QString> l3;
    for (const QString &c : ls(root, QStringLiteral("/sys/devices/system/cpu"), {QStringLiteral("cpu[0-9]*")}))
        for (const QString &ix : ls(root, QStringLiteral("/sys/devices/system/cpu/") + c + QStringLiteral("/cache"), {QStringLiteral("index*")})) {
            const QString b = QStringLiteral("/sys/devices/system/cpu/") + c + QStringLiteral("/cache/") + ix + QLatin1Char('/');
            if (rd(root, b + QStringLiteral("level")) == QStringLiteral("3"))
                l3.insert(rd(root, b + QStringLiteral("size")) + QLatin1Char('|') + rd(root, b + QStringLiteral("shared_cpu_list")));
        }
    QStringList l3s = l3.values();
    std::sort(l3s.begin(), l3s.end());
    for (const QString &g : l3s) {
        const QString size = g.section(QLatin1Char('|'), 0, 0);  // "32768K"
        const int kib = size.endsWith(QLatin1Char('K')) ? size.chopped(1).toInt() : 0;
        topo.add({tr("L3 %1").arg(kib ? tr("%1 MB").arg(kib / 1024) : size), g.section(QLatin1Char('|'), 1)});
    }
    if (l3s.size() > 1) topo.note = tr("%1 L3 domains: on Ryzen each is one CCD. Threads that talk a lot run faster on one of them.").arg(l3s.size());
    r.tables << topo;
    if (!r.count(Level::Review)) r.add(Level::Ok, tr("Memory set up sensibly and not under pressure"));
    return r;
}

// ── S11 ACPI / WMI Firmware Browser ───────────────────────────────────────────

Report firmware(const QString &root) {
    Report r;
    r.code = QStringLiteral("S11");
    r.title = tr("ACPI / WMI Firmware Browser");
    r.about = tr("The firmware interfaces the kernel found: WMI objects with the driver that claims each, firmware settings exposed through "
                 "firmware-attributes, and the ACPI tables present. Nothing is called or changed.");
    r.limits << tr("Passwords and authentication attributes are never read.");
    static const QHash<QString, const char *> known = {
        {"887B54E3-DDDC-4B2C-8B88-68A26A8835D0", QT_TRANSLATE_NOOP("systemdiagnostics", "Lenovo GameZone (profiles, GPU, thermal) — used by Centurion")},
        {"DC2A8805-3A8C-41BA-A6F7-092E0089CD3B", QT_TRANSLATE_NOOP("systemdiagnostics", "Lenovo Other Method (power limits) — used by Centurion")},
        {"92549549-4BDE-4F06-AC04-CE8BF898DBAA", QT_TRANSLATE_NOOP("systemdiagnostics", "Lenovo Fan Method (fan tables) — used by Centurion")},
        {"362A3AFE-3D96-4665-8530-96DAD5BB300E", QT_TRANSLATE_NOOP("systemdiagnostics", "Lenovo Capability Data 00 — used by Centurion")},
        {"7A8F5407-CB67-4D6E-B547-39B3BE018154", QT_TRANSLATE_NOOP("systemdiagnostics", "Lenovo Capability Data 01")}};
    Table w;
    w.title = tr("WMI objects");
    w.headers = {tr("GUID"), tr("Driver"), tr("Known as")};
    int unbound = 0;
    for (const QString &id : ls(root, QStringLiteral("/sys/bus/wmi/devices"))) {
        const QString guid = id.section(QLatin1Char('-'), 0, 4).toUpper();
        const QString drv = driverOf(root, QStringLiteral("/sys/bus/wmi/devices/") + id);
        QString name = known.contains(guid) ? tr(known.value(guid)) : QString();
        if (guid.startsWith(QStringLiteral("C3A03776"))) name = tr("Lenovo battery information — used by Centurion");
        if (drv.isEmpty()) ++unbound;
        w.add({guid, drv.isEmpty() ? tr("none") : drv, name}, name.isEmpty() ? Level::Info : Level::Ok);
    }
    if (w.rows.isEmpty()) r.add(Level::Info, tr("No WMI object exported (no ACPI-WMI on this machine, or CONFIG_ACPI_WMI off)"));
    else {
        w.note = tr("%1 object(s) have no driver: normal for objects no Linux driver knows; Centurion talks to the Lenovo ones through ACPI directly.").arg(unbound);
        r.tables << w;
    }
    Table fa;
    fa.title = tr("Firmware settings (firmware-attributes)");
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
    const QStringList tables = ls(root, QStringLiteral("/sys/firmware/acpi/tables"));
    if (!tables.isEmpty()) {
        Table at;
        at.title = tr("ACPI tables present");
        at.headers = {tr("Tables")};
        QStringList names = tables;
        names.removeAll(QStringLiteral("data"));
        names.removeAll(QStringLiteral("dynamic"));
        at.add({names.join(QLatin1Char(' '))});
        at.note = tr("Their contents are readable by root only (acpidump).");
        r.tables << at;
    }
    r.add(Level::Info, tr("%1 WMI object(s), %2 firmware setting(s)").arg(w.rows.size()).arg(fa.rows.size()));
    return r;
}

// ── S13 USB-C PD / Charger Inspector ──────────────────────────────────────────

Report charging(const QString &root) {
    Report r;
    r.code = QStringLiteral("S13");
    r.title = tr("USB-C PD / Charger Inspector");
    r.about = tr("What the connected charger offers (its USB Power Delivery profiles), what the laptop is drawing, and the battery's condition.");
    r.limits << tr("USB-PD profiles are visible only through the UCSI driver (typec_ucsi); barrel and proprietary chargers show as an online AC adapter only.")
             << tr("Battery power figures are the battery's own estimate.");
    // Type-C ports and their partners' source capabilities.
    Table pd;
    pd.title = tr("USB-C ports");
    pd.headers = {tr("Port"), tr("Role"), tr("Partner"), tr("Charger offers"), tr("Best")};
    double bestW = 0;
    for (const QString &port : ls(root, QStringLiteral("/sys/class/typec"), {QStringLiteral("port[0-9]")})) {
        const QString b = QStringLiteral("/sys/class/typec/") + port;
        const QString partner = b + QStringLiteral("-partner");
        const bool connected = QFile::exists(root + partner);
        QStringList pdos;
        double best = 0;
        const QString pdDir = QFileInfo(root + partner + QStringLiteral("/usb_power_delivery")).canonicalFilePath();
        if (connected && !pdDir.isEmpty()) {
            for (const QString &cap : diagsrc::dir(pdDir + QStringLiteral("/source-capabilities"))) {
                const QString c = pdDir + QStringLiteral("/source-capabilities/") + cap + QLatin1Char('/');
                const double i = diagsrc::read(c + QStringLiteral("maximum_current")).remove(QStringLiteral("mA")).toDouble() / 1000;
                double v = diagsrc::read(c + QStringLiteral("voltage")).remove(QStringLiteral("mV")).toDouble() / 1000;
                if (cap.contains(QStringLiteral("programmable"))) {
                    v = diagsrc::read(c + QStringLiteral("maximum_voltage")).remove(QStringLiteral("mV")).toDouble() / 1000;
                    pdos << tr("PPS ≤%1 V %2 A").arg(v).arg(i);
                } else if (v > 0) {
                    pdos << QStringLiteral("%1 V %2 A").arg(v).arg(i);
                }
                best = std::max(best, v * i);
            }
        }
        bestW = std::max(bestW, best);
        pd.add({port, QStringLiteral("%1 / %2").arg(chosen(rd(root, b + QStringLiteral("/power_role"))), chosen(rd(root, b + QStringLiteral("/data_role")))),
                connected ? tr("connected (PD %1)").arg(rd(root, partner + QStringLiteral("/usb_power_delivery_revision"))) : tr("nothing"),
                pdos.isEmpty() ? (connected ? tr("not reported") : QStringLiteral("—")) : pdos.join(QStringLiteral(", ")),
                best > 0 ? watts(best) : QStringLiteral("—")});
    }
    if (!pd.rows.isEmpty()) r.tables << pd;
    if (bestW > 0 && bestW < 65)
        r.add(Level::Review, tr("The USB-C charger offers at most %1").arg(watts(bestW)),
              tr("Gaming laptops draw far more under load: on this charger the battery can drain while plugged in, and the firmware lowers power limits."),
              tr("Use the original adapter for gaming; a USB-C charger is fine for light use."));
    // Power supplies.
    Table ps;
    ps.title = tr("Power supplies");
    ps.headers = {tr("Supply"), tr("Type"), tr("State"), tr("Power"), tr("Detail")};
    for (const QString &s : ls(root, QStringLiteral("/sys/class/power_supply"))) {
        const QString b = QStringLiteral("/sys/class/power_supply/") + s + QLatin1Char('/');
        const QString type = rd(root, b + QStringLiteral("type"));
        if (type == QStringLiteral("Battery")) {
            if (rd(root, b + QStringLiteral("scope")) == QStringLiteral("Device")) continue;  // a mouse or a headset
            const double full = rd(root, b + QStringLiteral("energy_full")).toDouble(), design = rd(root, b + QStringLiteral("energy_full_design")).toDouble();
            double w = rd(root, b + QStringLiteral("power_now")).toDouble() / 1e6;
            if (w <= 0) w = rd(root, b + QStringLiteral("current_now")).toDouble() * rd(root, b + QStringLiteral("voltage_now")).toDouble() / 1e12;
            const QString status = rd(root, b + QStringLiteral("status"));
            const QString cycles = rd(root, b + QStringLiteral("cycle_count"));
            const QString end = rd(root, b + QStringLiteral("charge_control_end_threshold"));
            const double health = design > 0 ? full * 100 / design : 0;
            ps.add({s, tr("battery"), QStringLiteral("%1, %2 %").arg(status, rd(root, b + QStringLiteral("capacity"))), w > 0 ? watts(w) : QStringLiteral("—"),
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
            const bool online = rd(root, b + QStringLiteral("online")) == QStringLiteral("1");
            const double v = rd(root, b + QStringLiteral("voltage_now")).toDouble() / 1e6, imax = rd(root, b + QStringLiteral("current_max")).toDouble() / 1e6;
            ps.add({s, type, online ? tr("online") : tr("offline"), v > 0 && imax > 0 && online ? watts(v * imax) : QStringLiteral("—"),
                    v > 0 && imax > 0 ? tr("contract %1 V × %2 A").arg(v, 0, 'f', 1).arg(imax, 0, 'f', 2) : chosen(rd(root, b + QStringLiteral("usb_type")))});
        }
    }
    r.tables << ps;
    bool acOnline = false, discharging = false;
    for (const QStringList &row : ps.rows) {
        if (row.value(1) != tr("battery") && row.value(2) == tr("online")) acOnline = true;
        if (row.value(1) == tr("battery") && row.value(2).startsWith(QStringLiteral("Discharging"))) discharging = true;
    }
    if (acOnline && discharging)
        r.add(Level::Review, tr("Plugged in, yet the battery is discharging"), tr("The load is above what the adapter supplies (or the adapter is not recognised)."),
              tr("Use a stronger adapter, or lower the load; check that the barrel plug is fully seated."));
    if (ps.rows.isEmpty() && pd.rows.isEmpty()) r.add(Level::Unknown, tr("No battery, charger or USB-C port is reported by the kernel"));
    else if (!r.count(Level::Review) && !r.count(Level::Attention)) r.add(Level::Ok, tr("No charging problem visible"));
    return r;
}

// ── S15 PipeWire Audio Doctor ─────────────────────────────────────────────────

Report audio(const QHash<QString, QString> &out, const QString &root) {
    Report r;
    r.code = QStringLiteral("S15");
    r.title = tr("PipeWire Audio Doctor");
    r.about = tr("Sound cards and codecs, which sound servers are running (and whether two fight over the devices), and PipeWire's own view.");
    r.limits << tr("Only your own user's sound servers are seen; nothing is restarted or re-routed.");
    Table c;
    c.title = tr("Sound cards");
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
    s.title = tr("Sound servers running");
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
    r.title = tr("Kernel Feature & Firmware Dependency Check");
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
    t.title = tr("Kernel options");
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
        f.title = tr("Firmware files the kernel could not load");
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
    u.title = tr("Devices without a driver");
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
