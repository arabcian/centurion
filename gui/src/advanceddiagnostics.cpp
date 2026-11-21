#include "advanceddiagnostics.h"

#include "eventinfo.h"
#include "sleepadvice.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

using diag::Level;
using diag::Report;
using diag::Table;

namespace advanceddiagnostics {

static QString tr(const char *s) { return QCoreApplication::translate("advanceddiagnostics", s); }
static QString num(double v, int prec = 1) { return QString::number(v, 'f', prec); }
/// A boot history state ("clean", "unclean", "running", "unknown") in words.
static QString stateName(const QString &s) {
    if (s == QLatin1String("clean")) return tr("ended cleanly");
    if (s == QLatin1String("unclean")) return tr("ended uncleanly");
    if (s == QLatin1String("running")) return tr("running now");
    return s.isEmpty() ? tr("unknown") : tr("end unknown");
}
static QString pct(double p) { return p < 0.001 ? QStringLiteral("<0.1 %") : QStringLiteral("%1 %").arg(p * 100, 0, 'f', 1); }

double upperTail(int k, int n, double p) {
    if (k <= 0) return 1.0;
    if (k > n || p <= 0) return 0.0;
    if (p >= 1) return 1.0;
    double sum = 0;
    for (int i = k; i <= n; ++i)
        sum += std::exp(std::lgamma(n + 1.0) - std::lgamma(i + 1.0) - std::lgamma(n - i + 1.0) + i * std::log(p) + (n - i) * std::log1p(-p));
    return std::min(1.0, sum);
}

// ── H01 Boot Regression Detective ─────────────────────────────────────────────

QJsonObject currentVersions() {
    QJsonObject v;
    auto put = [&v](const char *k, const QString &x) { if (!x.isEmpty()) v.insert(QLatin1String(k), x.left(80)); };
    put("kernel", diagsrc::read(QStringLiteral("/proc/sys/kernel/osrelease")));
    put("bios", diagsrc::read(QStringLiteral("/sys/class/dmi/id/bios_version")));
    put("nvidia", diagsrc::read(QStringLiteral("/sys/module/nvidia/version")));
    static const QRegularExpression mc(QStringLiteral(R"(^microcode\s*:\s*(\S+))"), QRegularExpression::MultilineOption);
    put("microcode", mc.match(diagsrc::read(QStringLiteral("/proc/cpuinfo"), 64 * 1024)).captured(1));
    return v;
}

QJsonObject legacyVersions() {
    return diagsrc::loadJson(diagsrc::dataDir() + QStringLiteral("/boot-regression-contexts.json")).value(QStringLiteral("boots")).toObject();
}

namespace {
struct FinishedBoot { QString id; qint64 start = 0; double hours = 0; bool unclean = false; double faults = 0; QJsonObject versions; };

// Hardware / driver faults of a boot: classified events that are not mere warnings (corrected MCE / AER).
double faultCount(const QJsonObject &events) {
    double n = 0;
    for (auto it = events.begin(); it != events.end(); ++it)
        if (!it.key().endsWith(QStringLiteral(":warn"))) n += it.value().toDouble();
    return n;
}

QList<FinishedBoot> finishedBoots(const QJsonArray &boots, const QJsonObject &legacy) {
    QList<FinishedBoot> out;
    for (const QJsonValue &v : boots) {
        const QJsonObject b = v.toObject();
        const QString state = b.value(QStringLiteral("state")).toString();
        if (state != QStringLiteral("clean") && state != QStringLiteral("unclean")) continue;
        FinishedBoot f;
        f.id = b.value(QStringLiteral("boot")).toString();
        f.start = qint64(b.value(QStringLiteral("start")).toDouble());
        f.unclean = state == QStringLiteral("unclean");
        // An unclean boot is known alive only to its last record: at least a quarter hour, as field stability counts it.
        f.hours = std::max(b.value(QStringLiteral("duration")).toDouble() / 3600.0, f.unclean ? 0.25 : 0.0);
        if (f.hours <= 0) continue;
        f.faults = f.unclean ? 0 : faultCount(b.value(QStringLiteral("events")).toObject());
        f.versions = b.value(QStringLiteral("versions")).toObject();
        if (f.versions.isEmpty()) f.versions = legacy.value(f.id).toObject();
        out << f;
    }
    std::sort(out.begin(), out.end(), [](const FinishedBoot &a, const FinishedBoot &b) { return a.start < b.start; });
    return out;
}
}  // namespace

Report bootRegression(const QJsonArray &boots, const QJsonObject &legacy, const QJsonObject &current) {
    Report r;
    r.code = QStringLiteral("H01");
    r.title = tr("Boot Regression Detective");
    r.about = tr("Groups finished boots by the kernel, NVIDIA driver, BIOS and CPU microcode they ran, and checks whether a "
                 "version change was followed by more unclean shutdowns or hardware faults.");
    r.limits << tr("Only boots the boot guard closed, or found unclean at the next start, are counted.")
             << tr("Faults (Xid, GSP, machine checks, uncorrectable AER, lockups…) are known only for clean boots: an unclean boot never saved its counts.")
             << tr("A rise after an update is a timing link, not proof: a new curve, game or room temperature can change at the same time.");
    QStringList now;
    for (const char *k : {"kernel", "nvidia", "bios", "microcode"})
        if (current.contains(QLatin1String(k))) now << QStringLiteral("%1 %2").arg(QLatin1String(k), current.value(QLatin1String(k)).toString());
    if (!now.isEmpty()) r.add(Level::Info, tr("This boot runs %1").arg(now.join(QStringLiteral(" · "))));

    const QList<FinishedBoot> fin = finishedBoots(boots, legacy);
    const int withVersions = int(std::count_if(fin.begin(), fin.end(), [](const FinishedBoot &f) { return !f.versions.isEmpty(); }));
    if (fin.size() < 2) {
        r.add(Level::Unknown, tr("Fewer than two finished boots are recorded"),
              tr("Every boot that ends (cleanly or not) is added by centurion-boot-guard. Compare again after a few more boots."));
        return r;
    }
    if (withVersions == 0) {
        r.add(Level::Unknown, tr("No boot has its versions recorded yet"),
              tr("The boot guard records the kernel, BIOS, microcode and NVIDIA driver of each boot from this release on. "
                 "Boots before it cannot be grouped."), tr("Come back after a few boots, or after the next kernel or driver update."));
        return r;
    }

    struct Group { QString version; int boots = 0, unclean = 0; double hours = 0, cleanHours = 0, faults = 0; qint64 first = 0, last = 0; };
    const QList<std::pair<const char *, QString>> fields = {
        {"kernel", tr("Kernel")}, {"nvidia", tr("NVIDIA driver")}, {"bios", tr("BIOS")}, {"microcode", tr("CPU microcode")}};
    bool compared = false, flagged = false;
    for (const auto &[key, label] : fields) {
        QMap<QString, Group> by;
        for (const FinishedBoot &f : fin) {
            const QString v = f.versions.value(QLatin1String(key)).toString();
            if (v.isEmpty()) continue;
            Group &g = by[v];
            g.version = v;
            if (!g.first) g.first = f.start;
            g.last = f.start;
            ++g.boots;
            g.hours += f.hours;
            if (f.unclean) ++g.unclean; else { g.cleanHours += f.hours; g.faults += f.faults; }
        }
        if (by.isEmpty()) continue;
        QList<Group> groups = by.values();
        std::sort(groups.begin(), groups.end(), [](const Group &a, const Group &b) { return a.first < b.first; });
        Table t;
        t.title = label;
        t.headers = {tr("Version"), tr("Boots"), tr("Hours"), tr("Unclean ends"), tr("Faults / 10 h"), tr("Used")};
        auto addRow = [&t](const Group &g, Level l) {
            t.add({g.version, QString::number(g.boots), num(g.hours), QString::number(g.unclean),
                   g.cleanHours > 0 ? num(g.faults * 10 / g.cleanHours, 2) : QStringLiteral("—"),
                   diagsrc::when(g.first).left(10) + QStringLiteral(" … ") + diagsrc::when(g.last).left(10)}, l);
        };
        if (groups.size() < 2) {
            addRow(groups.first(), Level::Info);
            r.tables << t;
            continue;
        }
        compared = true;
        const Group &nw = groups.last(), &old = groups.at(groups.size() - 2);
        Level rowLevel = Level::Info;
        // Unclean ends: are the new version's share of them more than its share of the hours explains?
        const int n = nw.unclean + old.unclean;
        const double p = upperTail(nw.unclean, n, nw.hours / (nw.hours + old.hours));
        if (nw.unclean >= 2 && p < 0.05) {
            flagged = true;
            rowLevel = nw.unclean >= 3 && p < 0.01 ? Level::Attention : Level::Review;
            r.add(rowLevel, tr("More unclean shutdowns since the %1 changed to %2").arg(label.toLower(), nw.version),
                  tr("%1 unclean ends in %2 h with %3, against %4 in %5 h with %6. The chance of a split this uneven by luck: %7.")
                      .arg(nw.unclean).arg(num(nw.hours)).arg(nw.version).arg(old.unclean).arg(num(old.hours)).arg(old.version).arg(pct(p)),
                  tr("Open Boot history for those boots. If a curve or undervolt was active, test at stock first; booting the previous version confirms or clears it."));
        }
        const double pf = upperTail(int(nw.faults), int(nw.faults + old.faults), nw.cleanHours / std::max(1e-9, nw.cleanHours + old.cleanHours));
        if (nw.cleanHours > 0 && old.cleanHours > 0 && nw.faults >= 3 && pf < 0.05) {
            flagged = true;
            if (rowLevel == Level::Info) rowLevel = Level::Review;
            r.add(Level::Review, tr("More hardware faults since the %1 changed to %2").arg(label.toLower(), nw.version),
                  tr("%1 faults in %2 clean hours with %3, against %4 in %5 h with %6 (chance by luck: %7).")
                      .arg(nw.faults).arg(num(nw.cleanHours)).arg(nw.version).arg(old.faults).arg(num(old.cleanHours)).arg(old.version).arg(pct(pf)),
                  tr("Events & alerts lists them by kind; Curve stability shows whether they follow a curve rather than the update."));
        }
        for (int i = 0; i < groups.size(); ++i) addRow(groups[i], i == groups.size() - 1 ? rowLevel : Level::Info);
        r.tables << t;
    }
    if (compared && !flagged)
        r.add(Level::Ok, tr("No version change was followed by a clear rise in unclean shutdowns or faults"),
              tr("%1 finished boots compared, %2 of them with recorded versions.").arg(fin.size()).arg(withVersions));
    else if (!compared)
        r.add(Level::Info, tr("Every recorded boot ran the same versions"),
              tr("Nothing to compare yet: the comparison starts after the first kernel, driver, BIOS or microcode change."));
    if (withVersions < fin.size())
        r.limits << tr("%1 of %2 finished boots have no recorded versions (from before this release).").arg(fin.size() - withVersions).arg(fin.size());
    return r;
}

// ── H06 NVMe Deep Health ──────────────────────────────────────────────────────

QString nvmeStatus(quint64 sf, bool *media) {
    const int sc = int(sf & 0xff), sct = int((sf >> 8) & 0x7);
    if (media) *media = sct == 2 && sc >= 0x80 && sc <= 0x85;
    static const QHash<int, const char *> generic = {
        {0x01, QT_TRANSLATE_NOOP("advanceddiagnostics", "Invalid Command Opcode")}, {0x02, QT_TRANSLATE_NOOP("advanceddiagnostics", "Invalid Field in Command")},
        {0x04, QT_TRANSLATE_NOOP("advanceddiagnostics", "Data Transfer Error")}, {0x05, QT_TRANSLATE_NOOP("advanceddiagnostics", "Aborted due to Power Loss")},
        {0x06, QT_TRANSLATE_NOOP("advanceddiagnostics", "Internal Error")}, {0x07, QT_TRANSLATE_NOOP("advanceddiagnostics", "Command Abort Requested")},
        {0x0b, QT_TRANSLATE_NOOP("advanceddiagnostics", "Invalid Namespace or Format")}, {0x0c, QT_TRANSLATE_NOOP("advanceddiagnostics", "Command Sequence Error")},
        {0x80, QT_TRANSLATE_NOOP("advanceddiagnostics", "LBA Out of Range")}, {0x82, QT_TRANSLATE_NOOP("advanceddiagnostics", "Namespace Not Ready")}};
    static const QHash<int, const char *> specific = {
        {0x02, QT_TRANSLATE_NOOP("advanceddiagnostics", "Invalid Queue Size")}, {0x06, QT_TRANSLATE_NOOP("advanceddiagnostics", "Invalid Firmware Slot")},
        {0x09, QT_TRANSLATE_NOOP("advanceddiagnostics", "Invalid Log Page")}, {0x0a, QT_TRANSLATE_NOOP("advanceddiagnostics", "Invalid Format")},
        {0x0d, QT_TRANSLATE_NOOP("advanceddiagnostics", "Feature Not Saveable")}, {0x0e, QT_TRANSLATE_NOOP("advanceddiagnostics", "Feature Not Changeable")},
        {0x0f, QT_TRANSLATE_NOOP("advanceddiagnostics", "Feature Not Namespace Specific")}};
    static const QHash<int, const char *> mediaErr = {
        {0x80, QT_TRANSLATE_NOOP("advanceddiagnostics", "Write Fault")}, {0x81, QT_TRANSLATE_NOOP("advanceddiagnostics", "Unrecovered Read Error")},
        {0x82, QT_TRANSLATE_NOOP("advanceddiagnostics", "End-to-end Guard Check Error")}, {0x83, QT_TRANSLATE_NOOP("advanceddiagnostics", "Application Tag Check Error")},
        {0x84, QT_TRANSLATE_NOOP("advanceddiagnostics", "Reference Tag Check Error")}, {0x85, QT_TRANSLATE_NOOP("advanceddiagnostics", "Compare Failure")},
        {0x86, QT_TRANSLATE_NOOP("advanceddiagnostics", "Access Denied")}, {0x87, QT_TRANSLATE_NOOP("advanceddiagnostics", "Deallocated or Unwritten Block")}};
    const QHash<int, const char *> &table = sct == 0 ? generic : sct == 1 ? specific : mediaErr;
    if (sct <= 2 && table.contains(sc)) return tr(table.value(sc));
    return tr("status type %1, code 0x%2").arg(sct).arg(sc, 2, 16, QLatin1Char('0'));
}

Report nvmeHealth(const QJsonObject &nv) {
    Report r;
    r.code = QStringLiteral("H06");
    r.title = tr("NVMe Deep Health");
    r.about = tr("The drive's own SMART / Health log and Error Information Log, read with nvme-cli (read-only admin commands; no self-test, format or reset).");
    r.limits << tr("SMART counters are what the drive's firmware reports; some drives under-report media errors.")
             << tr("The error log keeps only the last entries (up to 16 are read here).");
    if (!nv.value(QStringLiteral("ok")).toBool()) {
        r.add(Level::Unknown, tr("The NVMe logs could not be read"), nv.value(QStringLiteral("error")).toString(tr("helper unavailable or permission denied")));
        return r;
    }
    const QJsonArray ctrls = nv.value(QStringLiteral("controllers")).toArray();
    if (ctrls.isEmpty()) { r.add(Level::Unknown, tr("No NVMe controller found")); return r; }
    Table drives;
    drives.title = tr("Drives");
    drives.headers = {tr("Drive"), tr("Model"), tr("Temp"), tr("Life used"), tr("Spare"), tr("Media errors"),
                      tr("Error log"), tr("Unsafe shutdowns"), tr("Power-on"), tr("Written")};
    Table errors;
    errors.title = tr("Error log entries (newest first)");
    errors.headers = {tr("Drive"), tr("Error #"), tr("Status"), tr("Queue"), tr("LBA")};
    for (const QJsonValue &cv : ctrls) {
        const QJsonObject c = cv.toObject();
        const QString name = c.value(QStringLiteral("name")).toString();
        const QString model = c.value(QStringLiteral("model")).toString();
        const QString who = model.isEmpty() ? name : QStringLiteral("%1 (%2)").arg(name, model);
        const QJsonObject s = c.value(QStringLiteral("smart")).toObject();
        if (s.isEmpty()) {
            r.add(Level::Unknown, tr("%1: SMART log not read").arg(who), c.value(QStringLiteral("smart_status")).toString());
            drives.add({name, model, QStringLiteral("—"), QStringLiteral("—"), QStringLiteral("—"), QStringLiteral("—"),
                        QStringLiteral("—"), QStringLiteral("—"), QStringLiteral("—"), QStringLiteral("—")}, Level::Unknown);
            continue;
        }
        auto f = [&s](const char *k) { return s.contains(QLatin1String(k)) ? std::optional<double>(s.value(QLatin1String(k)).toDouble()) : std::nullopt; };
        auto show = [](std::optional<double> x, const QString &suffix = {}) { return x ? QString::number(*x) + suffix : QStringLiteral("—"); };
        Level worst = Level::Ok;
        auto raise = [&worst](Level l) { worst = std::max(worst, l); };
        const int cw = int(f("critical_warning").value_or(0));
        const QList<std::pair<int, QString>> bits = {
            {0x01, tr("spare capacity is below the drive's threshold")}, {0x02, tr("temperature is outside the drive's limits")},
            {0x04, tr("reliability is degraded by media or internal errors")}, {0x08, tr("the drive has switched itself to read-only")},
            {0x10, tr("the volatile memory backup has failed")}, {0x20, tr("the persistent memory region is read-only")}};
        for (const auto &[bit, what] : bits)
            if (cw & bit) {
                raise(Level::Attention);
                r.add(Level::Attention, tr("%1: critical warning — %2").arg(who, what),
                      tr("The drive itself raised this flag (SMART critical_warning bit 0x%1).").arg(bit, 2, 16, QLatin1Char('0')),
                      tr("Back up now. A drive that reports this usually needs replacing."));
            }
        const auto spare = f("spare_pct"), thr = f("spare_threshold_pct"), life = f("life_used_pct"), media = f("media_errors");
        if (spare && thr && *spare < *thr && !(cw & 0x01)) {
            raise(Level::Attention);
            r.add(Level::Attention, tr("%1: spare blocks below threshold (%2 % < %3 %)").arg(who).arg(*spare).arg(*thr));
        }
        if (media && *media > 0) {
            raise(Level::Review);
            r.add(Level::Review, tr("%1: %2 media error(s) in its lifetime").arg(who).arg(*media),
                  tr("Data the drive could not read or write cleanly at least once. A few old ones can stay constant for years; a growing count is the warning sign."),
                  tr("Watch the trend on Drive health; back up and run a long self-test (nvme device-self-test /dev/%1 -s 2).").arg(name));
        }
        if (life && *life >= 90) {
            raise(Level::Review);
            r.add(Level::Review, tr("%1: %2 % of the rated endurance used").arg(who).arg(*life),
                  tr("Drives usually keep working past 100 %; it is the manufacturer's write rating, not a failure date."),
                  tr("Keep backups current and plan a replacement."));
        }
        if (const auto crit = f("critical_temp_minutes"); crit && *crit > 0) {
            raise(Level::Review);
            r.add(Level::Review, tr("%1: %2 minute(s) above its critical temperature (lifetime)").arg(who).arg(*crit),
                  tr("Heat shortens flash life and can cause errors."), tr("Check its heatsink / airflow; Cooling shows the fan behaviour."));
        } else if (const auto warn = f("warning_temp_minutes"); warn && *warn > 60) {
            r.add(Level::Info, tr("%1: %2 minute(s) above its warning temperature (lifetime)").arg(who).arg(*warn));
        }
        if (const auto heavy = f("throttle_heavy"); heavy && *heavy > 0)
            r.add(Level::Info, tr("%1 throttled hard %2 time(s) to cool down (lifetime)").arg(who).arg(*heavy));
        const auto unsafe = f("unsafe_shutdowns"), cycles = f("power_cycles");
        if (unsafe && cycles && *cycles >= 20 && *unsafe / *cycles > 0.2)
            r.add(Level::Info, tr("%1: %2 of %3 power cycles were unsafe shutdowns").arg(who).arg(*unsafe).arg(*cycles),
                  tr("Power was cut without the drive being told (crashes, forced power-offs). Each one risks the most recent writes."));

        // Error log: rejected admin commands (a tool asking for an unsupported page) are not drive faults.
        const QJsonArray items = c.value(QStringLiteral("error_log")).toObject().value(QStringLiteral("items")).toArray();
        int harmless = 0, mediaHits = 0;
        for (const QJsonValue &ev : items) {
            const QJsonObject e = ev.toObject();
            bool isMedia = false;
            const QString st = e.contains(QStringLiteral("status_field")) ? nvmeStatus(quint64(e.value(QStringLiteral("status_field")).toDouble()), &isMedia) : tr("unknown");
            const bool admin = e.value(QStringLiteral("sqid")).toInt(-1) == 0;
            if (isMedia) ++mediaHits; else if (admin) ++harmless;
            errors.add({name, QString::number(qint64(e.value(QStringLiteral("error_count")).toDouble())), st,
                        admin ? tr("admin") : tr("I/O %1").arg(e.value(QStringLiteral("sqid")).toInt()),
                        e.contains(QStringLiteral("lba")) && isMedia ? QString::number(qint64(e.value(QStringLiteral("lba")).toDouble())) : QStringLiteral("—")},
                       isMedia ? Level::Attention : Level::Info);
        }
        if (mediaHits) {
            raise(Level::Attention);
            r.add(Level::Attention, tr("%1: the error log holds %2 media error(s)").arg(who).arg(mediaHits),
                  tr("Reads or writes that failed on the flash itself (see the table for the block address)."),
                  tr("Back up now and run a long self-test."));
        } else if (!items.isEmpty() && harmless == items.size()) {
            const qint64 total = qint64(f("error_log_entries").value_or(double(items.size())));
            r.add(Level::Info, tr("%1: its %2 logged errors are rejected admin commands").arg(who).arg(total),
                  tr("A tool (smartctl, nvme-cli, the firmware updater) asked for a feature or log page the drive does not support. "
                     "This raises the error count but says nothing about the drive's health."));
        } else if (!items.isEmpty()) {
            r.add(Level::Review, tr("%1: %2 recent error-log entries are failed I/O commands").arg(who).arg(items.size()),
                  tr("Not media errors, but I/O commands the drive did not complete normally."), tr("Check Events & alerts for NVMe timeouts or resets."));
        } else if (c.value(QStringLiteral("error_status")).toString().contains(QStringLiteral("success")) == false) {
            r.add(Level::Unknown, tr("%1: error log not read").arg(who), c.value(QStringLiteral("error_status")).toString());
        }
        if (worst == Level::Ok) r.add(Level::Ok, tr("%1: no warning in the SMART / Health log").arg(who));
        drives.add({name, model, show(f("temp_c"), QStringLiteral(" °C")), show(life, QStringLiteral(" %")),
                    spare ? show(spare, QStringLiteral(" %")) + (thr ? tr(" (min %1)").arg(*thr) : QString()) : QStringLiteral("—"),
                    show(media), show(f("error_log_entries")), show(unsafe),
                    f("power_on_hours") ? tr("%1 h").arg(*f("power_on_hours")) : QStringLiteral("—"), show(f("written_tb"), QStringLiteral(" TB"))}, worst);
    }
    r.tables << drives;
    if (!errors.rows.isEmpty()) r.tables << errors;
    r.raw = QString::fromUtf8(QJsonDocument(nv).toJson(QJsonDocument::Indented));
    return r;
}

// ── H10 USB Stability Watch ───────────────────────────────────────────────────

QHash<QString, QString> usbPresent() {
    QHash<QString, QString> out;
    for (const QString &d : diagsrc::dir(QStringLiteral("/sys/bus/usb/devices"))) {
        if (d.contains(QLatin1Char(':'))) continue;  // interfaces
        const QString b = QStringLiteral("/sys/bus/usb/devices/") + d + QLatin1Char('/');
        const QString product = diagsrc::read(b + QStringLiteral("product"), 200), maker = diagsrc::read(b + QStringLiteral("manufacturer"), 200);
        out.insert(d, (maker.isEmpty() || product.startsWith(maker) ? product : maker + QLatin1Char(' ') + product).trimmed());
    }
    return out;
}

static QString usbErrno(const QString &e) {
    if (e == QLatin1String("-71")) return tr("protocol error (-71): garbled signal — cable, connector, power or interference");
    if (e == QLatin1String("-110")) return tr("timeout (-110): the device did not answer");
    if (e == QLatin1String("-32")) return tr("stall (-32): the device refused the request");
    if (e == QLatin1String("-62")) return tr("timer expired (-62)");
    return e.isEmpty() ? QString() : tr("error %1").arg(e);
}

Report usbStability(const QList<diagsrc::LogLine> &kernel, const QHash<QString, QString> &present) {
    Report r;
    r.code = QStringLiteral("H10");
    r.title = tr("USB Stability Watch");
    r.about = tr("USB enumeration failures, port shutdowns and host-controller faults from the saved kernel log, grouped by port.");
    r.limits << tr("The kernel log is saved while Centurion runs; only warnings and errors are kept (normal connects and disconnects are not).")
             << tr("Port paths (1-2, 3-1.4) name a position on the USB tree; after a reboot the same path is usually the same physical port.");
    if (kernel.isEmpty()) {
        r.add(Level::Unknown, tr("No saved kernel log yet"), tr("Centurion saves kernel warnings while it runs (Health → Kernel log)."));
        return r;
    }
    struct Hit { QString port, type, detail; int count = 0; QSet<QString> boots; QString last; Level level = Level::Review; };
    QMap<QString, Hit> hits;
    static const QRegularExpression enumFail(QStringLiteral(R"(usb (\d+-[\d.]+): (?:device descriptor read/\w+, error (-\d+)|device not accepting address \d+, error (-\d+)|unable to enumerate USB device))"));
    static const QRegularExpression portFail(QStringLiteral(R"(usb usb(\d+)-port(\d+): (disabled by hub \(EMI\?\), re-enabling|over-current condition|Cannot enable|cannot reset|unable to enumerate))"),
                                             QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression hcDied(QStringLiteral(R"(xhci_hcd (\S+): (HC died|xHCI host controller not responding|Host halt failed|Host controller not halted))"),
                                           QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression xhciErr(QStringLiteral(R"(xhci_hcd (\S+): (ERROR .{0,80}|WARN .{0,80}))"));
    int scanned = 0;
    for (const diagsrc::LogLine &l : kernel) {
        ++scanned;
        auto note = [&](const QString &key, const QString &port, const QString &type, const QString &detail, Level lv) {
            Hit &h = hits[key];
            h.port = port; h.type = type; h.detail = detail;
            h.level = h.count ? std::max(h.level, lv) : lv;
            ++h.count; h.boots.insert(l.boot); h.last = l.time;
        };
        if (const auto m = enumFail.match(l.text); m.hasMatch()) {
            const QString e = m.captured(2).isEmpty() ? m.captured(3) : m.captured(2);
            note(QStringLiteral("enum|") + m.captured(1) + e, m.captured(1), tr("Enumeration failed"), usbErrno(e), Level::Review);
        } else if (const auto m = portFail.match(l.text); m.hasMatch()) {
            const QString what = m.captured(3).toLower();
            const QString port = m.captured(1) + QLatin1Char('-') + m.captured(2);
            if (what.startsWith(QStringLiteral("over-current")))
                note(QStringLiteral("oc|") + port, port, tr("Over-current"), tr("the port drew more current than allowed and was switched off"), Level::Attention);
            else if (what.startsWith(QStringLiteral("disabled by hub")))
                note(QStringLiteral("emi|") + port, port, tr("Port disabled (EMI?)"), tr("the hub saw electrical noise on the port and reset it"), Level::Review);
            else note(QStringLiteral("port|") + port, port, tr("Port reset failed"), m.captured(3), Level::Review);
        } else if (const auto m = hcDied.match(l.text); m.hasMatch()) {
            note(QStringLiteral("hc|") + m.captured(1), m.captured(1), tr("USB controller died"), m.captured(2), Level::Attention);
        } else if (const auto m = xhciErr.match(l.text); m.hasMatch()) {
            note(QStringLiteral("xhci|") + m.captured(1) + m.captured(2).left(30), m.captured(1), tr("xHCI driver warning"), m.captured(2), Level::Info);
        }
    }
    Table t;
    t.title = tr("USB problems by port");
    t.headers = {tr("Port"), tr("Device there now"), tr("Problem"), tr("Count"), tr("Boots"), tr("Last"), tr("Meaning")};
    QList<Hit> list = hits.values();
    std::sort(list.begin(), list.end(), [](const Hit &a, const Hit &b) { return a.level != b.level ? a.level > b.level : a.count > b.count; });
    for (const Hit &h : list) {
        const QString dev = present.value(h.port, h.port.contains(QLatin1Char(':')) ? QString() : tr("(nothing connected)"));
        t.add({h.port, dev, h.type, QString::number(h.count), QString::number(h.boots.size()), h.last, h.detail}, h.level);
        if (h.level == Level::Attention)
            r.add(Level::Attention, tr("%1 on %2 (%3×)").arg(h.type, h.port).arg(h.count), h.detail,
                  h.type == tr("Over-current") ? tr("Unplug what is on that port; a shorted cable or a device drawing too much power trips it.")
                                              : tr("Every device on that controller dropped off. Note what was connected; if it repeats, test without hubs and docks."));
        else if (h.level == Level::Review && h.count >= 3)
            r.add(Level::Review, tr("%1 on port %2: %3 time(s) in %4 boot(s)").arg(h.type, h.port).arg(h.count).arg(h.boots.size()),
                  h.detail + (dev.isEmpty() ? QString() : tr(" · now there: %1").arg(dev)),
                  tr("Try another cable and another port. Repeats on one port only point at the port or its hub; on every port, at the device."));
    }
    if (!list.isEmpty()) r.tables << t;
    if (!r.count(Level::Attention) && !r.count(Level::Review))
        r.add(Level::Ok, list.isEmpty() ? tr("No USB failures in %1 saved kernel-log lines").arg(scanned)
                                        : tr("Only isolated USB errors (fewer than 3 per port)"),
              tr("Single enumeration errors happen when a plug is inserted slowly or at an angle."));
    return r;
}

// ── H12 Sleep / Resume Failure Matrix ─────────────────────────────────────────

SleepInputs sleepInputsFromSystem() {
    SleepInputs in;
    for (const char *k : {"success", "fail", "failed_freeze", "failed_prepare", "failed_suspend", "failed_suspend_late", "failed_suspend_noirq",
                          "failed_resume", "failed_resume_early", "failed_resume_noirq", "last_failed_dev", "last_failed_errno", "last_failed_step",
                          "last_hw_sleep", "total_hw_sleep"}) {
        const QString v = diagsrc::read(QStringLiteral("/sys/power/suspend_stats/") + QLatin1String(k), 256);
        if (!v.isEmpty()) in.stats.insert(QLatin1String(k), v);
    }
    auto lines = [](const QString &path, int max) {
        QList<QJsonObject> out;
        for (const QString &l : diagsrc::read(path, 512 * 1024).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            const QJsonObject o = QJsonDocument::fromJson(l.toUtf8()).object();
            if (!o.isEmpty()) out << o;
        }
        return out.mid(std::max<qsizetype>(0, out.size() - max));
    };
    in.sleeps = lines(QStringLiteral("/var/log/centurion/sleep.log"), 400);
    in.died = lines(QStringLiteral("/var/log/centurion/sleep-failed.log"), 100);
    in.inflight = diagsrc::read(QStringLiteral("/var/lib/centurion/sleep-inflight"), 200);
    in.currentBoot = diagsrc::bootId();
    in.memSleep = diagsrc::read(QStringLiteral("/sys/power/mem_sleep"), 200);
    return in;
}

static QString errnoText(const QString &e) {
    const int n = std::abs(e.toInt());
    switch (n) {
    case 16: return tr("device busy (EBUSY): a driver refused to suspend");
    case 11: return tr("try again (EAGAIN): usually a wakeup event arrived while suspending — harmless when rare");
    case 5: return tr("I/O error (EIO) in a driver");
    case 110: return tr("timeout: a device did not finish suspending or resuming");
    case 19: return tr("no such device (ENODEV)");
    case 22: return tr("invalid argument (EINVAL) from a driver");
    case 0: return QString();
    }
    return tr("error -%1").arg(n);
}

Report sleepMatrix(const SleepInputs &in, const QJsonArray &boots) {
    Report r;
    r.code = QStringLiteral("H12");
    r.title = tr("Sleep / Resume Failure Matrix");
    r.about = tr("The kernel's own suspend counters, every recorded sleep per boot, and boots that went to sleep and never came back.");
    r.limits << tr("The kernel counters cover this boot only; earlier boots are seen through the sleep log.")
             << tr("A boot is recognised as dead in sleep only from this release's sleep hook on (it leaves a marker until the machine resumes).");
    const QString mode = QRegularExpression(QStringLiteral(R"(\[(\w+)\])")).match(in.memSleep).captured(1);

    // Kernel counters of this boot.
    const int ok = in.stats.value(QStringLiteral("success")).toString().toInt(), fail = in.stats.value(QStringLiteral("fail")).toString().toInt();
    if (in.stats.isEmpty()) {
        r.add(Level::Unknown, tr("Kernel suspend counters not available"), tr("/sys/power/suspend_stats is missing (an older kernel, or one without CONFIG_PM_SLEEP)."));
    } else {
        Table k;
        k.title = tr("This boot (kernel suspend_stats)");
        k.headers = {tr("Counter"), tr("Value")};
        for (auto it = in.stats.begin(); it != in.stats.end(); ++it) k.add({it.key(), it.value().toString()},
            it.key().startsWith(QStringLiteral("failed_")) && it.value().toString().toInt() > 0 ? Level::Review : Level::Info);
        r.tables << k;
        const int resumeFails = in.stats.value(QStringLiteral("failed_resume")).toString().toInt() + in.stats.value(QStringLiteral("failed_resume_early")).toString().toInt() +
                                in.stats.value(QStringLiteral("failed_resume_noirq")).toString().toInt();
        const QString dev = in.stats.value(QStringLiteral("last_failed_dev")).toString(), step = in.stats.value(QStringLiteral("last_failed_step")).toString();
        const QString err = errnoText(in.stats.value(QStringLiteral("last_failed_errno")).toString());
        const QString last = QStringList{dev.isEmpty() ? QString() : tr("device %1").arg(dev), step.isEmpty() ? QString() : tr("step %1").arg(step), err}
                                 .filter(QRegularExpression(QStringLiteral("."))).join(QStringLiteral(" · "));
        if (resumeFails > 0)
            r.add(Level::Attention, tr("%1 resume failure(s) this boot").arg(resumeFails),
                  last.isEmpty() ? tr("A device failed to resume.") : tr("Last failure: %1").arg(last),
                  tr("The device named is the first suspect: update its firmware / driver, or check Sleep & wake → wake sources."));
        else if (fail > 0)
            r.add(fail >= 3 ? Level::Review : Level::Info, tr("%1 of %2 attempts to sleep failed this boot").arg(fail).arg(ok + fail),
                  last.isEmpty() ? QString() : tr("Last failure: %1").arg(last),
                  tr("A failed suspend returns to the desktop at once; the device and step say what refused."));
    }

    // Boots that died asleep (or while waking).
    QList<QJsonObject> died = in.died;
    QStringList deadBoots;
    for (const QJsonObject &d : died) deadBoots << d.value(QStringLiteral("boot")).toString();
    // A marker of another boot that no later sleep has moved to the log yet.
    const QStringList inflight = in.inflight.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (inflight.size() >= 2 && inflight.at(0) != in.currentBoot && !deadBoots.contains(inflight.at(0))) {
        deadBoots << inflight.at(0);
        died << QJsonObject{{"boot", inflight.at(0)}, {"start", inflight.at(1).toDouble()}};
    } else if (inflight.size() >= 2 && inflight.at(0) == in.currentBoot) {
        r.add(Level::Info, tr("The last sleep of this boot has no resume record"),
              tr("The sleep hook's post step did not run (or is running now). That sleep is missing from the log."));
    }
    for (const QJsonObject &d : died) {
        const QString id = d.value(QStringLiteral("boot")).toString();
        QString state = tr("not in boot history");
        for (const QJsonValue &bv : boots)
            if (bv.toObject().value(QStringLiteral("boot")).toString() == id) state = stateName(bv.toObject().value(QStringLiteral("state")).toString());
        r.add(Level::Attention, tr("Boot %1 went to sleep at %2 and never resumed").arg(id.left(8), diagsrc::when(qint64(d.value(QStringLiteral("start")).toDouble()))),
              tr("The machine hung or lost power while asleep or while waking (boot history: %1). Nothing after that moment was logged.").arg(state),
              tr("If this repeats: try the other sleep mode, check the dGPU state on the way in (table below), and look at the kernel log of the next boot for pstore records."));
    }

    // The sleep log, per boot.
    struct Span { QString id; qint64 start = 0, end = 0; QString state; };
    QList<Span> spans;
    for (const QJsonValue &bv : boots) {
        const QJsonObject b = bv.toObject();
        Span s{b.value(QStringLiteral("boot")).toString(), qint64(b.value(QStringLiteral("start")).toDouble()), 0, b.value(QStringLiteral("state")).toString()};
        s.end = b.contains(QStringLiteral("end")) ? qint64(b.value(QStringLiteral("end")).toDouble())
                                                  : s.start + qint64(b.value(QStringLiteral("duration")).toDouble());
        spans << s;
    }
    std::sort(spans.begin(), spans.end(), [](const Span &a, const Span &b) { return a.start < b.start; });
    struct Row { int sleeps = 0, dgpuAwake = 0, idleHw = 0, unexpected = 0, failMax = 0; QList<qint64> lens; qint64 first = 0; QString state; };
    QMap<QString, Row> per;
    QStringList order;
    QList<sleepadvice::Sleep> parsed;
    for (const QJsonObject &o : in.sleeps) {
        const sleepadvice::Sleep s = sleepadvice::parse(o);
        if (s.start <= 0 || s.end < s.start) continue;
        parsed << s;
        QString id = tr("(unknown boot)");
        QString state;
        for (int i = 0; i < spans.size(); ++i) {
            const qint64 nextStart = i + 1 < spans.size() ? spans[i + 1].start : std::numeric_limits<qint64>::max();
            if (s.start >= spans[i].start && s.start < nextStart) { id = spans[i].id.left(8); state = spans[i].state; break; }
        }
        if (!per.contains(id)) order << id;
        Row &row = per[id];
        if (!row.first) row.first = s.start;
        row.state = state;
        ++row.sleeps;
        row.lens << s.end - s.start;
        if (o.value(QStringLiteral("dgpu_before")).toString() == QStringLiteral("active")) ++row.dgpuAwake;
        if (s.hasHwSleep && s.end - s.start >= sleepadvice::HW_IDLE_MIN_S && s.hwSleepUs / 1e6 / double(s.end - s.start) < sleepadvice::HW_IDLE_SHARE) ++row.idleHw;
        bool user = s.wokeBy.isEmpty();
        for (const QString &w : s.wokeBy) user = user || sleepadvice::isUserWake(w);
        if (!user) ++row.unexpected;
        row.failMax = std::max(row.failMax, o.value(QStringLiteral("fail_total")).toString().toInt());
    }
    if (parsed.isEmpty()) {
        r.add(Level::Unknown, tr("No sleep recorded in the sleep log"),
              tr("The sleep hook (centurion-sleep-log) writes one line per sleep; none is there yet, or the hook is not installed."));
    } else {
        Table t;
        t.title = tr("Sleeps per boot");
        t.headers = {tr("Boot"), tr("Started"), tr("Boot ended"), tr("Sleeps"), tr("Median length"), tr("dGPU awake on entry"),
                     tr("No HW sleep"), tr("Unexpected wakes"), tr("Failed attempts")};
        int dgpuTotal = 0;
        for (const QString &id : order) {
            Row &row = per[id];
            std::sort(row.lens.begin(), row.lens.end());
            dgpuTotal += row.dgpuAwake;
            const bool bad = deadBoots.contains(id) || std::any_of(deadBoots.begin(), deadBoots.end(), [&id](const QString &d) { return d.startsWith(id); });
            t.add({id, diagsrc::when(row.first).left(10), bad ? tr("died asleep") : stateName(row.state), QString::number(row.sleeps),
                   diagsrc::duration(row.lens.at(row.lens.size() / 2)), QString::number(row.dgpuAwake),
                   mode == QStringLiteral("s2idle") ? QString::number(row.idleHw) : QStringLiteral("—"),
                   QString::number(row.unexpected), QString::number(row.failMax)},
                  bad ? Level::Attention : row.failMax > 0 || row.idleHw > 0 ? Level::Review : Level::Ok);
        }
        r.tables << t;
        if (mode == QStringLiteral("s2idle") && sleepadvice::hwSleepIdle(parsed))
            r.add(Level::Review, tr("The last long sleeps barely reached the hardware sleep state"),
                  tr("With s2idle the platform must enter its low-power state by itself; when it does not, the battery drains almost as if awake."),
                  tr("Health → Sleep & wake → \"Why doesn't the hardware sleep?\" names the device that blocks it."));
        if (dgpuTotal * 2 > parsed.size())
            r.add(Level::Review, tr("The NVIDIA GPU was awake at the start of %1 of %2 sleeps").arg(dgpuTotal).arg(parsed.size()),
                  tr("A dGPU that is powered when sleep begins is the most common reason for a failed or power-hungry s2idle sleep on hybrid laptops."),
                  tr("Health → dGPU power shows what keeps it awake."));
        if (!r.count(Level::Attention) && !r.count(Level::Review))
            r.add(Level::Ok, tr("%1 recorded sleep(s); none failed and no boot died asleep").arg(parsed.size()));
    }
    if (!mode.isEmpty()) r.limits.prepend(tr("Sleep mode now: %1 (s2idle = modern standby, deep = S3).").arg(mode));
    return r;
}

// ── H13 CO Evidence Cross-Link ────────────────────────────────────────────────

CoRecords coRecordsFromSystem() {
    CoRecords co;
    const QString root = diagsrc::stateDir();
    const QString journal = root + QStringLiteral("/co-search-session.json"), guards = root + QStringLiteral("/co-search-crash-guards.json");
    co.sessionPresent = QFile::exists(journal);
    co.session = diagsrc::loadJson(journal);
    if (QFile::exists(guards)) {
        const QJsonObject g = diagsrc::loadJson(guards);
        co.guardsValid = g.value(QStringLiteral("version")).toInt() == 1 && g.value(QStringLiteral("guards")).isArray();
        co.guards = g.value(QStringLiteral("guards")).toArray();
    }
    co.history = diagsrc::loadJson(root + QStringLiteral("/co-search-runs.json")).value(QStringLiteral("history")).toArray();
    co.bios = diagsrc::read(QStringLiteral("/sys/class/dmi/id/bios_version"));
    return co;
}

Report coCrossLink(const CoRecords &co, const QJsonArray &boots, const QJsonObject &evidence) {
    Report r;
    r.code = QStringLiteral("H13");
    r.title = tr("CO Evidence Cross-Link");
    r.about = tr("Ryzen Curve → CO Search records side by side: the recovery journal, crash guards, past searches, and how often boots "
                 "ended uncleanly under each Curve Optimizer state. Read-only: no guard or curve is changed here.");
    r.limits << tr("An unclean end under a CO profile is a timing link; the crash watcher of CO Search is what blames a core.")
             << tr("Curve stability (Health) lays kernel events over curve intervals in more detail.");
    // Recovery journal.
    if (co.sessionPresent && (co.session.isEmpty() || co.session.value(QStringLiteral("version")).toInt() != 3)) {
        r.add(Level::Attention, tr("The CO Search recovery journal is unreadable"),
              tr("Its trial state is unknown, so the curve may not be at stock."), tr("Open Ryzen Curve → CO Search: it offers to reset to stock and discard the journal."));
    } else if (co.sessionPresent) {
        const int attempts = int(co.session.value(QStringLiteral("attempts")).toArray().size());
        if (co.session.value(QStringLiteral("pending")).toBool())
            r.add(Level::Review, tr("A CO Search was interrupted during a trial (%1)").arg(co.session.value(QStringLiteral("trial")).toObject().value(QStringLiteral("what")).toString()),
                  tr("%1 attempts recorded; reboots during the search: %2.").arg(attempts).arg(co.session.value(QStringLiteral("reboots")).toInt()),
                  tr("Open Ryzen Curve → CO Search to resume or discard it (discarding resets the curve to stock first)."));
        else
            r.add(Level::Info, tr("A CO Search session is in progress or was left between trials"), tr("%1 attempts recorded.").arg(attempts));
    }
    // Crash guards.
    if (!co.guardsValid) {
        r.add(Level::Attention, tr("The crash-guard file is unreadable"), tr("CO Search refuses to start until it is fixed, so no guarded value is retried."));
    } else if (!co.guards.isEmpty()) {
        Table g;
        g.title = tr("Crash guards (values at or below are never tried again)");
        g.headers = {tr("Core"), tr("Logical CPUs"), tr("Crashed at"), tr("BIOS then")};
        int otherBios = 0;
        for (const QJsonValue &v : co.guards) {
            const QJsonObject o = v.toObject();
            QStringList cpus;
            for (const QJsonValue &c : o.value(QStringLiteral("cpus")).toArray()) cpus << QString::number(c.toInt());
            const QString bios = o.value(QStringLiteral("bios")).toString();
            if (!bios.isEmpty() && !co.bios.isEmpty() && bios != co.bios) ++otherBios;
            g.add({QStringLiteral("CCD%1 / %2").arg(o.value(QStringLiteral("ccd")).toInt()).arg(o.value(QStringLiteral("slot")).toInt()),
                   cpus.join(QLatin1Char(',')), QString::number(o.value(QStringLiteral("failed")).toInt()), bios.isEmpty() ? QStringLiteral("—") : bios});
        }
        r.tables << g;
        r.add(Level::Info, tr("%1 core(s) have a crash guard").arg(co.guards.size()),
              otherBios ? tr("%1 of them were recorded under another BIOS; a new AGESA can change what a core holds, but the guards still apply.").arg(otherBios) : QString());
    }
    // Past searches.
    if (!co.history.isEmpty()) {
        Table h;
        h.title = tr("Past searches (newest first)");
        h.headers = {tr("When"), tr("Mode"), tr("Level"), tr("Result"), tr("Values"), tr("Note")};
        for (int i = int(co.history.size()) - 1; i >= 0 && h.rows.size() < 20; --i) {
            const QJsonObject o = co.history.at(i).toObject();
            QStringList vals;
            for (const QJsonValue &x : o.value(QStringLiteral("values")).toArray()) vals << QString::number(x.toInt());
            const bool complete = o.value(QStringLiteral("complete")).toBool(), reset = o.value(QStringLiteral("reset_ok")).toBool(true);
            h.add({diagsrc::when(qint64(o.value(QStringLiteral("t")).toDouble())),
                   o.value(QStringLiteral("mode")).toString() == QStringLiteral("per") ? tr("per-core") : tr("all-core"),
                   o.value(QStringLiteral("level")).toString(), complete ? tr("finished") : reset ? tr("stopped") : tr("stopped, reset FAILED"),
                   vals.join(QLatin1Char(' ')), o.value(QStringLiteral("notes")).toString().left(120)},
                  !reset ? Level::Attention : complete ? Level::Ok : Level::Info);
            if (!reset && i == int(co.history.size()) - 1)
                r.add(Level::Attention, tr("The last CO Search could not confirm the reset to stock"),
                      tr("The last trial value may still have been applied when it ended."), tr("Reboot, or press Reset in Ryzen Curve."));
        }
        r.tables << h;
    }
    // Unclean ends per CO state (the last state of each boot: what was in effect when it ended).
    struct G { int boots = 0, unclean = 0; double hours = 0; };
    QMap<QString, G> by;
    for (const QJsonValue &bv : boots) {
        const QJsonObject b = bv.toObject();
        const QString st = b.value(QStringLiteral("state")).toString();
        if (st != QStringLiteral("clean") && st != QStringLiteral("unclean")) continue;
        QString co_ = b.value(QStringLiteral("curves")).toObject().value(QStringLiteral("cpu_co")).toString().section(QStringLiteral(" → "), -1).trimmed();
        if (co_.isEmpty() || co_ == QStringLiteral("reset")) co_ = tr("stock");
        G &g = by[co_];
        ++g.boots;
        g.hours += std::max(b.value(QStringLiteral("duration")).toDouble() / 3600.0, st == QStringLiteral("unclean") ? 0.25 : 0.0);
        if (st == QStringLiteral("unclean")) ++g.unclean;
    }
    if (by.size() >= 2 || (by.size() == 1 && !by.contains(tr("stock")))) {
        Table t;
        t.title = tr("Boot endings by Curve Optimizer state at the end of the boot");
        t.headers = {tr("CO state"), tr("Boots"), tr("Hours"), tr("Unclean ends"), tr("Per 100 h")};
        const G stock = by.value(tr("stock"));
        for (auto it = by.cbegin(); it != by.cend(); ++it) {
            const G &g = it.value();
            Level l = Level::Info;
            if (it.key() != tr("stock") && g.unclean >= 2 && stock.hours > 0) {
                const double p = upperTail(g.unclean, g.unclean + stock.unclean, g.hours / (g.hours + stock.hours));
                if (p < 0.05) {
                    l = Level::Review;
                    r.add(Level::Review, tr("More unclean ends with CO state %1").arg(it.key()),
                          tr("%1 in %2 h, against %3 in %4 h at stock (chance by luck: %5).").arg(g.unclean).arg(num(g.hours)).arg(stock.unclean).arg(num(stock.hours)).arg(pct(p)),
                          tr("Verify that profile again (CO Search → Verify), or step it towards stock."));
                }
            }
            t.add({it.key(), QString::number(g.boots), num(g.hours), QString::number(g.unclean), g.hours > 0 ? num(g.unclean * 100 / g.hours) : QStringLiteral("—")}, l);
        }
        r.tables << t;
    }
    // Machine checks of this boot, for context.
    const QJsonArray mce = evidence.value(QStringLiteral("mce")).toArray();
    if (!mce.isEmpty())
        r.add(Level::Review, tr("%1 machine-check record(s) in this boot's kernel log").arg(mce.size()),
              tr("See Evidence Lab → H02 for the core and unit each names. Corrected errors on a core's own units under a CO offset are the classic sign of a value that is too low."));
    if (r.findings.isEmpty() || (!r.count(Level::Attention) && !r.count(Level::Review)))
        r.add(Level::Ok, tr("No interrupted search, unreadable record or excess of unclean ends under a CO profile"));
    return r;
}

// ── H15 Pre-crash Coverage Inspector ──────────────────────────────────────────

Report flightCoverage(const QJsonArray &boots) {
    Report r;
    r.code = QStringLiteral("H15");
    r.title = tr("Pre-crash Coverage Inspector");
    r.about = tr("How well the flight recorder covered the end of the previous boot, and what its last readings were.");
    r.limits << tr("The recorder keeps the last samples of the previous boot only (its ring); older boots have none.")
             << tr("The last sample is the last moment written to disk, not the moment of the crash: a hang can follow it by up to one interval.");
    bool found = false;
    for (const QJsonValue &v : boots) {
        const QJsonObject boot = v.toObject();
        const QJsonArray samples = boot.value(QStringLiteral("flight")).toArray();
        if (samples.isEmpty()) continue;
        found = true;
        const QString id = boot.value(QStringLiteral("boot")).toString().left(8), state = boot.value(QStringLiteral("state")).toString();
        qint64 first = 0, last = 0, prior = 0, maxGap = 0;
        int invalid = 0;
        QList<qint64> intervals;
        for (const QJsonValue &s : samples) {
            const qint64 t = qint64(s.toObject().value(QStringLiteral("t")).toDouble());
            if (t <= 0) { ++invalid; continue; }
            if (!first) first = t;
            if (prior && t > prior) intervals << t - prior;
            if (prior && t <= prior) ++invalid;
            prior = t;
            last = t;
        }
        // The ring header keeps the interval of its start; a changed setting shows only in the samples.
        const qint64 declared = qint64(boot.value(QStringLiteral("flight_interval")).toDouble());
        qint64 observed = 0;
        if (!intervals.isEmpty()) {
            QList<qint64> o = intervals;
            std::sort(o.begin(), o.end());
            observed = o.at(o.size() / 2);
        }
        const bool validDeclared = declared >= 1 && declared <= 120;
        const qint64 expected = std::max<qint64>(1, std::max(validDeclared ? declared : 0, observed));
        const qint64 threshold = std::max<qint64>(15, expected * 5 / 2);
        int gaps = 0;
        for (qint64 dt : intervals) { maxGap = std::max(maxGap, dt); if (dt > threshold) ++gaps; }
        Table t;
        t.title = tr("Boot %1 (%2): %3 samples, %4 → %5").arg(id, stateName(state)).arg(samples.size()).arg(diagsrc::when(first), diagsrc::when(last));
        t.headers = {tr("Time"), tr("CPU °C"), tr("GPU °C"), tr("CPU MHz"), tr("Package W"), tr("Battery W"), tr("Fans"), tr("Load"), tr("dGPU")};
        for (int i = std::max(0, int(samples.size()) - 6); i < samples.size(); ++i) {
            const QJsonObject o = samples.at(i).toObject();
            auto g = [&o](const char *k) { return o.contains(QLatin1String(k)) ? o.value(QLatin1String(k)).toVariant().toString() : QStringLiteral("—"); };
            QStringList fans;
            for (const QJsonValue &f : o.value(QStringLiteral("fan")).toArray()) fans << QString::number(f.toInt());
            t.add({QDateTime::fromSecsSinceEpoch(qint64(o.value(QStringLiteral("t")).toDouble())).toString(QStringLiteral("HH:mm:ss")),
                   g("cpu_c"), g("gpu_c"), g("mhz"), g("pkg_w"), g("bat_w"), fans.isEmpty() ? QStringLiteral("—") : fans.join(QLatin1Char('/')), g("load"), g("dgpu")});
        }
        t.note = tr("The last six samples. Longest gap %1 s; gaps over %2 s: %3; bad timestamps: %4.").arg(maxGap).arg(threshold).arg(gaps).arg(invalid);
        r.tables << t;
        if (state == QStringLiteral("unclean")) {
            const QJsonObject end = samples.last().toObject();
            const double cpu = end.value(QStringLiteral("cpu_c")).toDouble(), gpu = end.value(QStringLiteral("gpu_c")).toDouble();
            if (cpu >= 95)
                r.add(Level::Review, tr("Boot %1 ended at %2 °C CPU").arg(id).arg(cpu), tr("The last reading before the crash was at the CPU's thermal limit."),
                      tr("Check Cooling; a crash at the limit can be heat-related rather than a curve."));
            if (gpu >= 85)
                r.add(Level::Review, tr("Boot %1 ended with the GPU at %2 °C").arg(id).arg(gpu));
            if (gaps > 0 || invalid > 0)
                r.add(Level::Review, tr("The recording of boot %1 has %2 gap(s)").arg(id).arg(gaps + invalid),
                      tr("Missing samples before the crash: the disk may have stalled or the machine froze gradually."));
            else
                r.add(Level::Ok, tr("Boot %1 ended uncleanly; its recording is continuous to the last sample").arg(id),
                      tr("The machine stopped within about %1 s of %2.").arg(expected).arg(diagsrc::when(last)));
        } else {
            r.add(gaps ? Level::Info : Level::Ok, tr("Boot %1 %2; its recording %3").arg(id, stateName(state), gaps ? tr("has %1 gap(s)").arg(gaps) : tr("is continuous")));
        }
        if (validDeclared && observed > 0 && declared != observed)
            r.limits << tr("The recorder interval changed during boot %1 (ring header %2 s, samples %3 s): gaps are judged against the slower one.").arg(id).arg(declared).arg(observed);
    }
    if (!found) r.add(Level::Unknown, tr("No flight-recorder samples for the previous boot"),
                      tr("The recorder runs in centurion-boot-guard; it is off or the previous boot was too short."));
    return r;
}

// ── H16 GPU Memory Reliability ────────────────────────────────────────────────

QStringList nvidiaGpus() {
    QStringList out;
    for (const QString &id : diagsrc::dir(QStringLiteral("/sys/bus/pci/devices"))) {
        const QString b = QStringLiteral("/sys/bus/pci/devices/") + id + QLatin1Char('/');
        const QString cls = diagsrc::read(b + QStringLiteral("class"));
        if (diagsrc::read(b + QStringLiteral("vendor")) != QStringLiteral("0x10de") || !cls.startsWith(QStringLiteral("0x03"))) continue;
        out << QStringLiteral("%1 · %2 · %3").arg(id, diagsrc::pciName(QStringLiteral("10de"), diagsrc::read(b + QStringLiteral("device")), cls),
                                                 diagsrc::read(b + QStringLiteral("power/runtime_status")));
    }
    return out;
}

Report gpuMemory(const QJsonObject &evidence, const QStringList &gpus, const QString &active) {
    Report r;
    r.code = QStringLiteral("H16");
    r.title = tr("GPU Memory Reliability");
    r.about = tr("NVIDIA memory-related Xid events of this boot, and — only when you ask — the driver's ECC, row-remapping and page-retirement counters.");
    r.limits << tr("GeForce GPUs have no ECC on their GDDR memory: their counters read N/A, which says nothing about the memory's health.")
             << tr("The passive part never wakes the GPU; the active query (nvidia-smi) does.");
    if (gpus.isEmpty()) r.add(Level::Unknown, tr("No NVIDIA GPU found on the PCI bus"));
    // Memory-related Xids (codes as the driver documents them).
    static const QHash<int, const char *> memXid = {
        {48, QT_TRANSLATE_NOOP("advanceddiagnostics", "double-bit ECC error")}, {63, QT_TRANSLATE_NOOP("advanceddiagnostics", "ECC page retirement / row remap recorded")},
        {64, QT_TRANSLATE_NOOP("advanceddiagnostics", "ECC page retirement / row remap failed")}, {92, QT_TRANSLATE_NOOP("advanceddiagnostics", "high single-bit ECC error rate")},
        {94, QT_TRANSLATE_NOOP("advanceddiagnostics", "contained ECC error")}, {95, QT_TRANSLATE_NOOP("advanceddiagnostics", "uncontained ECC error")},
        {140, QT_TRANSLATE_NOOP("advanceddiagnostics", "unrecovered ECC error")}};
    QMap<int, int> counts;
    int other = 0;
    for (const QJsonValue &v : evidence.value(QStringLiteral("xid")).toArray()) {
        const int code = v.toObject().value(QStringLiteral("code")).toInt();
        if (memXid.contains(code)) ++counts[code]; else ++other;
    }
    if (evidence.isEmpty())
        r.add(Level::Unknown, tr("This boot's kernel events could not be scanned"), tr("Without them no memory Xid can be seen."));
    for (auto it = counts.cbegin(); it != counts.cend(); ++it)
        r.add(it.key() == 63 || it.key() == 92 ? Level::Review : Level::Attention, tr("Xid %1 × %2: %3").arg(it.key()).arg(it.value()).arg(tr(memXid.value(it.key()))),
              tr("The driver reported a GPU memory error."), tr("Run the active query below; at stock clocks, repeated memory Xids point at the card (RMA)."));
    if (!evidence.isEmpty() && counts.isEmpty())
        r.add(Level::Ok, tr("No memory-related Xid in this boot"), other ? tr("%1 other Xid event(s): see Evidence Lab → H04.").arg(other) : QString());
    Table g;
    g.title = tr("NVIDIA GPUs");
    g.headers = {tr("PCI address · name · runtime power")};
    for (const QString &s : gpus) g.add({s});
    r.tables << g;

    if (!active.isEmpty()) {
        // nvidia-smi -q layout: "Section" lines, then "    Key : Value" (indentation = depth).
        Table q;
        q.title = tr("Active query (nvidia-smi)");
        q.headers = {tr("Counter"), tr("Value")};
        QStringList path;
        QList<int> depth;
        bool any = false, ecc = false;
        for (const QString &line : active.split(QLatin1Char('\n'))) {
            const int ind = int(line.size() - line.trimmed().size());
            const QString s = line.trimmed();
            if (s.isEmpty()) continue;
            while (!depth.isEmpty() && depth.last() >= ind) { depth.removeLast(); path.removeLast(); }
            const int colon = s.indexOf(QStringLiteral(" : "));
            if (colon < 0) { path << s; depth << ind; continue; }
            const QString key = s.left(colon).trimmed(), val = s.mid(colon + 3).trimmed();
            const QString full = (path.mid(1) + QStringList{key}).join(QStringLiteral(" › "));
            const bool section = full.contains(QStringLiteral("ECC")) || full.contains(QStringLiteral("Remap")) || full.contains(QStringLiteral("Retire"));
            if (!section) continue;
            any = true;
            bool isNum = false;
            const double n = val.toDouble(&isNum);
            Level l = Level::Info;
            if (full.contains(QStringLiteral("Mode")) && val != QStringLiteral("N/A")) ecc = true;
            if (isNum && n > 0 && (key.contains(QStringLiteral("Uncorrectable")) || key.contains(QStringLiteral("Double Bit")))) l = Level::Attention;
            else if (val == QStringLiteral("Yes") && key.contains(QStringLiteral("Failure"))) l = Level::Attention;
            else if (val == QStringLiteral("Yes") && key.contains(QStringLiteral("Pending"))) l = Level::Review;
            else if (isNum && n > 0) l = Level::Review;
            q.add({full, val}, l);
            if (l == Level::Attention) r.add(Level::Attention, tr("%1 = %2").arg(full, val), tr("Memory errors the GPU could not correct."), tr("At stock clocks this is a hardware fault: contact the vendor."));
            else if (l == Level::Review && val == QStringLiteral("Yes")) r.add(Level::Review, tr("%1 = %2").arg(full, val), tr("A remap / retirement waits for the next GPU reset."), tr("Reboot to apply it."));
        }
        if (!any) r.add(Level::Unknown, tr("The active query returned no ECC / remap section"), active.left(300));
        else if (!ecc) r.add(Level::Info, tr("This GPU reports no ECC (N/A)"), tr("Normal for GeForce cards; memory faults show only as Xid events or artefacts."));
        r.tables << q;
        r.raw = active;
    }
    return r;
}

// ── H17 Incident Lifecycle ────────────────────────────────────────────────────

QStringList incidentStatuses() {
    return {QStringLiteral("New"), QStringLiteral("Investigating"), QStringLiteral("Monitoring"), QStringLiteral("Resolved"), QStringLiteral("Reopened")};
}

static QString notesPath() { return diagsrc::dataDir() + QStringLiteral("/incident-notes.json"); }
static bool validKey(const QString &k) { return QRegularExpression(QStringLiteral(R"(^[0-9a-f]{8}/[0-9]{1,20}$)")).match(k).hasMatch(); }

QJsonObject incidentNotes() { return diagsrc::loadJson(notesPath()).value(QStringLiteral("items")).toObject(); }

QJsonObject incidentNote(const QString &key) { return validKey(key) ? incidentNotes().value(key).toObject() : QJsonObject{}; }

QList<Incident> incidents(const QList<diagsrc::LogLine> &events, const QJsonObject &notes) {
    QList<Incident> out;
    QSet<QString> seen;
    for (auto it = events.crbegin(); it != events.crend() && out.size() < 400; ++it) {
        if (it->kind.isEmpty() || seen.contains(it->key())) continue;  // health.log lines only
        seen.insert(it->key());
        const QJsonObject n = notes.value(it->key()).toObject();
        out << Incident{*it, n.value(QStringLiteral("status")).toString(QStringLiteral("New")), n.value(QStringLiteral("note")).toString(),
                        n.value(QStringLiteral("edited")).toString()};
    }
    return out;
}

QString saveIncident(const QString &key, const QString &status, const QString &note) {
    if (!validKey(key)) return tr("Invalid event identity");
    if (!incidentStatuses().contains(status) || note.size() > 2000) return tr("Invalid status or note too long");
    const QString path = notesPath();
    QJsonObject all = diagsrc::loadJson(path);
    // An unreadable store is not overwritten: the notes of every other incident would be lost.
    if (QFile::exists(path) && all.isEmpty()) return tr("The note store is unreadable; not overwriting it");
    QJsonObject items = all.value(QStringLiteral("items")).toObject();
    if (items.size() >= 400 && !items.contains(key)) return tr("Note store full (400 incidents)");
    items.insert(key, QJsonObject{{"status", status}, {"note", note}, {"edited", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}});
    all.insert(QStringLiteral("schema"), 1);
    all.insert(QStringLiteral("items"), items);
    return diagsrc::saveJson(path, all);
}

// ── H18 Guided triage ─────────────────────────────────────────────────────────

QList<TriageCategory> triageCategories() {
    return {{QStringLiteral("gpu"), QT_TRANSLATE_NOOP("advanceddiagnostics", "NVIDIA GPU: Xid, GSP, black screens"), {"H04", "H16", "S04", "S06"}},
            {QStringLiteral("cpu"), QT_TRANSLATE_NOOP("advanceddiagnostics", "CPU: machine checks, Curve Optimizer"), {"H02", "H13", "H15"}},
            {QStringLiteral("crash"), QT_TRANSLATE_NOOP("advanceddiagnostics", "Freezes and crashes after an update"), {"H01", "H15", "H13"}},
            {QStringLiteral("storage"), QT_TRANSLATE_NOOP("advanceddiagnostics", "Storage: NVMe, F2FS"), {"H06", "H05", "H03"}},
            {QStringLiteral("radio"), QT_TRANSLATE_NOOP("advanceddiagnostics", "Wi-Fi and Bluetooth"), {"H09", "S07", "S18"}},
            {QStringLiteral("sleep"), QT_TRANSLATE_NOOP("advanceddiagnostics", "Sleep and resume"), {"H12", "H15"}},
            {QStringLiteral("usb"), QT_TRANSLATE_NOOP("advanceddiagnostics", "USB devices and ports"), {"H10", "S13", "H03"}}};
}

Report triage(const QString &label, const QList<Report> &checks) {
    Report r;
    r.code = QStringLiteral("H18");
    r.title = tr("Guided triage — %1").arg(label);
    QStringList codes;
    for (const Report &c : checks) codes << c.code;
    r.about = tr("Ran %1 and collected what needs a look. Each check's full report and its limits are on its own page.").arg(codes.join(QStringLiteral(", ")));
    Table t;
    t.title = tr("Checks run");
    t.headers = {tr("Check"), tr("Verdict"), tr("Summary")};
    for (const Report &c : checks) {
        t.add({c.code + QStringLiteral(" · ") + c.title, diag::levelName(c.verdict()), c.headline()}, c.verdict());
        for (const diag::Finding &f : c.findings)
            if (f.level == Level::Attention || f.level == Level::Review || f.level == Level::Unknown)
                r.findings << diag::Finding{f.level, c.code + QStringLiteral(" · ") + f.title, f.detail, f.action};
    }
    r.tables << t;
    if (!r.count(Level::Attention) && !r.count(Level::Review))
        r.add(Level::Ok, tr("None of the checks found a problem"),
              tr("If the symptom continues, note when it happens and save an incident note (H17); a pattern across boots is easier to pin down."));
    return r;
}

QString aerTotal(const QString &text, const QString &counter) {
    // aer_dev_* files hold several labelled lines ("Receiver Error 1\nTOTAL_ERR_COR 2"); kernels print the total with or without a colon.
    const QRegularExpression re(QStringLiteral("(?:^|\\n)\\s*") + QRegularExpression::escape(counter) + QStringLiteral(R"(\s*:?\s*([0-9]+)\s*(?:\n|$))"));
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch()) return {};
    bool ok = false;
    m.captured(1).toULongLong(&ok);
    return ok ? m.captured(1) : QString();
}

}  // namespace advanceddiagnostics
