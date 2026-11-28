#include "componentreports.h"

#include "diagsources.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QMap>
#include <QRegularExpression>

#include <algorithm>
#include <functional>

using diag::Level;
using diag::Report;
using diag::Table;

namespace componentreports {

static QString tr(const char *s) { return QCoreApplication::translate("componentreports", s); }
static QString uptime(const QJsonValue &ts) {
    if (!ts.isDouble()) return QStringLiteral("—");
    const qint64 s = qint64(ts.toDouble() / 1e6);
    return QStringLiteral("+%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
}
static qint64 number(const QString &s, int base = 10) {
    bool ok = false;
    const qint64 n = s.trimmed().toLongLong(&ok, base);
    return ok ? n : -1;
}

// ── The shared part: this boot's kernel log records of one component ──────────

/// One kind of record the helper reports for a component (health_diagnostics.rs component_of), what it means
/// and what to do. `name`, `meaning` and `action` are translated where shown.
struct Kind { const char *kind; Level level; const char *name, *meaning, *action; };

/// Adds the component's event table and one finding per kind. Returns how many events there were, or -1
/// when the scan carries no component events (an older helper, or an unreadable kernel log).
/// `levelOf` may raise or lower one event's level (a kernel warning is not an oops).
static int ledger(Report &r, const QJsonObject &scan, const char *component, const QList<Kind> &kinds,
                  const std::function<Level(const QJsonObject &, Level)> &levelOf = {}) {
    if (!scan.contains(QStringLiteral("components"))) {
        r.add(Level::Unknown, tr("The helper did not return this boot's component events"),
              tr("The installed health-reader is older than this page; reinstall Centurion's helpers."));
        return -1;
    }
    if (scan.contains(QStringLiteral("components_error"))) {
        r.add(Level::Unknown, tr("This boot's kernel log could not be read"), scan.value(QStringLiteral("components_error")).toString());
        return -1;
    }
    struct G { QString kind, title, dev, first, last; int n = 0; Level level = Level::Info; };
    QMap<QString, G> groups;  // kind|dev, in the kinds' order
    QStringList raw;
    const QJsonArray events = scan.value(QStringLiteral("components")).toObject().value(QLatin1String(component)).toArray();
    for (const QJsonValue &v : events) {
        const QJsonObject e = v.toObject();
        const QString kind = e.value(QStringLiteral("kind")).toString();
        const auto k = std::find_if(kinds.cbegin(), kinds.cend(), [&kind](const Kind &x) { return kind == QLatin1String(x.kind); });
        Level l = k == kinds.cend() ? Level::Review : k->level;
        if (levelOf) l = levelOf(e, l);
        const int order = k == kinds.cend() ? int(kinds.size()) : int(k - kinds.cbegin());
        const QString dev = e.value(QStringLiteral("dev")).toString();
        G &g = groups[QStringLiteral("%1|%2|%3").arg(order, 3, 10, QLatin1Char('0')).arg(kind, dev)];
        g.kind = kind; g.dev = dev;
        if (g.title.isEmpty()) g.title = e.value(QStringLiteral("title")).toString();
        if (!g.n) g.first = uptime(e.value(QStringLiteral("ts_us")));
        g.last = uptime(e.value(QStringLiteral("ts_us")));
        g.level = std::max(g.level, l);
        ++g.n;
        raw << uptime(e.value(QStringLiteral("ts_us"))) + QStringLiteral("  ") + e.value(QStringLiteral("text")).toString();
    }
    r.raw += raw.join(QLatin1Char('\n'));
    if (groups.isEmpty()) return 0;
    Table t;
    t.title = tr("Events in This Boot");
    t.headers = {tr("Event"), tr("Device"), tr("Count"), tr("First (uptime)"), tr("Last (uptime)")};
    for (const G &g : groups) t.add({g.title, g.dev.isEmpty() ? QStringLiteral("—") : g.dev, QString::number(g.n), g.first, g.last}, g.level);
    r.tables << t;
    // One finding per kind, over all its devices.
    QList<QString> order;
    QHash<QString, QPair<int, Level>> per;
    QHash<QString, QStringList> devs;
    for (const G &g : groups) {
        if (!per.contains(g.kind)) order << g.kind;
        auto &p = per[g.kind];
        p.first += g.n;
        p.second = std::max(p.second, g.level);
        if (!g.dev.isEmpty() && devs[g.kind].size() < 6) devs[g.kind] << g.dev;
    }
    for (const QString &kind : order) {
        const auto k = std::find_if(kinds.cbegin(), kinds.cend(), [&kind](const Kind &x) { return kind == QLatin1String(x.kind); });
        const QString name = k == kinds.cend() ? kind : tr(k->name);
        const QStringList d = devs.value(kind);
        r.add(per[kind].second, d.isEmpty() ? tr("%1 (%2×)").arg(name).arg(per[kind].first) : tr("%1 (%2×): %3").arg(name).arg(per[kind].first).arg(d.join(QStringLiteral(", "))),
              k == kinds.cend() ? QString() : tr(k->meaning), k == kinds.cend() || !*k->action ? QString() : tr(k->action));
    }
    return int(events.size());
}

/// Ok when the sources could be read and showed nothing at all (an informational event, such as an ACPI
/// firmware error, leaves the verdict at Info rather than "no fault").
static void okUnlessFound(Report &r, int events, const QString &ok) {
    if (events != 0 || r.count(Level::Attention) || r.count(Level::Review) || r.count(Level::Unknown)) return;
    r.add(Level::Ok, ok);
}

// ── H19 Kernel ────────────────────────────────────────────────────────────────

KernelState kernelState() { return {number(diagsrc::read(QStringLiteral("/proc/sys/kernel/tainted")))}; }

Report kernel(const QJsonObject &scan, const KernelState &s) {
    Report r;
    r.code = QStringLiteral("H19");
    r.title = tr("Kernel Fault Ledger");
    r.about = tr("Faults of the kernel itself in this boot: CPU lockups and RCU stalls, oops / BUG reports and warnings, tasks stuck in the kernel, "
                 "damaged kernel data, and the taint flags the kernel keeps about what happened since boot.");
    r.limits << tr("Only this boot: a fault that crashed an earlier boot is in Boot History (crash records) or is gone.")
             << tr("A kernel warning names the code that complained, not necessarily the cause.");
    const int n = ledger(r, scan, "kernel", {
        {"lockup", Level::Attention, QT_TRANSLATE_NOOP("componentreports", "CPU lockup / RCU stall"),
         QT_TRANSLATE_NOOP("componentreports", "A CPU stopped scheduling for many seconds, or RCU could not make progress: the system froze or nearly froze."),
         QT_TRANSLATE_NOOP("componentreports", "Usual causes: an undervolt or Curve Optimizer value that is too low, a driver bug, or firmware. Test at stock settings and note what ran at the time.")},
        {"oops", Level::Attention, QT_TRANSLATE_NOOP("componentreports", "Kernel oops / BUG / warning"),
         QT_TRANSLATE_NOOP("componentreports", "The kernel hit a bug or a fault in its own code. After an oops the kernel may stay unreliable until a reboot; a warning alone is a complaint, not a crash."),
         QT_TRANSLATE_NOOP("componentreports", "Save your work and reboot after an oops. The module in the title is the first suspect: update it (driver, kernel) and report it if it repeats.")},
        {"hung_task", Level::Review, QT_TRANSLATE_NOOP("componentreports", "Task blocked in the kernel"),
         QT_TRANSLATE_NOOP("componentreports", "A task waited more than two minutes inside the kernel, usually for a disk, a network file system or a stuck driver."),
         QT_TRANSLATE_NOOP("componentreports", "Look at the Storage and File Systems tabs for errors at the same time.")},
        {"corruption", Level::Attention, QT_TRANSLATE_NOOP("componentreports", "Kernel data structure damaged"),
         QT_TRANSLATE_NOOP("componentreports", "The kernel found one of its own lists damaged: memory errors (RAM, unstable memory or CPU settings) or a kernel bug."),
         QT_TRANSLATE_NOOP("componentreports", "Reboot, then run a memory test (Health → System tools → Memory test).")},
        {"ubsan", Level::Info, QT_TRANSLATE_NOOP("componentreports", "Undefined behaviour caught in kernel code"),
         QT_TRANSLATE_NOOP("componentreports", "A sanitizer built into this kernel caught a programming error in kernel code. It is a bug report for the kernel developers and usually harmless for you."),
         ""}},
        [](const QJsonObject &e, Level l) {
            return e.value(QStringLiteral("title")).toString().startsWith(QStringLiteral("Kernel warning")) ? Level::Review : l;
        });
    // The taint flags (Documentation/admin-guide/tainted-kernels.rst): what happened since boot, even after the log rotated.
    struct Flag { int bit; char letter; Level level; const char *meaning; };
    static const Flag flags[] = {
        {0, 'P', Level::Info, QT_TRANSLATE_NOOP("componentreports", "A proprietary module is loaded (e.g. NVIDIA)")},
        {1, 'F', Level::Info, QT_TRANSLATE_NOOP("componentreports", "A module was force-loaded")},
        {2, 'S', Level::Review, QT_TRANSLATE_NOOP("componentreports", "The CPU runs outside its specification (SMP-unsafe)")},
        {3, 'R', Level::Info, QT_TRANSLATE_NOOP("componentreports", "A module was force-unloaded")},
        {4, 'M', Level::Review, QT_TRANSLATE_NOOP("componentreports", "A machine check occurred (see Machine Checks)")},
        {5, 'B', Level::Attention, QT_TRANSLATE_NOOP("componentreports", "A bad memory page was referenced")},
        {6, 'U', Level::Info, QT_TRANSLATE_NOOP("componentreports", "Tainted on request of user space")},
        {7, 'D', Level::Review, QT_TRANSLATE_NOOP("componentreports", "The kernel oopsed or hit a BUG in this boot")},
        {8, 'A', Level::Info, QT_TRANSLATE_NOOP("componentreports", "An ACPI table was overridden")},
        {9, 'W', Level::Info, QT_TRANSLATE_NOOP("componentreports", "The kernel issued a warning")},
        {10, 'C', Level::Info, QT_TRANSLATE_NOOP("componentreports", "A staging (not yet stable) driver is loaded")},
        {11, 'I', Level::Info, QT_TRANSLATE_NOOP("componentreports", "The kernel works around a platform firmware bug")},
        {12, 'O', Level::Info, QT_TRANSLATE_NOOP("componentreports", "An out-of-tree module is loaded")},
        {13, 'E', Level::Info, QT_TRANSLATE_NOOP("componentreports", "An unsigned module is loaded")},
        {14, 'L', Level::Review, QT_TRANSLATE_NOOP("componentreports", "A soft lockup occurred in this boot")},
        {15, 'K', Level::Info, QT_TRANSLATE_NOOP("componentreports", "The kernel has been live-patched")},
        {16, 'X', Level::Info, QT_TRANSLATE_NOOP("componentreports", "Auxiliary taint (set by the distribution)")},
        {17, 'T', Level::Info, QT_TRANSLATE_NOOP("componentreports", "Built with structure randomisation")},
        {18, 'N', Level::Info, QT_TRANSLATE_NOOP("componentreports", "A kernel test module is loaded")}};
    if (s.taint < 0) {
        r.limits << tr("/proc/sys/kernel/tainted is not readable: the taint flags are not shown.");
    } else {
        Table t;
        t.title = tr("Kernel Taint Flags");
        t.headers = {tr("Flag"), tr("Meaning")};
        for (const Flag &f : flags) {
            if (!(s.taint >> f.bit & 1)) continue;
            t.add({QString(QLatin1Char(f.letter)), tr(f.meaning)}, f.level);
            if (f.letter == 'B')
                r.add(Level::Attention, tr("The kernel referenced a bad memory page in this boot (taint B)"),
                      tr("Memory the kernel used was damaged or in an impossible state: RAM errors or unstable memory settings are the usual cause."),
                      tr("Run a memory test (Health → System tools → Memory test) and undo memory overclocks (EXPO / XMP)."));
            else if (f.level == Level::Review && !n)
                r.add(Level::Review, tr("Taint %1: %2").arg(QLatin1Char(f.letter)).arg(tr(f.meaning)),
                      tr("The kernel log of this boot no longer holds the record (it is limited in size), but the kernel remembers it until reboot."));
        }
        t.note = s.taint ? tr("Taint value %1. P, O and E are normal with the NVIDIA driver; they only tell kernel developers which code was loaded.").arg(s.taint)
                         : tr("The kernel is not tainted.");
        if (t.rows.isEmpty()) t.add({QStringLiteral("—"), tr("none")}, Level::Ok);
        r.tables << t;
    }
    okUnlessFound(r, n, tr("No lockup, oops or hung task in this boot"));
    return r;
}

// ── H20 Storage ───────────────────────────────────────────────────────────────

StorageState storageState() {
    StorageState s;
    for (const QString &name : diagsrc::dir(QStringLiteral("/sys/block"))) {
        // Disks only: loop devices, RAM disks, zram, device mapper and md are made of other disks.
        static const QRegularExpression disk(QStringLiteral(R"(^(nvme\d+n\d+|sd[a-z]+|hd[a-z]+|vd[a-z]+|xvd[a-z]+|mmcblk\d+)$)"));
        if (!disk.match(name).hasMatch()) continue;
        const QString base = QStringLiteral("/sys/block/") + name;
        Disk d;
        d.name = name;
        d.model = diagsrc::read(base + QStringLiteral("/device/model"));
        if (d.model.isEmpty()) d.model = diagsrc::read(base + QStringLiteral("/device/name"));  // SD / eMMC
        const qint64 sectors = number(diagsrc::read(base + QStringLiteral("/size")));
        if (sectors == 0) continue;  // an empty card reader or drive slot
        d.bytes = sectors < 0 ? -1 : sectors * 512;
        d.readOnly = diagsrc::read(base + QStringLiteral("/ro")) == QStringLiteral("1");
        // SCSI / SATA / USB disks: commands the disk completed with an error since boot (hex).
        const QString err = diagsrc::read(base + QStringLiteral("/device/ioerr_cnt"));
        if (!err.isEmpty()) d.ioErrors = number(QString(err).remove(QStringLiteral("0x")), 16);
        s.disks << d;
    }
    s.mdstat = diagsrc::read(QStringLiteral("/proc/mdstat"));
    return s;
}

Report storage(const QJsonObject &scan, const StorageState &s) {
    Report r;
    r.code = QStringLiteral("H20");
    r.title = tr("Storage Error Ledger");
    r.about = tr("This boot's disk errors below the file system: NVMe timeouts and resets, SATA link and command errors, read / write failures "
                 "the block layer reported, medium errors the drive itself reported, failed RAID members, device mapper and SD / eMMC errors.");
    r.limits << tr("The drive's own health record (SMART, NVMe error log, wear) is in Findings → NVMe Deep Health and Health → Drive health.")
             << tr("A USB disk that is unplugged without unmounting also logs I/O errors.");
    const int n = ledger(r, scan, "storage", {
        {"nvme", Level::Attention, QT_TRANSLATE_NOOP("componentreports", "NVMe timeout / controller reset"),
         QT_TRANSLATE_NOOP("componentreports", "An NVMe drive did not answer in time and the kernel aborted the request or reset the controller."),
         QT_TRANSLATE_NOOP("componentreports", "Back up. Then check the drive's error log (Findings → NVMe Deep Health) and update its firmware; APST power saving is a common cause.")},
        {"blk_io", Level::Attention, QT_TRANSLATE_NOOP("componentreports", "Read / write failed"),
         QT_TRANSLATE_NOOP("componentreports", "A read or write to the disk failed: the data of that request was not stored or not read."),
         QT_TRANSLATE_NOOP("componentreports", "Back up now. Check the drive's SMART / NVMe health; for SATA and USB disks also the cable and the port.")},
        {"ata", Level::Review, QT_TRANSLATE_NOOP("componentreports", "SATA link / command error"),
         QT_TRANSLATE_NOOP("componentreports", "The SATA link reset or a command failed. Repeated, it points at the cable, the port, the power supply or the drive."),
         QT_TRANSLATE_NOOP("componentreports", "Reseat or replace the SATA cable, try another port, and read the drive's SMART log.")},
        {"medium", Level::Attention, QT_TRANSLATE_NOOP("componentreports", "Drive reported a medium / hardware error"),
         QT_TRANSLATE_NOOP("componentreports", "The drive itself reported that it could not read or write a sector."),
         QT_TRANSLATE_NOOP("componentreports", "Back up now; check SMART (reallocated and pending sectors) and plan to replace the drive.")},
        {"raid", Level::Attention, QT_TRANSLATE_NOOP("componentreports", "RAID member failed"),
         QT_TRANSLATE_NOOP("componentreports", "A member of a software RAID array failed and was removed; the array runs without redundancy."),
         QT_TRANSLATE_NOOP("componentreports", "Read /proc/mdstat, replace the member and add it back (mdadm --manage /dev/mdX --add).")},
        {"dm", Level::Review, QT_TRANSLATE_NOOP("componentreports", "Device mapper error"),
         QT_TRANSLATE_NOOP("componentreports", "A device mapper target (LVM, LUKS, dm-thin, dm-cache) reported an error; the disk under it is the first suspect."), ""},
        {"mmc", Level::Review, QT_TRANSLATE_NOOP("componentreports", "SD / eMMC error"),
         QT_TRANSLATE_NOOP("componentreports", "The SD card or eMMC reported an error or a timeout. Cards wear out, and some readers do not get along with some cards."),
         QT_TRANSLATE_NOOP("componentreports", "Back up the card; try another card or reader.")}});
    if (!s.disks.isEmpty()) {
        Table t;
        t.title = tr("Disks");
        t.headers = {tr("Disk"), tr("Model"), tr("Size"), tr("Mode"), tr("Failed I/O since boot")};
        for (const Disk &d : s.disks) {
            const Level l = d.ioErrors > 0 ? Level::Review : Level::Info;
            t.add({d.name, d.model.isEmpty() ? QStringLiteral("—") : d.model, d.bytes < 0 ? QStringLiteral("—") : tr("%1 GB").arg(d.bytes / 1e9, 0, 'f', 0),
                   d.readOnly ? tr("read-only") : tr("read-write"), d.ioErrors < 0 ? QStringLiteral("—") : QString::number(d.ioErrors)}, l);
            if (d.ioErrors > 0)
                r.add(Level::Review, tr("%1: %2 command(s) failed since boot").arg(d.name).arg(d.ioErrors),
                      tr("The disk's driver counts every command that completed with an error (ioerr_cnt)."),
                      tr("Read the drive's SMART log; check the cable or USB adapter."));
        }
        t.note = tr("Failed I/O is counted by SCSI, SATA and USB disks only; NVMe keeps its own error log.");
        r.tables << t;
    }
    // Software RAID: "md0 : active raid1 sdb1[1] sda1[0]" then "… [2/1] [U_]": "_" is a missing member.
    if (!s.mdstat.isEmpty()) {
        Table t;
        t.title = tr("Software RAID Arrays");
        t.headers = {tr("Array"), tr("State"), tr("Members")};
        const QStringList lines = s.mdstat.split(QLatin1Char('\n'));
        static const QRegularExpression head(QStringLiteral(R"(^(md\S+) : (\S+) (\S+)? ?(.*)$)")), status(QStringLiteral(R"(\[([U_]+)\])"));
        for (int i = 0; i < lines.size(); ++i) {
            const QRegularExpressionMatch m = head.match(lines.at(i));
            if (!m.hasMatch()) continue;
            const QRegularExpressionMatch st = i + 1 < lines.size() ? status.match(lines.at(i + 1)) : QRegularExpressionMatch();
            const bool degraded = st.hasMatch() && st.captured(1).contains(QLatin1Char('_'));
            t.add({m.captured(1), m.captured(2) + (st.hasMatch() ? QStringLiteral(" [%1]").arg(st.captured(1)) : QString()), m.captured(4)},
                  degraded ? Level::Attention : Level::Ok);
            if (degraded)
                r.add(Level::Attention, tr("%1 is degraded [%2]").arg(m.captured(1), st.captured(1)),
                      tr("A member is missing (_): the array keeps working, without redundancy."),
                      tr("Replace the member and add it back (mdadm --manage /dev/%1 --add).").arg(m.captured(1)));
        }
        if (!t.rows.isEmpty()) r.tables << t;
    }
    okUnlessFound(r, n, tr("No disk error in this boot"));
    return r;
}

// ── H21 GPU (AMD / Intel) ─────────────────────────────────────────────────────

QList<DrmCard> drmCards() {
    QList<DrmCard> out;
    static const QRegularExpression card(QStringLiteral(R"(^card\d+$)"));
    for (const QString &c : diagsrc::dir(QStringLiteral("/sys/class/drm"))) {
        if (!card.match(c).hasMatch()) continue;
        const QString dev = QStringLiteral("/sys/class/drm/%1/device").arg(c);
        const QFileInfo pci(QFileInfo(dev).canonicalFilePath());
        DrmCard d;
        d.card = c;
        d.driver = QFileInfo(QFileInfo(dev + QStringLiteral("/driver")).canonicalFilePath()).fileName();
        d.pci = pci.fileName();
        d.name = diagsrc::pciName(diagsrc::read(dev + QStringLiteral("/vendor")), diagsrc::read(dev + QStringLiteral("/device")), diagsrc::read(dev + QStringLiteral("/class")));
        out << d;
    }
    return out;
}

Report gpu(const QJsonObject &scan, const QList<DrmCard> &cards) {
    Report r;
    r.code = QStringLiteral("H21");
    r.title = tr("AMD / Intel GPU Fault Ledger");
    r.about = tr("This boot's faults of the GPUs the open drivers run (amdgpu, i915, xe, nouveau): hangs and resets, GPU page faults and "
                 "display / GPU driver (DRM) errors.");
    r.limits << tr("The NVIDIA driver reports its faults as Xid codes: they are on the NVIDIA Xid tab.")
             << tr("A single DRM error at boot or when a monitor is plugged in is common.");
    const int n = ledger(r, scan, "gpu", {
        {"amdgpu", Level::Attention, QT_TRANSLATE_NOOP("componentreports", "AMD GPU hang / reset"),
         QT_TRANSLATE_NOOP("componentreports", "The AMD GPU stopped finishing work and the driver reset it; the screen freezes for a moment or the session crashes."),
         QT_TRANSLATE_NOOP("componentreports", "Update the kernel, Mesa and linux-firmware; note which program ran. Undo GPU or memory overclocks.")},
        {"igpu", Level::Review, QT_TRANSLATE_NOOP("componentreports", "Intel GPU hang / engine reset"),
         QT_TRANSLATE_NOOP("componentreports", "The Intel GPU hung on a job and the driver reset the engine."),
         QT_TRANSLATE_NOOP("componentreports", "Update the kernel, Mesa and linux-firmware; Panel Self Refresh (i915.enable_psr=0) is a common workaround for display hangs.")},
        {"gpu_fault", Level::Review, QT_TRANSLATE_NOOP("componentreports", "GPU page fault"),
         QT_TRANSLATE_NOOP("componentreports", "A program or the driver used GPU memory it does not own; a hang or a crash of that program often follows."),
         QT_TRANSLATE_NOOP("componentreports", "Usually a driver or program bug: update Mesa and the kernel.")},
        {"nouveau", Level::Review, QT_TRANSLATE_NOOP("componentreports", "nouveau GPU error"),
         QT_TRANSLATE_NOOP("componentreports", "nouveau, the open driver for NVIDIA GPUs, reported a fault."), ""},
        {"drm_error", Level::Info, QT_TRANSLATE_NOOP("componentreports", "Display / GPU driver error"),
         QT_TRANSLATE_NOOP("componentreports", "The display or GPU driver logged an error. Matters when it repeats or comes with a black screen or a freeze."), ""}});
    if (!cards.isEmpty()) {
        Table t;
        t.title = tr("Graphics Devices");
        t.headers = {tr("Card"), tr("GPU"), tr("Driver"), tr("PCI address")};
        for (const DrmCard &c : cards)
            t.add({c.card, c.name.isEmpty() ? QStringLiteral("—") : c.name, c.driver.isEmpty() ? tr("none") : c.driver, c.pci.isEmpty() ? QStringLiteral("—") : c.pci});
        r.tables << t;
    }
    okUnlessFound(r, n, tr("No GPU hang, reset or fault in this boot"));
    return r;
}

// ── H22 Thermal & Power ───────────────────────────────────────────────────────

PowerState powerState() {
    PowerState s;
    for (const QString &z : diagsrc::dir(QStringLiteral("/sys/class/thermal"), {QStringLiteral("thermal_zone*")})) {
        const QString base = QStringLiteral("/sys/class/thermal/") + z;
        ThermalZone t;
        t.name = z;
        t.type = diagsrc::read(base + QStringLiteral("/type"));
        if (const qint64 m = number(diagsrc::read(base + QStringLiteral("/temp"))); m != -1) t.tempC = m / 1000.0;
        for (int i = 0; i < 32; ++i) {
            const QString type = diagsrc::read(base + QStringLiteral("/trip_point_%1_type").arg(i));
            if (type.isEmpty()) break;
            if (type == QStringLiteral("critical")) {
                if (const qint64 m = number(diagsrc::read(base + QStringLiteral("/trip_point_%1_temp").arg(i))); m > 0) t.criticalC = m / 1000.0;
                break;
            }
        }
        s.zones << t;
    }
    // Intel: how often each core / package hit its thermal limit since boot (one counter per logical CPU).
    for (const QString &cpu : diagsrc::dir(QStringLiteral("/sys/devices/system/cpu"), {QStringLiteral("cpu[0-9]*")})) {
        const QString base = QStringLiteral("/sys/devices/system/cpu/%1/thermal_throttle/").arg(cpu);
        const qint64 core = number(diagsrc::read(base + QStringLiteral("core_throttle_count")));
        const qint64 pkg = number(diagsrc::read(base + QStringLiteral("package_throttle_count")));
        if (core >= 0) s.coreThrottle = std::max<qint64>(s.coreThrottle, 0) + core;
        if (pkg >= 0) s.packageThrottle = std::max(s.packageThrottle, pkg);  // the same counter on every CPU of a package
    }
    return s;
}

Report power(const QJsonObject &scan, const PowerState &s) {
    Report r;
    r.code = QStringLiteral("H22");
    r.title = tr("Thermal & Power Ledger");
    r.about = tr("This boot's temperature alarms and throttling, ACPI firmware errors, devices that did not wake from their deepest power state "
                 "and failed suspend / resume steps, with the current temperature of every thermal zone.");
    r.limits << tr("ACPI errors are firmware bugs the kernel works around; most laptops log some at every boot.")
             << tr("Sleep across boots: Findings → Sleep / Resume Failure Matrix.");
    const int n = ledger(r, scan, "power", {
        {"thermal", Level::Review, QT_TRANSLATE_NOOP("componentreports", "Temperature alarm / throttling"),
         QT_TRANSLATE_NOOP("componentreports", "A temperature limit was reached: the CPU slowed itself down, or at the critical limit the system shuts down to protect itself."),
         QT_TRANSLATE_NOOP("componentreports", "Clean the fans and vents, check the fan curve (Cooling), and lower power limits if it happens under normal load.")},
        {"d3cold", Level::Attention, QT_TRANSLATE_NOOP("componentreports", "Device did not wake from D3cold"),
         QT_TRANSLATE_NOOP("componentreports", "A device did not come back from its deepest power state; on laptops it is usually the discrete GPU, and a hang often follows."),
         QT_TRANSLATE_NOOP("componentreports", "Update the BIOS and the GPU driver; keeping the dGPU awake (runtime PM off) is the usual workaround.")},
        {"acpi", Level::Info, QT_TRANSLATE_NOOP("componentreports", "ACPI firmware error"),
         QT_TRANSLATE_NOOP("componentreports", "The firmware's ACPI tables have an error the kernel worked around. It matters only if a feature (battery, fans, brightness, sleep, keys) misbehaves."),
         QT_TRANSLATE_NOOP("componentreports", "If a feature is broken, update the BIOS; otherwise nothing to do.")},
        {"acpi_warning", Level::Info, QT_TRANSLATE_NOOP("componentreports", "ACPI firmware warning"),
         QT_TRANSLATE_NOOP("componentreports", "The firmware's ACPI tables have an inconsistency the kernel noted. Usually harmless."), ""},
        {"pm", Level::Review, QT_TRANSLATE_NOOP("componentreports", "Suspend / resume step failed"),
         QT_TRANSLATE_NOOP("componentreports", "A device or driver failed a suspend or resume step: the system may not have slept, or woke with that device not working."),
         QT_TRANSLATE_NOOP("componentreports", "The device named in the message is the suspect; Findings → Sleep / Resume Failure Matrix compares this across boots.")}},
        [](const QJsonObject &e, Level l) {
            return e.value(QStringLiteral("kind")).toString() == QStringLiteral("thermal") && e.value(QStringLiteral("level")).toString() == QStringLiteral("critical")
                       ? Level::Attention : l;
        });
    if (!s.zones.isEmpty()) {
        Table t;
        t.title = tr("Thermal Zones Now");
        t.headers = {tr("Zone"), tr("Sensor"), tr("Temperature"), tr("Critical at")};
        for (const ThermalZone &z : s.zones) {
            // Some firmware zones report placeholders (0 °C, -273 °C): those are not judged.
            const bool real = z.tempC > 0 && z.tempC < 150, judged = real && z.criticalC > 0;
            const bool near = judged && z.tempC >= z.criticalC - 5;
            t.add({z.name, z.type.isEmpty() ? QStringLiteral("—") : z.type, real ? tr("%1 °C").arg(z.tempC, 0, 'f', 0) : QStringLiteral("—"),
                   z.criticalC > 0 ? tr("%1 °C").arg(z.criticalC, 0, 'f', 0) : QStringLiteral("—")}, near ? Level::Attention : Level::Info);
            if (near)
                r.add(Level::Attention, tr("%1 (%2) is at %3 °C, within 5 °C of its critical limit (%4 °C)").arg(z.name, z.type).arg(z.tempC, 0, 'f', 0).arg(z.criticalC, 0, 'f', 0),
                      tr("At the critical limit the system powers off to protect the hardware."), tr("Reduce the load now; check fans and vents."));
        }
        r.tables << t;
    }
    if (s.coreThrottle >= 0 || s.packageThrottle >= 0) {
        Table t;
        t.title = tr("CPU Thermal Throttling Since Boot");
        t.headers = {tr("Counter"), tr("Events")};
        if (s.coreThrottle >= 0) t.add({tr("Core limit reached (all cores)"), QString::number(s.coreThrottle)});
        if (s.packageThrottle >= 0) t.add({tr("Package limit reached"), QString::number(s.packageThrottle)});
        t.note = tr("Reaching the limit under full load is how modern CPUs boost; it matters when performance drops in ordinary use.");
        r.tables << t;
    }
    okUnlessFound(r, n, tr("No temperature alarm, wake failure or failed suspend step in this boot"));
    return r;
}

// ── H23 Memory ────────────────────────────────────────────────────────────────

MemoryState memoryState() {
    MemoryState s;
    for (const QString &mc : diagsrc::dir(QStringLiteral("/sys/devices/system/edac/mc"), {QStringLiteral("mc*")})) {
        const QString base = QStringLiteral("/sys/devices/system/edac/mc/") + mc;
        EdacController c;
        c.name = mc + QStringLiteral(" · ") + diagsrc::read(base + QStringLiteral("/mc_name"));
        c.ce = number(diagsrc::read(base + QStringLiteral("/ce_count")));
        c.ue = number(diagsrc::read(base + QStringLiteral("/ue_count")));
        s.edac << c;
    }
    static const QRegularExpression corrupted(QStringLiteral(R"(HardwareCorrupted:\s+(\d+))")), oom(QStringLiteral(R"((?:^|\n)oom_kill (\d+))"));
    if (const auto m = corrupted.match(diagsrc::read(QStringLiteral("/proc/meminfo"))); m.hasMatch()) s.hardwareCorruptedKib = m.captured(1).toLongLong();
    if (const auto m = oom.match(diagsrc::read(QStringLiteral("/proc/vmstat"), 256 * 1024)); m.hasMatch()) s.oomKills = m.captured(1).toLongLong();
    return s;
}

Report memory(const QJsonObject &scan, const MemoryState &s) {
    Report r;
    r.code = QStringLiteral("H23");
    r.title = tr("Memory Fault Ledger");
    r.about = tr("This boot's out-of-memory kills and allocation failures, ECC memory errors (EDAC) and memory pages taken offline for hardware "
                 "errors, with the kernel's own counters of them since boot.");
    r.limits << tr("Without ECC memory (most laptops) the hardware cannot report memory errors: they show up as crashes, machine checks or corrupted data instead.")
             << tr("Memory pressure over time: Health → System tools (S10).");
    const int n = ledger(r, scan, "memory", {
        {"oom", Level::Review, QT_TRANSLATE_NOOP("componentreports", "Out of memory: a process was killed"),
         QT_TRANSLATE_NOOP("componentreports", "The system (or a cgroup's limit) ran out of memory and the kernel killed a process to free some."),
         QT_TRANSLATE_NOOP("componentreports", "Find the program that grew (the message names it); add swap or zram, or close memory-heavy programs.")},
        {"alloc", Level::Info, QT_TRANSLATE_NOOP("componentreports", "Kernel memory allocation failure"),
         QT_TRANSLATE_NOOP("componentreports", "The kernel could not get memory for a request at once and usually retried. Repeats under load mean memory is fragmented or nearly full."), ""},
        {"hwpoison", Level::Attention, QT_TRANSLATE_NOOP("componentreports", "Memory page taken offline"),
         QT_TRANSLATE_NOOP("componentreports", "A memory page with a hardware error was taken out of use; a program using it may have been killed."),
         QT_TRANSLATE_NOOP("componentreports", "Run a memory test; with ECC memory, the EDAC counters below name the controller.")},
        {"edac_ce", Level::Review, QT_TRANSLATE_NOOP("componentreports", "Corrected ECC memory error"),
         QT_TRANSLATE_NOOP("componentreports", "ECC memory found and corrected an error. A few over months are normal; a steady stream points at one memory module."),
         QT_TRANSLATE_NOOP("componentreports", "If they repeat, test or replace the module the message names.")},
        {"edac_ue", Level::Attention, QT_TRANSLATE_NOOP("componentreports", "Uncorrected ECC memory error"),
         QT_TRANSLATE_NOOP("componentreports", "ECC memory found an error it could not correct: data in memory was lost."),
         QT_TRANSLATE_NOOP("componentreports", "Replace or reseat the module the message names; run a memory test.")}});
    Table t;
    t.title = tr("Memory Error Counters Since Boot");
    t.headers = {tr("Counter"), tr("Value")};
    for (const EdacController &c : s.edac) {
        t.add({tr("%1: corrected (ECC)").arg(c.name), c.ce < 0 ? QStringLiteral("—") : QString::number(c.ce)}, c.ce > 0 ? Level::Review : Level::Ok);
        t.add({tr("%1: uncorrected (ECC)").arg(c.name), c.ue < 0 ? QStringLiteral("—") : QString::number(c.ue)}, c.ue > 0 ? Level::Attention : Level::Ok);
        if (c.ue > 0) r.add(Level::Attention, tr("%1: %2 uncorrected memory error(s) since boot").arg(c.name).arg(c.ue));
        else if (c.ce > 0) r.add(Level::Review, tr("%1: %2 corrected memory error(s) since boot").arg(c.name).arg(c.ce),
                                 tr("Corrected errors are harmless one by one; a growing count points at one module."));
    }
    if (s.edac.isEmpty()) r.limits << tr("No EDAC memory controller is registered: this system's memory errors are not counted by hardware.");
    if (s.hardwareCorruptedKib >= 0) {
        t.add({tr("Memory taken offline (HardwareCorrupted)"), tr("%1 kB").arg(s.hardwareCorruptedKib)}, s.hardwareCorruptedKib > 0 ? Level::Attention : Level::Ok);
        if (s.hardwareCorruptedKib > 0)
            r.add(Level::Attention, tr("%1 kB of memory are taken offline for hardware errors").arg(s.hardwareCorruptedKib),
                  tr("The kernel stopped using memory pages that failed."), tr("Run a memory test; with ECC memory, replace the failing module."));
    }
    if (s.oomKills >= 0) {
        t.add({tr("Processes killed for lack of memory"), QString::number(s.oomKills)}, s.oomKills > 0 ? Level::Review : Level::Ok);
        if (s.oomKills > 0 && n == 0)
            r.add(Level::Review, tr("%1 process(es) were killed for lack of memory in this boot").arg(s.oomKills),
                  tr("The kernel log no longer holds the records (it is limited in size), but the kernel counts every kill."));
    }
    if (!t.rows.isEmpty()) r.tables << t;
    okUnlessFound(r, n, tr("No memory fault in this boot"));
    return r;
}

// ── H24 USB ───────────────────────────────────────────────────────────────────

Report usb(const QJsonObject &scan) {
    Report r;
    r.code = QStringLiteral("H24");
    r.title = tr("USB Fault Ledger");
    r.about = tr("This boot's USB faults: devices that did not enumerate, ports switched off for over-current, host controllers that stopped "
                 "responding, hub errors and USB-C (UCSI) controller errors.");
    r.limits << tr("Disconnects and reconnects across boots: Findings → USB Stability Watch.")
             << tr("A device unplugged while it starts up can log one enumeration error; that is not a fault.");
    const int n = ledger(r, scan, "usb", {
        {"overcurrent", Level::Attention, QT_TRANSLATE_NOOP("componentreports", "USB port over-current"),
         QT_TRANSLATE_NOOP("componentreports", "A port drew more current than allowed and was switched off: a faulty device or cable, or a short."),
         QT_TRANSLATE_NOOP("componentreports", "Unplug what was on that port and try it alone; replace a damaged cable.")},
        {"xhci", Level::Attention, QT_TRANSLATE_NOOP("componentreports", "USB host controller stopped responding"),
         QT_TRANSLATE_NOOP("componentreports", "Every device on this controller is gone until it is reset or the system reboots."),
         QT_TRANSLATE_NOOP("componentreports", "Usually a firmware or kernel bug triggered by one device: update the kernel and BIOS, and note what was plugged in.")},
        {"enumerate", Level::Review, QT_TRANSLATE_NOOP("componentreports", "USB device failed to enumerate"),
         QT_TRANSLATE_NOOP("componentreports", "A device was plugged in or reset and did not answer correctly (error -71 / -110): a bad cable, a hub without enough power, or the device itself."),
         QT_TRANSLATE_NOOP("componentreports", "Try another cable and port, and a powered hub for power-hungry devices.")},
        {"hub", Level::Review, QT_TRANSLATE_NOOP("componentreports", "USB hub port error"),
         QT_TRANSLATE_NOOP("componentreports", "A hub could not read the state of one of its ports."), ""},
        {"typec", Level::Review, QT_TRANSLATE_NOOP("componentreports", "USB-C controller error"),
         QT_TRANSLATE_NOOP("componentreports", "The USB-C / Power Delivery controller (UCSI) reported an error: charging or display output over USB-C may misbehave."),
         QT_TRANSLATE_NOOP("componentreports", "Update the BIOS and the EC firmware; USB-C details are in Health → System tools (S13).")}});
    okUnlessFound(r, n, tr("No USB fault in this boot"));
    return r;
}

// ── H25 Audio ─────────────────────────────────────────────────────────────────

Report audio(const QJsonObject &scan, const QString &cards) {
    Report r;
    r.code = QStringLiteral("H25");
    r.title = tr("Audio Fault Ledger");
    r.about = tr("This boot's sound driver faults: codecs that stopped answering, audio DSP (SOF) firmware that failed to load or start, "
                 "and other errors of the sound drivers, with the sound cards the kernel registered.");
    r.limits << tr("Faults of the sound server (PipeWire, PulseAudio) are not in the kernel log.");
    const int n = ledger(r, scan, "audio", {
        {"firmware", Level::Attention, QT_TRANSLATE_NOOP("componentreports", "Audio DSP firmware failure"),
         QT_TRANSLATE_NOOP("componentreports", "The audio DSP's firmware failed to load or start: there is no sound from the devices behind it."),
         QT_TRANSLATE_NOOP("componentreports", "Install or update the SOF firmware (Gentoo: sys-firmware/sof-firmware) and the kernel.")},
        {"codec_timeout", Level::Review, QT_TRANSLATE_NOOP("componentreports", "Audio codec did not respond"),
         QT_TRANSLATE_NOOP("componentreports", "The sound codec stopped answering commands; sound can crackle or stop until the driver recovers."),
         QT_TRANSLATE_NOOP("componentreports", "Disabling audio power saving (snd_hda_intel.power_save=0) is the usual workaround.")},
        {"error", Level::Info, QT_TRANSLATE_NOOP("componentreports", "Sound driver error"),
         QT_TRANSLATE_NOOP("componentreports", "A sound driver logged an error. It matters if a sound device is missing or silent."), ""}});
    // /proc/asound/cards: " 0 [PCH            ]: HDA-Intel - HDA Intel PCH" per card.
    static const QRegularExpression line(QStringLiteral(R"(^\s*(\d+) \[(\S+)\s*\]: (\S+) - (.+)$)"));
    Table t;
    t.title = tr("Sound Cards");
    t.headers = {tr("Card"), tr("Driver"), tr("Name")};
    for (const QString &l : cards.split(QLatin1Char('\n'))) {
        const QRegularExpressionMatch m = line.match(l);
        if (m.hasMatch()) t.add({m.captured(1) + QStringLiteral(" · ") + m.captured(2), m.captured(3), m.captured(4).trimmed()});
    }
    if (!t.rows.isEmpty()) r.tables << t;
    else if (cards.contains(QStringLiteral("no soundcards")))
        r.add(Level::Review, tr("The kernel registered no sound card"), tr("The sound driver did not find, or could not start, the audio hardware."),
              tr("See the firmware and driver errors (Firmware & Drivers tab)."));
    okUnlessFound(r, n, tr("No sound driver fault in this boot"));
    return r;
}

// ── H26 Firmware & Drivers ────────────────────────────────────────────────────

Report firmware(const QJsonObject &scan) {
    Report r;
    r.code = QStringLiteral("H26");
    r.title = tr("Firmware & Driver Ledger");
    r.about = tr("This boot's firmware files that were missing or failed to load, drivers that could not initialise their device, and kernel "
                 "modules that do not match the running kernel.");
    r.limits << tr("Wi-Fi and Bluetooth firmware failures are on the Wi-Fi / Bluetooth tab.")
             << tr("Many drivers ask for several firmware versions and use the newest one installed: a missing file alone is not a fault.");
    QJsonObject withFiles = scan;
    // "Direct firmware load for <file> failed": the file is the interesting part, the device is the driver.
    if (scan.contains(QStringLiteral("components"))) {
        QJsonObject comps = scan.value(QStringLiteral("components")).toObject();
        QJsonArray events;
        static const QRegularExpression file(QStringLiteral(R"(firmware load for (\S+) failed)"));
        for (const QJsonValue &v : comps.value(QStringLiteral("firmware")).toArray()) {
            QJsonObject e = v.toObject();
            if (const auto m = file.match(e.value(QStringLiteral("text")).toString()); m.hasMatch())
                e.insert(QStringLiteral("dev"), (e.value(QStringLiteral("dev")).toString().section(QLatin1Char(' '), 0, 0) + QLatin1Char(' ') + m.captured(1)).trimmed());
            events << e;
        }
        comps.insert(QStringLiteral("firmware"), events);
        withFiles.insert(QStringLiteral("components"), comps);
    }
    const int n = ledger(r, withFiles, "firmware", {
        {"module", Level::Attention, QT_TRANSLATE_NOOP("componentreports", "Kernel module does not match the kernel"),
         QT_TRANSLATE_NOOP("componentreports", "A module was built for another kernel and could not be loaded: the feature it provides is missing in this boot."),
         QT_TRANSLATE_NOOP("componentreports", "Rebuild out-of-tree modules for the running kernel (Gentoo: emerge @module-rebuild).")},
        {"fw_failed", Level::Review, QT_TRANSLATE_NOOP("componentreports", "Firmware failed to load"),
         QT_TRANSLATE_NOOP("componentreports", "A driver could not load its firmware; the device may not work at all in this boot."),
         QT_TRANSLATE_NOOP("componentreports", "Install or update linux-firmware (Gentoo: sys-kernel/linux-firmware) and check that the file the kernel asked for exists.")},
        {"probe", Level::Review, QT_TRANSLATE_NOOP("componentreports", "Driver could not initialise a device"),
         QT_TRANSLATE_NOOP("componentreports", "A driver found its device but failed to start it; that device is not available unless a later attempt succeeded."),
         QT_TRANSLATE_NOOP("componentreports", "If a device is missing, the driver and the error code in the message are the lead; a kernel or BIOS update often helps.")},
        {"fw_missing", Level::Info, QT_TRANSLATE_NOOP("componentreports", "Firmware file not found"),
         QT_TRANSLATE_NOOP("componentreports", "A driver asked for a firmware file that is not installed. It often tries another version next; it matters only if the device does not work."),
         QT_TRANSLATE_NOOP("componentreports", "If the device does not work, install or update linux-firmware (Gentoo: sys-kernel/linux-firmware).")}});
    okUnlessFound(r, n, tr("No firmware or driver failure in this boot"));
    return r;
}

}  // namespace componentreports
