#include "evidencereports.h"

#include "coevidence.h"
#include "eventinfo.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSet>

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

// ── H05 F2FS Integrity Guard ──────────────────────────────────────────────────

QList<F2fsVolume> f2fsVolumes(const QJsonObject &scan) {
    QList<F2fsVolume> out;
    for (const QJsonValue &v : scan.value(QStringLiteral("f2fs_mounts")).toArray()) {
        const QJsonObject m = v.toObject();
        F2fsVolume f{m.value(QStringLiteral("source")).toString(), m.value(QStringLiteral("mount")).toString(), m.value(QStringLiteral("options")).toString(), {}, {}};
        f.dev = QFileInfo(QFileInfo(f.source).canonicalFilePath()).fileName();
        if (f.dev.isEmpty()) f.dev = QFileInfo(f.source).fileName();
        const QString base = QStringLiteral("/sys/fs/f2fs/") + f.dev + QLatin1Char('/');
        for (const char *k : {"stat/sb_status", "stat/cp_status", "sb_status", "cp_status", "errors", "stop_reason", "free_segments",
                              "dirty_segments", "lifetime_write_kbytes", "unusable", "gc_urgent"}) {
            const QString x = diagsrc::read(base + QLatin1String(k), 4096);
            if (!x.isEmpty()) f.sys.insert(QString::fromLatin1(k).section(QLatin1Char('/'), -1), x);
        }
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

Report f2fsGuard(const QJsonObject &scan, const QList<F2fsVolume> &volumes, const QList<diagsrc::LogLine> &history) {
    Report r;
    r.code = QStringLiteral("H05");
    r.title = tr("F2FS Integrity Guard");
    r.about = tr("The flags F2FS itself keeps about each mounted volume (needs fsck, checkpoint error, recorded on-disk errors), and its integrity "
                 "errors in the kernel log of this and earlier boots. Nothing is repaired or remounted.");
    r.limits << tr("The kernel's status flags need a recent kernel (6.x); older ones show only the kernel log.")
             << tr("fsck.f2fs must run on an unmounted volume: for the root file system, from a live USB or at boot.");
    if (volumes.isEmpty()) r.add(Level::Info, tr("No F2FS volume is mounted"));
    Table t;
    t.title = tr("Mounted F2FS Volumes");
    t.headers = {tr("Volume"), tr("Mounted at"), tr("Needs fsck"), tr("Checkpoint"), tr("On-disk errors"), tr("Written (lifetime)")};
    for (const F2fsVolume &v : volumes) {
        quint64 sb = 0, cp = 0, err = 0;
        const bool hasSb = hexFlags(v.sys.value(QStringLiteral("sb_status")), &sb), hasCp = hexFlags(v.sys.value(QStringLiteral("cp_status")), &cp);
        const bool hasErr = hexValue(v.sys.value(QStringLiteral("errors")), &err);
        // f2fs.h: SBI_NEED_FSCK is bit 2 of the superblock flags; f2fs_fs.h: CP_ERROR_FLAG 0x8, CP_FSCK_FLAG 0x10.
        const bool needFsck = hasSb && (sb & 0x4), cpError = hasCp && (cp & 0x8), cpFsck = hasCp && (cp & 0x10);
        Level l = Level::Ok;
        if (needFsck || cpError || (hasErr && err)) l = Level::Attention;
        else if (cpFsck) l = Level::Review;
        else if (!hasSb && !hasCp) l = Level::Unknown;
        bool kb = false;
        const double gb = v.sys.value(QStringLiteral("lifetime_write_kbytes")).toDouble(&kb) / 1024 / 1024;
        t.add({v.source, v.mount, hasSb ? (needFsck ? tr("YES") : tr("no")) : QStringLiteral("—"),
               hasCp ? (cpError ? tr("error") : cpFsck ? tr("fsck requested") : tr("ok")) : QStringLiteral("—"),
               hasErr ? (err ? QStringLiteral("0x%1").arg(err, 0, 16) : tr("none")) : QStringLiteral("—"),
               kb ? tr("%1 GB").arg(gb, 0, 'f', 0) : QStringLiteral("—")}, l);
        if (needFsck || cpError)
            r.add(Level::Attention, tr("%1 (%2): F2FS has marked the volume as needing fsck").arg(v.source, v.mount),
                  cpError ? tr("The checkpoint carries the error flag: F2FS stopped trusting its metadata. It may go read-only.")
                          : tr("F2FS found an inconsistency and set its need-fsck flag."),
                  tr("Back up now. Then run fsck.f2fs -f on the unmounted volume (live USB for the root volume)."));
        else if (hasErr && err)
            r.add(Level::Attention, tr("%1: F2FS recorded on-disk errors (flags 0x%2)").arg(v.source).arg(err, 0, 16),
                  tr("Errors F2FS met while reading metadata are kept in the superblock until fsck clears them."),
                  tr("Back up, then fsck.f2fs -f on the unmounted volume."));
        else if (cpFsck)
            r.add(Level::Review, tr("%1: an fsck is requested at the next check").arg(v.source));
        if (v.sys.contains(QStringLiteral("stop_reason")) && v.sys.value(QStringLiteral("stop_reason")).remove(QRegularExpression(QStringLiteral("[0x ,\\n]"))).size())
            r.raw += tr("%1 stop_reason: %2\n").arg(v.source, v.sys.value(QStringLiteral("stop_reason")));
    }
    if (!volumes.isEmpty()) r.tables << t;
    // Kernel log: this boot's classified errors, then the saved ones of earlier boots.
    Table e;
    e.title = tr("F2FS Errors in the Kernel Log");
    e.headers = {tr("Boot"), tr("When"), tr("Message")};
    const QString boot = scan.value(QStringLiteral("boot_id")).toString().left(8);
    QSet<QString> seen;
    for (const QJsonValue &v : scan.value(QStringLiteral("f2fs_events")).toArray()) {
        e.add({boot, uptime(v.toObject().value(QStringLiteral("ts_us"))), v.toObject().value(QStringLiteral("text")).toString()}, Level::Attention);
        seen.insert(v.toObject().value(QStringLiteral("text")).toString());
    }
    QSet<QString> earlier;
    for (const diagsrc::LogLine &l : history) {
        if (l.kind != QStringLiteral("f2fs") || seen.contains(l.text)) continue;
        e.add({l.boot, l.time, l.text}, l.boot == boot ? Level::Attention : Level::Review);
        if (l.boot != boot) earlier.insert(l.boot);
    }
    if (!e.rows.isEmpty()) {
        r.tables << e;
        const int now = int(scan.value(QStringLiteral("f2fs_events")).toArray().size());
        if (now) r.add(Level::Attention, tr("%1 F2FS integrity error(s) in this boot").arg(now),
                       tr("I/O failures, corrupt metadata or a failed checkpoint."), tr("Back up, check the drive (H06, Drive health), then fsck the volume."));
        if (!earlier.isEmpty()) r.add(Level::Review, tr("F2FS errors in %1 earlier boot(s)").arg(earlier.size()),
                                      tr("If fsck has run since, these are history; if not, the volume may still be inconsistent."));
    }
    if (!r.count(Level::Attention) && !r.count(Level::Review) && !volumes.isEmpty())
        r.add(Level::Ok, tr("No F2FS flag or logged error"), tr("%1 volume(s) checked.").arg(volumes.size()));
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
