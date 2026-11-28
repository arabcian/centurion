#include "evidencereports.h"

#include "coevidence.h"
#include "eventinfo.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QStorageInfo>

#include <algorithm>

using diag::Level;
using diag::Report;
using diag::Table;

namespace evidencereports {

static QString tr(const char *s) { return QCoreApplication::translate("evidencereports", s); }
static QString uptime(const QJsonValue &ts) {
    if (!ts.isDouble()) return QStringLiteral("—");
    const qint64 s = qint64(ts.toDouble() / 1e6);
    return QStringLiteral("+%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
}

// ── H02 Ryzen MCE Core / CCD Atlas ────────────────────────────────────────────

static QString bankUnit(int bank) {
    // AMD SMCA on Zen client parts (coevidence.h): 0–6 are one core's own units; the rest is shared.
    switch (bank) {
    case 0: return tr("load/store");
    case 1: return tr("instruction fetch");
    case 2: return tr("L2 cache");
    case 3: return tr("decode");
    case 5: return tr("execution");
    case 6: return tr("floating point");
    case -1: return tr("unknown bank");
    }
    return bank < 7 ? tr("core unit %1").arg(bank) : tr("shared (L3 / memory / fabric), bank %1").arg(bank);
}

Report mceAtlas(const QJsonObject &scan, const QHash<int, CoreRef> &cores) {
    Report r;
    r.code = QStringLiteral("H02");
    r.title = tr("Ryzen MCE Core / CCD Atlas");
    r.about = tr("Machine checks of this boot, placed on the Curve Optimizer's own core map (CCD and core slot, as Ryzen Curve numbers them) "
                 "and sorted into a core's own units and shared ones.");
    r.limits << tr("Only this boot's kernel log; machine checks of a crashed boot are printed early in the next boot and appear there.")
             << tr("A shared unit (L3, memory controller, fabric) cannot blame one core: RAM, EXPO or the fabric clock are as likely.");
    struct Agg { QString core, cpu, unit; int ce = 0, ue = 0; bool own = false; QString last; };
    QMap<QString, Agg> by;
    QSet<QString> ownCores;
    int ue = 0, total = 0;
    for (const QJsonValue &v : scan.value(QStringLiteral("mce")).toArray()) {
        const QJsonObject e = v.toObject();
        const auto rec = coevidence::fromKernelText(e.value(QStringLiteral("text")).toString(), QStringLiteral("boot-kernel"));
        const int cpu = rec ? rec->cpu : e.value(QStringLiteral("cpu")).toInt(-1);
        const int bank = rec ? rec->bank : e.value(QStringLiteral("bank")).toInt(-1);
        const bool uncorrected = rec ? rec->uncorrected : e.value(QStringLiteral("severity")).toString() == QStringLiteral("critical");
        ++total;
        const CoreRef c = cores.value(cpu);
        const QString core = c.ccd >= 0 ? QStringLiteral("CCD%1 / core %2").arg(c.ccd).arg(c.slot) : (cpu >= 0 ? tr("not mapped") : tr("no CPU named"));
        const bool own = bank >= 0 && bank < 7;
        Agg &a = by[core + QLatin1Char('|') + QString::number(bank)];
        a.core = core; a.cpu = cpu >= 0 ? QString::number(cpu) : QStringLiteral("—"); a.unit = bankUnit(bank); a.own = own;
        (uncorrected ? a.ue : a.ce)++;
        a.last = uptime(e.value(QStringLiteral("ts_us")));
        if (uncorrected) ++ue;
        if (own && c.ccd >= 0) ownCores.insert(core);
    }
    if (!total) {
        r.add(Level::Ok, tr("No machine check in this boot's kernel log"));
    } else {
        Table t;
        t.title = tr("Machine Checks by Core and Unit");
        t.headers = {tr("Core"), tr("Logical CPU"), tr("Unit"), tr("Corrected"), tr("Uncorrected"), tr("Last (uptime)")};
        for (const Agg &a : by) t.add({a.core, a.cpu, a.unit, QString::number(a.ce), QString::number(a.ue), a.last},
                                     a.ue ? Level::Attention : a.own ? Level::Review : Level::Info);
        r.tables << t;
        if (ue)
            r.add(Level::Attention, tr("%1 uncorrected machine check(s)").arg(ue),
                  tr("The CPU could not repair the error; the affected program or the whole system may have crashed."),
                  tr("Undo recent Curve Optimizer / PBO / memory changes and test at stock (Ryzen Curve → Reset)."));
        if (ownCores.size() == 1) {
            const QString core = *ownCores.begin();
            r.add(Level::Review, tr("Errors on %1's own units").arg(core),
                  tr("Corrected errors in one core's load/store, L2 or execution units, and on no other core, are the classic sign of a "
                     "Curve Optimizer value that is too low for that core."),
                  tr("Ease that core by 2–3 CO steps, or run CO Search's verification on it."));
        } else if (ownCores.size() > 1) {
            r.add(Level::Review, tr("Core-unit errors on %1 cores").arg(ownCores.size()),
                  tr("Errors spread over several cores point at something they share (voltage, clocks, an all-core offset) rather than one weak core."));
        }
        const bool shared = std::any_of(by.begin(), by.end(), [](const Agg &a) { return !a.own; });
        if (shared)
            r.add(Level::Review, tr("Errors in shared units (L3, memory, fabric)"),
                  tr("These do not point at one core."), tr("Check memory settings (EXPO / timings) and run a memory test (Health → System tools → Memory test)."));
    }
    // The map itself, so a CPU number in any log can be placed.
    QMap<int, QPair<int, QStringList>> ccds;
    for (auto it = cores.cbegin(); it != cores.cend(); ++it) {
        ccds[it.value().ccd].first = it.value().l3Kib;
        ccds[it.value().ccd].second << QString::number(it.key());
    }
    if (!ccds.isEmpty()) {
        Table m;
        m.title = tr("Core Map (Logical CPUs per CCD)");
        m.headers = {tr("CCD"), tr("L3"), tr("Logical CPUs")};
        for (auto it = ccds.begin(); it != ccds.end(); ++it) {
            QStringList cpus = it.value().second;
            std::sort(cpus.begin(), cpus.end(), [](const QString &a, const QString &b) { return a.toInt() < b.toInt(); });
            m.add({QStringLiteral("CCD%1").arg(it.key()), it.value().first ? tr("%1 MB").arg(it.value().first / 1024) : QStringLiteral("—"), cpus.join(QLatin1Char(' '))});
        }
        m.note = tr("A CCD with more L3 than the other is the 3D V-Cache die.");
        r.tables << m;
    } else {
        r.limits << tr("The Curve Optimizer core map is not available on this CPU; logical CPU numbers are shown as the kernel gives them.");
    }
    QStringList raw;
    for (const QJsonValue &v : scan.value(QStringLiteral("mce")).toArray())
        raw << uptime(v.toObject().value(QStringLiteral("ts_us"))) + QStringLiteral("  ") + v.toObject().value(QStringLiteral("text")).toString();
    r.raw = raw.join(QLatin1Char('\n'));
    return r;
}

// ── H03 PCIe AER Delta Tracker ────────────────────────────────────────────────

QJsonObject aerSample(const QJsonObject &scan) {
    return {{"boot_id", scan.value(QStringLiteral("boot_id"))}, {"taken_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
            {"counters", scan.value(QStringLiteral("aer"))}};
}

Report aerDelta(const QJsonObject &scan, const QJsonObject &previous) {
    Report r;
    r.code = QStringLiteral("H03");
    r.title = tr("PCIe AER Delta Tracker");
    r.about = tr("PCIe Advanced Error Reporting counters of every device that has them, and how much each grew since the previous scan of this boot.");
    r.limits << tr("The counters count since boot; they say how many, not when.")
             << tr("A few corrected errors when links change power state (ASPM) are common and harmless.");
    const QString boot = scan.value(QStringLiteral("boot_id")).toString();
    const bool sameBoot = !boot.isEmpty() && previous.value(QStringLiteral("boot_id")).toString() == boot;
    QHash<QString, QJsonObject> before;
    if (sameBoot)
        for (const QJsonValue &v : previous.value(QStringLiteral("counters")).toArray()) before.insert(v.toObject().value(QStringLiteral("dev")).toString(), v.toObject());
    const QJsonArray now = scan.value(QStringLiteral("aer")).toArray();
    if (now.isEmpty()) { r.add(Level::Unknown, tr("No device exports AER counters"), tr("The kernel needs CONFIG_PCIEAER and the platform must grant AER to the OS.")); return r; }
    Table t;
    t.title = tr("Devices with Errors");
    t.headers = {tr("Device"), tr("Corrected"), tr("Since last scan"), tr("Non-fatal"), tr("Fatal")};
    int quiet = 0, unreadable = 0;
    for (const QJsonValue &v : now) {
        const QJsonObject a = v.toObject();
        const QString dev = a.value(QStringLiteral("dev")).toString();
        auto count = [](const QJsonValue &v) -> qint64 { return v.isDouble() ? v.toInteger(-1) : -1; };
        auto n = [&a, count](const char *k) { return count(a.value(QLatin1String(k))); };
        const qint64 cor = n("cor"), nonfatal = n("nonfatal"), fatal = n("fatal");
        const QJsonObject old = before.value(dev);
        const bool sameDevice = !old.isEmpty() && old.value(QStringLiteral("vendor")) == a.value(QStringLiteral("vendor")) &&
                                old.value(QStringLiteral("device")) == a.value(QStringLiteral("device")) &&
                                !a.value(QStringLiteral("vendor")).toString().isEmpty() && !a.value(QStringLiteral("device")).toString().isEmpty();
        QString delta = QStringLiteral("—");
        qint64 grew = 0;
        const qint64 then = count(old.value(QStringLiteral("cor")));
        if (sameDevice && then >= 0 && cor >= 0) {
            if (cor >= then) { grew = cor - then; delta = QStringLiteral("+%1").arg(grew); }
            else delta = tr("reset");
        } else if (!old.isEmpty() && !sameDevice) delta = tr("other device");
        const bool missing = cor < 0 || nonfatal < 0 || fatal < 0;
        if (missing) ++unreadable;
        if (cor == 0 && nonfatal == 0 && fatal == 0) { ++quiet; continue; }
        const QString cls = diagsrc::read(QStringLiteral("/sys/bus/pci/devices/%1/class").arg(dev));
        const QString name = QStringLiteral("%1 · %2").arg(dev, diagsrc::pciName(a.value(QStringLiteral("vendor")).toString(), a.value(QStringLiteral("device")).toString(), cls));
        auto s = [](qint64 x) { return x < 0 ? QStringLiteral("?") : QString::number(x); };
        const Level l = fatal > 0 || nonfatal > 0 ? Level::Attention : grew >= 10 || cor >= 100 ? Level::Review : missing ? Level::Unknown : Level::Info;
        t.add({name, s(cor), delta, s(nonfatal), s(fatal)}, l);
        if (fatal > 0 || nonfatal > 0)
            r.add(Level::Attention, tr("%1: uncorrectable PCIe errors (non-fatal %2, fatal %3)").arg(name).arg(s(nonfatal), s(fatal)),
                  tr("Transactions were lost on this link; the driver may have reset the device."),
                  tr("If it is the GPU or an NVMe drive: test at stock clocks, reseat if it is socketed, and check Events & alerts for resets."));
        else if (l == Level::Review)
            r.add(Level::Review, tr("%1: %2 corrected errors%3").arg(name).arg(cor).arg(grew ? tr(" (+%1 since the last scan)").arg(grew) : QString()),
                  tr("The link keeps retrying transfers: signal integrity (riser, slot, cable) or a link speed the hardware does not hold."),
                  tr("Health → System tools → PCIe Link Lab shows the link's speed and width."));
    }
    t.note = tr("%1 device(s) with zero counts are not listed.").arg(quiet);
    r.tables << t;
    if (!sameBoot) r.limits << tr("No earlier scan of this boot: deltas start with the next scan.");
    if (unreadable) r.add(Level::Unknown, tr("%1 device(s) have unavailable AER counters").arg(unreadable));
    if (!r.count(Level::Attention) && !r.count(Level::Review) && !r.count(Level::Unknown))
        r.add(Level::Ok, tr("No uncorrectable PCIe error or elevated corrected error count found"), tr("%1 device(s) checked.").arg(now.size()));
    return r;
}

// ── H04 NVIDIA Xid Case Analyzer ──────────────────────────────────────────────

static Level xidLevel(int code) {
    switch (code) {
    case 13: case 31: case 43: case 45: case 69: return Level::Info;   // an application's fault, usually
    case 79: case 119: case 120: case 143: case 48: case 94: case 95: case 140: case 64: case 62: case 61: return Level::Attention;
    default: return Level::Review;
    }
}

Report xidCases(const QJsonObject &scan) {
    Report r;
    r.code = QStringLiteral("H04");
    r.title = tr("NVIDIA Xid Case Analyzer");
    r.about = tr("Every NVIDIA Xid and GSP event of this boot grouped by code with what it means, and what came before each Xid 154 recovery.");
    r.limits << tr("Xid 154 is the driver's recovery action, not a cause; the events before it are a time link only.")
             << tr("This boot's kernel log only.");
    struct G { int code = 0; QString kind, title, gpu, first, last, text; int n = 0; };
    QMap<QString, G> by;
    for (const QJsonValue &v : scan.value(QStringLiteral("xid")).toArray()) {
        const QJsonObject e = v.toObject();
        const QString kind = e.value(QStringLiteral("kind")).toString();
        const int code = e.value(QStringLiteral("code")).toInt();
        G &g = by[kind + QString::number(code)];
        g.code = code; g.kind = kind; g.title = e.value(QStringLiteral("title")).toString(); g.gpu = e.value(QStringLiteral("pci")).toString();
        if (!g.n) g.first = uptime(e.value(QStringLiteral("ts_us")));
        g.last = uptime(e.value(QStringLiteral("ts_us")));
        g.text = e.value(QStringLiteral("text")).toString();
        ++g.n;
    }
    if (by.isEmpty()) {
        r.add(scan.contains(QStringLiteral("xid")) ? Level::Ok : Level::Unknown,
              scan.contains(QStringLiteral("xid")) ? tr("No NVIDIA Xid or GSP event in this boot") : tr("This helper did not return Xid events"));
    } else {
        Table t;
        t.title = tr("Xid Events by Code");
        t.headers = {tr("Code"), tr("Count"), tr("Meaning"), tr("GPU"), tr("First"), tr("Last")};
        for (const G &g : by) {
            const Level l = g.kind == QStringLiteral("gsp") ? Level::Attention : xidLevel(g.code);
            t.add({g.kind == QStringLiteral("gsp") ? QStringLiteral("GSP") : QString::number(g.code), QString::number(g.n), g.title, g.gpu.isEmpty() ? QStringLiteral("—") : g.gpu, g.first, g.last}, l);
            const eventinfo::Info info = eventinfo::explain(QJsonObject{{"kind", g.kind}, {"code", g.code}, {"title", g.title}, {"text", g.text}});
            if (g.code == 154) continue;  // shown as cases below
            r.add(l, tr("%1 × %2").arg(g.title).arg(g.n), info.meaning + (info.urgency.isEmpty() ? QString() : QStringLiteral(" ") + info.urgency),
                  info.steps.isEmpty() ? QString() : info.steps.first());
        }
        r.tables << t;
    }
    const QJsonArray cases = scan.value(QStringLiteral("xid_cases")).toArray();
    if (!cases.isEmpty()) {
        Table c;
        c.title = tr("Xid 154 Recoveries and What Preceded Them (5 min, same boot)");
        c.headers = {tr("When"), tr("GPU"), tr("Recovery"), tr("Before it")};
        for (const QJsonValue &v : cases) {
            const QJsonObject o = v.toObject();
            QStringList before;
            for (const QJsonValue &p : o.value(QStringLiteral("preceding")).toArray()) {
                const QJsonObject x = p.toObject();
                before << QStringLiteral("%1 %2%3").arg(uptime(x.value(QStringLiteral("ts_us"))), x.value(QStringLiteral("kind")).toString(),
                                                       x.value(QStringLiteral("code")).isDouble() ? QStringLiteral(" ") + QString::number(x.value(QStringLiteral("code")).toInt()) : QString());
            }
            QString action;
            for (const QJsonValue &x : scan.value(QStringLiteral("xid")).toArray())
                if (x.toObject().value(QStringLiteral("code")).toInt() == 154 && x.toObject().value(QStringLiteral("ts_us")) == o.value(QStringLiteral("ts_us")))
                    action = x.toObject().value(QStringLiteral("title")).toString().section(QStringLiteral("→"), 1).trimmed();
            c.add({uptime(o.value(QStringLiteral("ts_us"))), o.value(QStringLiteral("pci")).toString(), action.isEmpty() ? QStringLiteral("—") : action,
                   before.isEmpty() ? tr("nothing captured") : before.join(QStringLiteral(", "))}, Level::Attention);
        }
        r.tables << c;
        r.add(Level::Attention, tr("%1 Xid 154 recovery request(s)").arg(cases.size()),
              tr("The driver asked for a GPU reset, a driver reload or a reboot to recover. The events listed before it are the candidates for the cause."),
              tr("Save your work; do what the recovery action names. Repeats at stock clocks: driver bug or hardware."));
    }
    return r;
}

// ── H05 File System Integrity Guard ─────────────────────────────────────────

// The error state each file system keeps where the kernel shows it (sysfs / procfs). File systems that keep
// none (xfs, vfat, exfat, ntfs3, …) are judged by the kernel log alone.
static void readFsState(FsVolume &f) {
    auto put = [&f](const QString &base, std::initializer_list<const char *> keys) {
        for (const char *k : keys) {
            const QString x = diagsrc::read(base + QLatin1String(k), 4096);
            if (!x.isEmpty()) f.sys.insert(QString::fromLatin1(k).section(QLatin1Char('/'), -1), x);
        }
    };
    if (f.fstype == QStringLiteral("f2fs")) {
        put(QStringLiteral("/sys/fs/f2fs/") + f.dev + QLatin1Char('/'), {"stat/sb_status", "stat/cp_status", "sb_status", "cp_status", "errors", "stop_reason",
                                                                        "free_segments", "dirty_segments", "lifetime_write_kbytes", "unusable", "gc_urgent"});
    } else if (f.fstype.startsWith(QStringLiteral("ext"))) {
        // ext2 and ext3 are mounted by the ext4 driver on current kernels: the same directory.
        put(QStringLiteral("/sys/fs/ext4/") + f.dev + QLatin1Char('/'), {"errors_count", "first_error_time", "last_error_time", "first_error_func",
                                                                        "last_error_func", "lifetime_write_kbytes"});
    } else if (f.fstype == QStringLiteral("btrfs")) {
        // /sys/fs/btrfs/<fsid>/devices/<dev> names the file system a device belongs to; devinfo/<id>/error_stats
        // (kernel 5.14+) holds the counters "btrfs device stats" prints.
        for (const QString &fsid : diagsrc::dir(QStringLiteral("/sys/fs/btrfs"))) {
            const QString base = QStringLiteral("/sys/fs/btrfs/") + fsid;
            if (!QFileInfo::exists(base + QStringLiteral("/devices/") + f.dev)) continue;
            qint64 devices = 0, missing = 0;
            QHash<QString, qint64> sum;
            bool stats = false;
            for (const QString &id : diagsrc::dir(base + QStringLiteral("/devinfo"))) {
                ++devices;
                if (diagsrc::read(base + QStringLiteral("/devinfo/%1/missing").arg(id)) == QStringLiteral("1")) ++missing;
                const QString text = diagsrc::read(base + QStringLiteral("/devinfo/%1/error_stats").arg(id));
                for (const QString &line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                    bool ok = false;
                    const qint64 n = line.section(QLatin1Char(' '), -1).toLongLong(&ok);
                    if (ok) { sum[line.section(QLatin1Char(' '), 0, 0)] += n; stats = true; }
                }
            }
            f.sys.insert(QStringLiteral("devices"), QString::number(devices));
            f.sys.insert(QStringLiteral("missing"), QString::number(missing));
            if (stats) for (auto it = sum.cbegin(); it != sum.cend(); ++it) f.sys.insert(it.key(), QString::number(it.value()));
            break;
        }
    } else if (f.fstype == QStringLiteral("zfs")) {
        const QString state = diagsrc::read(QStringLiteral("/proc/spl/kstat/zfs/%1/state").arg(f.source.section(QLatin1Char('/'), 0, 0)));
        if (!state.isEmpty()) f.sys.insert(QStringLiteral("pool_state"), state);
    }
}

QList<FsVolume> fsVolumes(const QJsonObject &scan) {
    // One volume per device: btrfs subvolumes and bind mounts mount the same device several times.
    QList<FsVolume> out;
    QHash<QString, int> index;
    for (const QJsonValue &v : scan.value(QStringLiteral("fs_mounts")).toArray()) {
        const QJsonObject m = v.toObject();
        const QString source = m.value(QStringLiteral("source")).toString(), type = m.value(QStringLiteral("fstype")).toString();
        const QString key = type + QLatin1Char('|') + source;
        if (index.contains(key)) { out[index.value(key)].mounts << m.value(QStringLiteral("mount")).toString(); continue; }
        FsVolume f;
        f.source = source;
        f.fstype = type;
        f.options = m.value(QStringLiteral("options")).toString();
        f.mounts << m.value(QStringLiteral("mount")).toString();
        f.dev = QFileInfo(QFileInfo(f.source).canonicalFilePath()).fileName();
        if (f.dev.isEmpty()) f.dev = QFileInfo(f.source).fileName();
        readFsState(f);
        const QStorageInfo st(f.mounts.first());
        if (st.isValid() && st.isReady()) { f.total = st.bytesTotal(); f.avail = st.bytesAvailable(); }
        index.insert(key, int(out.size()));
        out << f;
    }
    return out;
}

static bool hexValue(const QString &s, quint64 *out) {
    bool ok = false;
    const QString t = s.trimmed();
    *out = t.startsWith(QStringLiteral("0x")) ? t.mid(2).toULongLong(&ok, 16) : t.toULongLong(&ok, 0);
    return ok;
}

// f2fs sysfs.c prints sb_status ("%lx") and cp_status ("%x") in hex without a 0x prefix: "42" is 0x42.
static bool hexFlags(const QString &s, quint64 *out) {
    bool ok = false;
    QString t = s.trimmed();
    if (t.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) t = t.mid(2);
    *out = t.toULongLong(&ok, 16);
    return ok && !t.isEmpty();
}

static QString size(qint64 bytes) {
    if (bytes < 0) return QStringLiteral("—");
    const double gb = bytes / 1e9;
    return gb >= 1000 ? tr("%1 TB").arg(gb / 1000, 0, 'f', 1) : tr("%1 GB").arg(gb, 0, 'f', gb < 10 ? 1 : 0);
}

// What one volume's own error state says: a short cell text, its level and (when there is one) a finding.
struct FsState { QString text; Level level = Level::Info; QString finding, detail, action; };

static FsState f2fsState(const FsVolume &v) {
    FsState s;
    quint64 sb = 0, cp = 0, err = 0;
    const bool hasSb = hexFlags(v.sys.value(QStringLiteral("sb_status")), &sb), hasCp = hexFlags(v.sys.value(QStringLiteral("cp_status")), &cp);
    const bool hasErr = hexValue(v.sys.value(QStringLiteral("errors")), &err);
    // f2fs.h: SBI_NEED_FSCK is bit 2 of the superblock flags; f2fs_fs.h: CP_ERROR_FLAG 0x8, CP_FSCK_FLAG 0x10.
    const bool needFsck = hasSb && (sb & 0x4), cpError = hasCp && (cp & 0x8), cpFsck = hasCp && (cp & 0x10);
    if (!hasSb && !hasCp) { s.text = tr("status flags not exposed"); s.level = Level::Unknown; return s; }
    if (needFsck || cpError) {
        s.level = Level::Attention;
        s.text = cpError ? tr("checkpoint error") : tr("needs fsck");
        s.finding = tr("F2FS has marked the volume as needing fsck");
        s.detail = cpError ? tr("The checkpoint carries the error flag: F2FS stopped trusting its metadata. It may go read-only.")
                           : tr("F2FS found an inconsistency and set its need-fsck flag.");
    } else if (hasErr && err) {
        s.level = Level::Attention;
        s.text = tr("on-disk errors 0x%1").arg(err, 0, 16);
        s.finding = tr("F2FS recorded on-disk errors (flags 0x%1)").arg(err, 0, 16);
        s.detail = tr("Errors F2FS met while reading metadata are kept in the superblock until fsck clears them.");
    } else if (cpFsck) {
        s.level = Level::Review;
        s.text = tr("fsck requested");
        s.finding = tr("an fsck is requested at the next check");
    } else {
        s.level = Level::Ok;
        s.text = tr("clean");
    }
    return s;
}

static FsState extState(const FsVolume &v) {
    FsState s;
    bool ok = false;
    const qint64 n = v.sys.value(QStringLiteral("errors_count")).toLongLong(&ok);
    if (!ok) { s.text = tr("error count not exposed"); s.level = Level::Unknown; return s; }
    if (!n) { s.text = tr("no errors recorded"); s.level = Level::Ok; return s; }
    s.level = Level::Attention;
    s.text = tr("%1 error(s) recorded").arg(n);
    s.finding = tr("%1 has recorded %2 error(s) since its last check").arg(v.fstype).arg(n);
    auto at = [&v](const char *func, const char *time) {
        const QString f = v.sys.value(QLatin1String(func));
        return f.isEmpty() ? QStringLiteral("—") : tr("%1 (%2)").arg(f, diagsrc::when(v.sys.value(QLatin1String(time)).toLongLong()));
    };
    s.detail = tr("The superblock keeps a count of the errors the file system met until e2fsck clears it. First: %1; last: %2.")
                   .arg(at("first_error_func", "first_error_time"), at("last_error_func", "last_error_time"));
    return s;
}

static FsState btrfsState(const FsVolume &v) {
    FsState s;
    if (!v.sys.contains(QStringLiteral("devices"))) { s.text = tr("device statistics not exposed"); s.level = Level::Unknown; return s; }
    const qint64 missing = v.sys.value(QStringLiteral("missing")).toLongLong();
    QStringList errs;
    for (const char *k : {"write_errs", "read_errs", "flush_errs", "corruption_errs", "generation_errs"})
        if (v.sys.value(QLatin1String(k)).toLongLong() > 0) errs << QStringLiteral("%1 %2").arg(QString::fromLatin1(k).section(QLatin1Char('_'), 0, 0), v.sys.value(QLatin1String(k)));
    if (missing > 0) {
        s.level = Level::Attention;
        s.text = tr("%1 device(s) missing").arg(missing);
        s.finding = tr("btrfs is running without %1 of its %2 device(s)").arg(missing).arg(v.sys.value(QStringLiteral("devices")));
        s.detail = tr("A missing device means no redundancy is left for the data it held; another failure loses data.");
    } else if (!errs.isEmpty()) {
        s.level = Level::Attention;
        s.text = errs.join(QStringLiteral(", "));
        s.finding = tr("btrfs device statistics show errors (%1)").arg(errs.join(QStringLiteral(", ")));
        s.detail = tr("btrfs counts every failed write, read and flush and every checksum (corruption) or generation mismatch it met, "
                      "until the counters are reset (btrfs device stats -z). Corruption on a healthy drive points at RAM.");
    } else if (!v.sys.contains(QStringLiteral("write_errs"))) {
        s.level = Level::Unknown;
        s.text = tr("no error counters (kernel older than 5.14)");
    } else {
        s.level = Level::Ok;
        s.text = tr("no device errors");
    }
    return s;
}

static FsState zfsState(const FsVolume &v) {
    FsState s;
    const QString st = v.sys.value(QStringLiteral("pool_state"));
    if (st.isEmpty()) { s.text = tr("pool state not exposed"); s.level = Level::Unknown; return s; }
    s.text = st;
    if (st == QStringLiteral("ONLINE")) { s.level = Level::Ok; return s; }
    s.level = Level::Attention;
    s.finding = tr("ZFS pool %1 is %2").arg(v.source.section(QLatin1Char('/'), 0, 0), st);
    s.detail = tr("A pool that is not ONLINE has lost a device or found errors it could not repair.");
    return s;
}

static FsState fsState(const FsVolume &v) {
    if (v.fstype == QStringLiteral("f2fs")) return f2fsState(v);
    if (v.fstype.startsWith(QStringLiteral("ext"))) return extState(v);
    if (v.fstype == QStringLiteral("btrfs")) return btrfsState(v);
    if (v.fstype == QStringLiteral("zfs")) return zfsState(v);
    return {tr("not kept (kernel log only)"), Level::Info, {}, {}, {}};
}

static bool readOnlyImage(const QString &t) {
    return t == QStringLiteral("squashfs") || t == QStringLiteral("erofs") || t == QStringLiteral("iso9660") || t == QStringLiteral("cramfs");
}

Report fsGuard(const QJsonObject &scan, const QList<FsVolume> &volumes, const QList<diagsrc::LogLine> &history) {
    Report r;
    r.code = QStringLiteral("H05");
    r.title = tr("File System Integrity Guard");
    r.about = tr("Every mounted disk file system (ext4, btrfs, XFS, F2FS, FAT, exFAT, NTFS, bcachefs, ZFS and the rest the kernel supports): "
                 "the error state the file system itself keeps, free space, and its integrity errors in the kernel log of this and earlier boots. "
                 "Nothing is repaired or remounted.");
    r.limits << tr("ext4, btrfs, F2FS and ZFS keep an error state the kernel shows; XFS, FAT, exFAT, NTFS and the others are judged by their kernel log messages alone.")
             << tr("Repair tools (fsck, xfs_repair) must run on an unmounted volume: for the root file system, from a live USB or at boot.")
             << tr("Network and in-memory file systems (NFS, tmpfs, overlay) are not disk volumes and are not checked.");
    QStringList supported;
    for (const QJsonValue &v : scan.value(QStringLiteral("fs_supported")).toArray()) supported << v.toString();
    if (volumes.isEmpty()) r.add(Level::Info, tr("No disk file system is mounted"));
    // This boot's logged errors, by kernel device, to tell a volume that went read-only after errors.
    const QString boot = scan.value(QStringLiteral("boot_id")).toString().left(8);
    QHash<QString, int> nowByDev;
    for (const QJsonValue &v : scan.value(QStringLiteral("fs_events")).toArray())
        ++nowByDev[eventinfo::fsOfMessage(v.toObject().value(QStringLiteral("text")).toString()).dev];
    Table t;
    t.title = tr("Mounted Disk File Systems");
    t.headers = {tr("Volume"), tr("Type"), tr("Mounted at"), tr("Mode"), tr("Free"), tr("File system's own state"), tr("Written (lifetime)")};
    t.privateColumns = {2};
    QSet<QString> types;
    for (const FsVolume &v : volumes) {
        types.insert(v.fstype);
        r.privatePaths << v.mounts;
        if (v.source.startsWith(QStringLiteral("/dev/disk/by-"))) r.privatePaths << v.source;
        const QString name = QStringLiteral("%1 (%2)").arg(v.source, v.mounts.first());
        const bool ro = v.options.section(QLatin1Char(','), 0, 0) == QStringLiteral("ro");
        FsState s = fsState(v);
        if (!s.finding.isEmpty())
            r.add(s.level, tr("%1: %2").arg(name, s.finding), s.detail, s.level == Level::Attention ? eventinfo::fsRepairAction(v.fstype) : QString());
        if (ro && !readOnlyImage(v.fstype) && nowByDev.value(v.dev)) {
            s.level = Level::Attention;
            r.add(Level::Attention, tr("%1 is read-only after %2 error(s) in this boot").arg(name).arg(nowByDev.value(v.dev)),
                  tr("File systems switch to read-only when they stop trusting their metadata, so that damage does not spread. Writes fail until it is repaired."),
                  eventinfo::fsRepairAction(v.fstype));
        }
        QString free = QStringLiteral("—");
        if (v.total > 0 && v.avail >= 0) {
            const double pct = 100.0 * double(v.avail) / double(v.total);
            free = tr("%1 (%2%)").arg(size(v.avail)).arg(pct, 0, 'f', 0);
            if (pct < 5 && !ro && !readOnlyImage(v.fstype)) {
                if (s.level < Level::Review) s.level = Level::Review;
                r.add(Level::Review, tr("%1 is %2% full").arg(name).arg(100 - pct, 0, 'f', 0),
                      tr("Nearly full file systems fail writes, and copy-on-write and log-structured ones (btrfs, F2FS, ZFS, bcachefs) slow down or stop well before 100%."),
                      tr("Free some space; on btrfs also check snapshots (they keep deleted data)."));
            }
        }
        bool kb = false;
        const double written = v.sys.value(QStringLiteral("lifetime_write_kbytes")).toDouble(&kb) * 1024;
        t.add({v.source, v.fstype, v.mounts.join(QStringLiteral(", ")), ro ? tr("read-only") : tr("read-write"), free, s.text,
               kb ? size(qint64(written)) : QStringLiteral("—")}, s.level);
        if (v.sys.contains(QStringLiteral("stop_reason")) && v.sys.value(QStringLiteral("stop_reason")).remove(QRegularExpression(QStringLiteral("[0x ,\\n]"))).size())
            r.raw += tr("%1 stop_reason: %2\n").arg(v.source, v.sys.value(QStringLiteral("stop_reason")));
    }
    if (!supported.isEmpty()) t.note = tr("Disk file systems this kernel supports: %1.").arg(supported.join(QStringLiteral(", ")));
    if (!volumes.isEmpty()) r.tables << t;
    // Kernel log: this boot's classified errors, then the saved ones of earlier boots.
    Table e;
    e.title = tr("File System Errors in the Kernel Log");
    e.headers = {tr("Boot"), tr("When"), tr("Type"), tr("Device"), tr("Message")};
    e.privateColumns = {0};
    struct Group { QString fstype, dev; int n = 0; };
    QMap<QString, Group> now;
    QSet<QString> seen;
    for (const QJsonValue &v : scan.value(QStringLiteral("fs_events")).toArray()) {
        const QString text = v.toObject().value(QStringLiteral("text")).toString();
        const eventinfo::FsRef fs = eventinfo::fsOfMessage(text);
        e.add({boot, uptime(v.toObject().value(QStringLiteral("ts_us"))), fs.fstype.isEmpty() ? QStringLiteral("—") : fs.fstype,
               fs.dev.isEmpty() ? QStringLiteral("—") : fs.dev, text}, Level::Attention);
        Group &g = now[fs.fstype + QLatin1Char('|') + fs.dev];
        g.fstype = fs.fstype; g.dev = fs.dev; ++g.n;
        seen.insert(text);
    }
    QSet<QString> earlier;
    for (const diagsrc::LogLine &l : history) {
        if ((l.kind != QStringLiteral("f2fs") && l.kind != QStringLiteral("fs")) || seen.contains(l.text)) continue;
        const eventinfo::FsRef fs = eventinfo::fsOfMessage(l.text);
        e.add({l.boot, l.time, fs.fstype.isEmpty() ? QStringLiteral("—") : fs.fstype, fs.dev.isEmpty() ? QStringLiteral("—") : fs.dev, l.text},
              l.boot == boot ? Level::Attention : Level::Review);
        if (l.boot != boot) earlier.insert(l.boot);
    }
    if (!e.rows.isEmpty()) {
        r.tables << e;
        for (const Group &g : now)
            r.add(Level::Attention, tr("%1 on %2: %3 integrity error(s) in this boot").arg(g.fstype.isEmpty() ? tr("A file system") : g.fstype,
                                                                                          g.dev.isEmpty() ? tr("an unnamed device") : g.dev).arg(g.n),
                  tr("I/O failures, corrupt metadata, a failed checkpoint or journal, or a forced shutdown of the file system."),
                  eventinfo::fsRepairAction(g.fstype) + QLatin1Char(' ') + tr("Check the drive too (Storage tab, Drive health)."));
        if (!earlier.isEmpty()) r.add(Level::Review, tr("File system errors in %1 earlier boot(s)").arg(earlier.size()),
                                      tr("If the volume has been checked since, these are history; if not, it may still be inconsistent."));
    }
    if (!r.count(Level::Attention) && !r.count(Level::Review) && !volumes.isEmpty()) {
        QStringList list = types.values();
        std::sort(list.begin(), list.end());
        r.add(Level::Ok, tr("No file system flag or logged error"), tr("%1 volume(s) checked (%2).").arg(volumes.size()).arg(list.join(QStringLiteral(", "))));
    }
    return r;
}

// ── H09 Wi-Fi / Bluetooth Failure Ledger ──────────────────────────────────────

Report radioLedger(const QJsonObject &scan, const QList<diagsrc::LogLine> &history) {
    Report r;
    r.code = QStringLiteral("H09");
    r.title = tr("Wi-Fi / Bluetooth Failure Ledger");
    r.about = tr("Wi-Fi and Bluetooth firmware load failures, firmware crashes and controller timeouts, per adapter, in this and earlier boots.");
    r.limits << tr("Normal reconnects and roaming are not failures and are not counted.")
             << tr("Messages differ by driver; failures a driver prints at info level are not seen.");
    struct G { QString device, kind; int load = 0, crash = 0, timeout = 0, other = 0; QSet<QString> boots; };
    QMap<QString, G> by;
    auto classify = [&by](const QString &device, const QString &kind, const QString &title, const QString &boot) {
        G &g = by[kind + QLatin1Char('|') + device];
        g.device = device; g.kind = kind; g.boots.insert(boot);
        const QString t = title.toLower();
        if (t.contains(QStringLiteral("load"))) ++g.load;
        else if (t.contains(QStringLiteral("timeout"))) ++g.timeout;
        else if (t.contains(QStringLiteral("crash")) || t.contains(QStringLiteral("reset"))) ++g.crash;
        else ++g.other;
    };
    const QString boot = scan.value(QStringLiteral("boot_id")).toString().left(8);
    QSet<QString> nowTexts;
    for (const QJsonValue &v : scan.value(QStringLiteral("radio_events")).toArray()) {
        const QJsonObject e = v.toObject();
        const QString text = e.value(QStringLiteral("text")).toString();
        nowTexts.insert(text);
        const QString kind = e.value(QStringLiteral("kind")).toString();
        QString dev = text.section(QLatin1Char(' '), 0, 1).remove(QLatin1Char(':'));
        if (kind == QStringLiteral("bluetooth")) {
            const auto m = QRegularExpression(QStringLiteral(R"((hci\d+))")).match(text);
            dev = m.hasMatch() ? m.captured(1) : tr("Bluetooth");
        }
        classify(dev, kind, e.value(QStringLiteral("title")).toString(), boot);
    }
    for (const diagsrc::LogLine &l : history) {
        if ((l.kind != QStringLiteral("wifi") && l.kind != QStringLiteral("bluetooth")) || l.boot == boot) continue;
        QString dev = l.text.section(QLatin1Char(' '), 0, 1).remove(QLatin1Char(':'));
        if (l.kind == QStringLiteral("bluetooth")) {
            const auto m = QRegularExpression(QStringLiteral(R"((hci\d+))")).match(l.text);
            dev = m.hasMatch() ? m.captured(1) : tr("Bluetooth");
        }
        classify(dev, l.kind, l.title, l.boot);
    }
    if (by.isEmpty()) {
        r.add(Level::Ok, tr("No Wi-Fi or Bluetooth failure in this boot or the saved history"));
        return r;
    }
    Table t;
    t.title = tr("Failures per Adapter");
    t.headers = {tr("Adapter"), tr("Type"), tr("Firmware load"), tr("Crash / reset"), tr("Timeout"), tr("Other"), tr("Boots")};
    for (const G &g : by) {
        const Level l = g.load ? Level::Attention : g.crash + g.timeout >= 3 ? Level::Review : Level::Info;
        t.add({g.device, g.kind == QStringLiteral("wifi") ? tr("Wi-Fi") : tr("Bluetooth"), QString::number(g.load), QString::number(g.crash),
               QString::number(g.timeout), QString::number(g.other), QString::number(g.boots.size())}, l);
        if (g.load)
            r.add(Level::Attention, tr("%1: firmware failed to load (%2×)").arg(g.device).arg(g.load),
                  tr("Without its firmware the adapter does not work at all in that boot."),
                  tr("Update the firmware package (Gentoo: sys-kernel/linux-firmware) and check that the file the kernel asked for exists (S18)."));
        else if (l == Level::Review)
            r.add(Level::Review, tr("%1: %2 firmware crash(es) / timeout(s) in %3 boot(s)").arg(g.device).arg(g.crash + g.timeout).arg(g.boots.size()),
                  tr("The adapter's firmware stops answering and the driver restarts it: drop-outs for the user."),
                  tr("A newer kernel or firmware often fixes it; disabling the adapter's power saving is the usual workaround."));
    }
    r.tables << t;
    if (!r.count(Level::Attention) && !r.count(Level::Review)) r.add(Level::Ok, tr("Only isolated radio errors"));
    return r;
}

}  // namespace evidencereports
