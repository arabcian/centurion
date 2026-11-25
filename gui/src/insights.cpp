#include "insights.h"
#ifndef CENTURION_HELPER_DIR
#define CENTURION_HELPER_DIR "/usr/libexec/centurion"
#endif
#include "autotunereport.h"
#include "diagnostichistory.h"
#include "diagsources.h"
#include <QDateTime>
#include <QMap>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QByteArray readFile(const QString &path, qint64 max = 4 * 1024 * 1024) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > max) return {};
    return f.read(max);
}
QString human(const QJsonValue &v) {
    if (v.isNull() || v.isUndefined()) return QStringLiteral("unknown");
    if (v.isString()) return v.toString();
    if (v.isBool()) return v.toBool() ? QStringLiteral("yes") : QStringLiteral("no");
    return QString::number(v.toDouble(), 'g', 8);
}
QString firstLine(const QString &p) { return QString::fromUtf8(readFile(p, 4096)).trimmed(); }
}

namespace insights {
void show(QWidget *parent, const QString &title, const QStringList &lines) {
    QDialog dlg(parent);
    dlg.setWindowTitle(title);
    dlg.resize(790, 570);
    auto *v = new QVBoxLayout(&dlg);
    auto *text = new QPlainTextEdit;
    text->setReadOnly(true);
    text->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    text->setPlainText(lines.join(QLatin1Char('\n')));
    v->addWidget(text);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(buttons);
    dlg.exec();
}

QStringList confidence(const QJsonObject &report) {
    QStringList out{QStringLiteral("CALIBRATION CONFIDENCE EXPLORER"),
        QStringLiteral("Evidence is not a stability guarantee; estimates and measured samples are distinct."), QString()};
    struct Entry { QString key, source, detail, recommendation; double uncertainty = 0; bool unknown = false; };
    QList<Entry> entries;
    for (const auto &v : report.value("decisions").toArray()) {
        const QJsonObject d = v.toObject();
        Entry e;
        e.key = d.value("key").toString();
        if (e.key.isEmpty()) continue;
        e.source = autotunereport::badge(d.value("source").toString("unknown"));
        e.detail = autotunereport::confidence(d);
        const QJsonArray ci = d.value("in_context").toArray().size() == 2
            ? d.value("in_context").toArray() : d.value("alone").toArray();
        if (ci.size() == 2) e.uncertainty = std::abs(ci.at(1).toDouble());
        const QString source = d.value("source").toString();
        const bool noSamples = d.value("n").toDouble() <= 0;
        e.unknown = (d.value("seen").isBool() && !d.value("seen").toBool()) ||
                    (noSamples && (source == "estimate" || source == "joint" || source == "measured"));
        e.recommendation = e.unknown ? QStringLiteral("PRIORITY: validate in this workload")
              : (ci.size() == 2 && e.uncertainty > std::abs(ci.at(0).toDouble())
                  ? QStringLiteral("uncertainty overlaps zero: test before trusting ranking")
                  : QStringLiteral("context-bound evidence; repeat after environment changes"));
        entries << e;
    }
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        if (a.unknown != b.unknown) return a.unknown;
        return a.uncertainty > b.uncertainty;
    });
    if (entries.isEmpty()) return out << QStringLiteral("No structured 'decisions' in this report. Run a recent Autotune helper for evidence details.");
    const int unknown = std::count_if(entries.begin(), entries.end(), [](const Entry &e) { return e.unknown; });
    out << QStringLiteral("%1 decisions · %2 insufficiently observed · ordered by evidence gap").arg(entries.size()).arg(unknown) << QString();
    for (const Entry &e : entries) {
        out << QStringLiteral("%1  [%2]  %3").arg(e.key, e.source, e.detail.isEmpty() ? QStringLiteral("confidence not reported") : e.detail)
            << QStringLiteral("    %1").arg(e.recommendation);
    }
    const QJsonObject timings = report.value("timings_ms").toObject();
    if (report.value("explore_truncated").toBool() || report.value("explore_available").toBool())
        out << QString() << QStringLiteral("Sensitivity exploration was skipped or truncated. Do not interpret missing alternatives as certainty.");
    if (!timings.isEmpty()) out << QStringLiteral("Decision/model time (ms): %1 / %2").arg(human(timings.value("decision")), human(timings.value("model")));
    return out;
}

QStringList pareto(const QJsonArray &runs) {
    QStringList out{QStringLiteral("FAN / TEMPERATURE PARETO LAB"),
        QStringLiteral("Uses the existing CPU cooling load only; no gaming frametimes or acoustic dB are measured."),
        QStringLiteral("Lower fan RPM and die temperature are better; higher sustained MHz is better."), QString()};
    struct Run { QString id, key; double rpm, temp, mhz, start; int secs; };
    QList<Run> usable;
    for (const QJsonValue &v : runs) {
        const auto o = v.toObject();
        if (o.value("noisy").toBool() || !o.value("fan_rpm").isDouble() ||
            !o.value("steady_temp").isDouble() || !o.value("mhz").isDouble()) continue;
        const double rpm = o.value("fan_rpm").toDouble(), temp = o.value("steady_temp").toDouble(), mhz = o.value("mhz").toDouble();
        const int secs = o.value("secs").toInt();
        if (!(rpm > 0 && temp > 0 && mhz > 0 && secs > 0) || !std::isfinite(rpm + temp + mhz)) continue;
        usable << Run{QDateTime::fromSecsSinceEpoch(qint64(o.value("t").toDouble())).toString("yyyy-MM-dd HH:mm"),
                     o.value("key").toString(), rpm, temp, mhz,
                     o.value("start_temp").isDouble() ? o.value("start_temp").toDouble() : std::nan(""), secs};
    }
    if (usable.size() < 2) return out << QStringLiteral("Need at least two clean runs with fan, temperature, MHz and duration data. Run matching cooling tests first.");
    // No comparing tests with different platform profiles, firmware limits, test revision or duration.
    QMap<QString, QList<Run>> groups;
    for (const auto &r : usable) groups[r.key + ':' + QString::number(r.secs)].append(r);
    int groupsShown = 0;
    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
        const auto &g = it.value();
        if (g.size() < 2) continue;
        double lowStart = std::numeric_limits<double>::infinity(), highStart = -std::numeric_limits<double>::infinity();
        // Only the runs actually compared below (a run dropped for missing data must not block the group).
        for (const Run &r : g) {
            if (std::isnan(r.start)) continue;
            lowStart = std::min(lowStart, r.start);
            highStart = std::max(highStart, r.start);
        }
        if (std::isfinite(highStart - lowStart) && highStart - lowStart > 3.0) {
            out << QStringLiteral("Context %1: start temperatures differ by %2 °C; repeat runs after equal cooldown before comparing.")
                       .arg(it.key().left(15)).arg(highStart - lowStart, 0, 'f', 1);
            continue;
        }
        ++groupsShown;
        if (!std::isfinite(highStart - lowStart))
            out << QStringLiteral("Starting temperatures missing: environmental comparability unverified.");
        out << QStringLiteral("Context %1 · %2 matching %3-second runs").arg(it.key().left(15)).arg(g.size()).arg(g.first().secs);
        for (int i = 0; i < g.size(); ++i) {
            const auto &a = g.at(i);
            bool dominated = false;
            for (int j = 0; j < g.size(); ++j) {
                if (i == j) continue;
                const auto &b = g.at(j);
                if (b.rpm <= a.rpm && b.temp <= a.temp && b.mhz >= a.mhz &&
                    (b.rpm < a.rpm || b.temp < a.temp || b.mhz > a.mhz)) { dominated = true; break; }
            }
            out << QStringLiteral("%1 · %2 rpm · %3 °C · %4 MHz · %5")
                       .arg(a.id).arg(a.rpm, 0, 'f', 0).arg(a.temp, 0, 'f', 1).arg(a.mhz, 0, 'f', 0)
                       .arg(dominated ? QStringLiteral("dominated") : QStringLiteral("PARETO FRONTIER"));
        }
        const auto silent = *std::min_element(g.begin(), g.end(), [](const Run &a, const Run &b) { return a.rpm < b.rpm; });
        const auto fast = *std::max_element(g.begin(), g.end(), [](const Run &a, const Run &b) { return a.mhz < b.mhz; });
        const auto cool = *std::min_element(g.begin(), g.end(), [](const Run &a, const Run &b) { return a.temp < b.temp; });
        out << QStringLiteral("Lowest fan speed: %1 · Highest sustained clock: %2 · Coolest: %3")
                       .arg(silent.id, fast.id, cool.id) << QString();
    }
    if (!groupsShown) out << QStringLiteral("No comparable runs: firmware context or test duration differs. Keep the same profile, limits, power source and duration.");
    out << QStringLiteral("Important: RPM is not measured sound pressure. Different room conditions can invalidate comparisons.");
    return out;
}

QStringList capabilities() {
    QStringList o{QStringLiteral("CAPABILITY DOCTOR · READ ONLY"),
        QStringLiteral("This probes sysfs, procfs and installed executables. No device is woken or written."), QString()};
    auto report = [&](const QString &name, bool ok, const QString &reason, const QString &fix) {
        o << QStringLiteral("[%1] %2").arg(ok ? QStringLiteral("available") : QStringLiteral("missing"), name);
        o << QStringLiteral("    %1").arg(reason);
        if (!ok) o << QStringLiteral("    Check: %1").arg(fix);
    };
    // ryzen-co-helper uses the ryzen_smu driver when it is loaded, else the raw SMN mailbox through the
    // host bridge's PCI config space (centurion_helpers::smu): the module is optional.
    const bool smuDriver = QDir("/sys/kernel/ryzen_smu_drv").exists();
    const bool amd = readFile("/proc/cpuinfo", 1 << 20).contains("AuthenticAMD");
    report(QStringLiteral("Ryzen CO (SMU)"), amd && (smuDriver || QFile::exists("/sys/bus/pci/devices/0000:00:00.0/config")),
           smuDriver ? QStringLiteral("ryzen_smu driver") : QStringLiteral("raw SMN mailbox via /sys/bus/pci/devices/0000:00:00.0/config (no ryzen_smu module)"),
           QStringLiteral("AMD CPU of a supported family, kernel lockdown off; Ryzen Curve → probe shows the exact reason"));
    report(QStringLiteral("CPU temperature sensors"), QDir("/sys/class/hwmon").exists(),
           QStringLiteral("/sys/class/hwmon"), QStringLiteral("k10temp, zenpower or coretemp driver and hwmon support"));
    report(QStringLiteral("Firmware attributes"), QDir("/sys/class/firmware-attributes").exists(),
           QStringLiteral("/sys/class/firmware-attributes"), QStringLiteral("vendor WMI kernel driver, supported BIOS"));
    report(QStringLiteral("NVIDIA kernel interface"), QDir("/proc/driver/nvidia").exists(),
           QStringLiteral("/proc/driver/nvidia"), QStringLiteral("nvidia kernel modules and driver build compatibility"));
    report(QStringLiteral("NVIDIA dGPU initialized"), !QDir("/proc/driver/nvidia/gpus").entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty(),
           QStringLiteral("/proc/driver/nvidia/gpus (directory only; no NVML call)"),
           QStringLiteral("dGPU power state, driver initialization and MUX; intentionally do not wake sleeping GPU"));
    report(QStringLiteral("CPU frequency controls"), QFile::exists("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor"),
           QStringLiteral("cpufreq policy0"), QStringLiteral("cpufreq scaling driver and kernel configuration"));
    report(QStringLiteral("PCIe AER counters"), QDir("/sys/bus/pci/devices").exists(),
           QStringLiteral("PCI sysfs"), QStringLiteral("CONFIG_PCIEAER and device support; errors may still require kernel log access"));
    report(QStringLiteral("Flight recorder"), QFile::exists("/var/lib/centurion/flight.ring"),
           QStringLiteral("/var/lib/centurion/flight.ring"), QStringLiteral("enable the centurion boot service and check state-directory permissions"));
    for (const QString &program : {QStringLiteral("centurion-cpustress"), QStringLiteral("centurion-cooltest"), QStringLiteral("ryzen-co-helper")}) {
        const QString p = program == "ryzen-co-helper"
                ? QStringLiteral(CENTURION_HELPER_DIR "/") + program
                : QStandardPaths::findExecutable(program);
        report(program, QFileInfo::exists(p) && QFileInfo(p).isExecutable(), p,
               QStringLiteral("install Centurion's compiled helper binaries with matching version"));
    }
    o << QString() << QStringLiteral("A missing interface is not proof the hardware lacks that capability. Some files are only visible after driver load, and installed helpers may use a custom prefix.");
    return o;
}

QWidget *capabilityPage(QWidget *parent) {
    auto *page = new QWidget(parent);
    auto *v = new QVBoxLayout(page);
    auto *intro = new QLabel(QObject::tr("Read-only diagnosis of why hardware controls may be unavailable. No root prompts or GPU wake-up."));
    intro->setWordWrap(true);
    v->addWidget(intro);
    auto *text = new QPlainTextEdit;
    text->setReadOnly(true);
    text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    v->addWidget(text, 1);
    auto *refresh = new QPushButton(QObject::tr("Scan capabilities"));
    QObject::connect(refresh, &QPushButton::clicked, page, [text] { text->setPlainText(capabilities().join(QLatin1Char('\n'))); });
    v->addWidget(refresh);
    text->setPlainText(capabilities().join(QLatin1Char('\n')));
    return page;
}

QStringList incident(const QJsonObject &event, const QString &bootId) {
    QStringList out{QStringLiteral("HARDWARE INCIDENT TIMELINE"),
        QStringLiteral("Time proximity suggests correlation, never establishes root cause."), QString()};
    const QString b = bootId.trimmed().toLower();
    const auto canonicalBoot = [](const QString &id) {
        return QString::fromStdString(diagnostichistory::fullBoot(id.toStdString()));
    };
    const auto identity = canonicalBoot(b);
    const QString kind = event.value("kind").toString();
    const QString title = event.value("title").toString();
    const double ts = event.value("ts_us").toDouble();
    out << QStringLiteral("Event: %1 · %2 · boot %3").arg(kind, title, b.left(8));
    const auto validTime = [](double seconds) -> qint64 {
        // Limit to a representable, plausible calendar range; malformed JSON is unknown.
        return std::isfinite(seconds) && seconds > 0 && seconds < 253402300800.0 ? qint64(seconds) : 0;
    };
    qint64 when = validTime(event.value("wall_ms").toDouble() / 1000);
    struct Entry { qint64 time; QString source, detail; };
    QList<Entry> timeline;
    auto append = [&](qint64 time, const QString &source, const QString &detail) { if(time > 0) timeline.append({time,source,detail}); };
    const auto appendNearest = [&](QList<Entry> &entries, qsizetype limit) {
        std::stable_sort(entries.begin(), entries.end(), [&](const Entry &a, const Entry &b) {
            return std::abs(a.time - when) < std::abs(b.time - when);
        });
        for (qsizetype i = 0; i < qMin(limit, entries.size()); ++i) timeline.append(entries.at(i));
    };
    // Current boot kernel timestamps are monotonic-since-boot; translate only if boot IDs match.
    const QString curBoot = firstLine("/proc/sys/kernel/random/boot_id");
    if (!when && !b.isEmpty() && b == curBoot && ts > 0) {
        const QByteArray up = readFile("/proc/uptime", 256);
        bool ok = false;
        const double uptime = QString::fromLatin1(up).section(' ', 0, 0).toDouble(&ok);
        if (ok && std::isfinite(uptime) && uptime >= 0 && ts / 1000000.0 <= uptime + 1) when = validTime(double(QDateTime::currentSecsSinceEpoch()) - uptime + ts / 1000000.0);
    }
    if (when) { out << QStringLiteral("Event wall time: %1").arg(QDateTime::fromSecsSinceEpoch(when).toString(Qt::ISODate)); append(when, QStringLiteral("SELECTED EVENT"), kind + QStringLiteral(" · ") + title); }
    if (!when && b != curBoot) out << QStringLiteral("Historical event: wall clock timestamp missing; temporal correlation unavailable.");
    if (!b.isEmpty() && b.size() < 32)
        out << QStringLiteral("Archived log retained only a boot-ID prefix. Full-boot flight/curve/scene attribution is deliberately unavailable.");
    if (b == curBoot) {
        out << QStringLiteral("Current kernel: %1").arg(firstLine("/proc/sys/kernel/osrelease"));
        out << QStringLiteral("Current BIOS: %1 · NVIDIA driver: %2")
                   .arg(firstLine("/sys/class/dmi/id/bios_version"), firstLine("/proc/driver/nvidia/version").section('\n', 0, 0));
        out << QStringLiteral("Version data is current; it is not a historical version snapshot.");
    }
    if (b.isEmpty()) out << QStringLiteral("Boot ID absent: refusing to attribute samples across boots.");
    // Journal is bounded and read-only. Only matching boot records are correlated.
    const QStringList journals = QString::fromUtf8(readFile("/var/lib/centurion/oc-journal.jsonl", 512 * 1024)).split('\n');
    for (const QString &line : journals) {
        const QJsonObject j = QJsonDocument::fromJson(line.toUtf8()).object();
        if (identity.isEmpty() || canonicalBoot(j.value("boot").toString()) != identity) continue;
        const qint64 t = validTime(j.value("t").toDouble());
        if (when && t && t <= when && when - t <= 7200)
            append(t, QStringLiteral("CURVE"), QStringLiteral("%1 = %2").arg(j.value("kind").toString(), j.value("state").toString()));
    }
    const QString ring = (b == curBoot) ? QStringLiteral("/var/lib/centurion/flight.ring") : QStringLiteral("/var/lib/centurion/flight-prev.ring");
    const auto bytes = diagsrc::readRecord(ring, 4 * 1024 * 1024).value_or(QByteArray{});
    QList<QJsonObject> samples;
    auto lines = bytes.split('\n');
    if (!lines.isEmpty()) lines.removeLast();  // final empty or interrupted slot
    const auto header = lines.isEmpty() ? QJsonObject{} : QJsonDocument::fromJson(lines.takeFirst().trimmed()).object();
    if (identity.isEmpty() || canonicalBoot(header.value("boot").toString()) != identity
        || header.value("interval").toInteger() <= 0) {
        out << QStringLiteral("No matching flight ring for this boot; no cross-boot sample mixing.");
    } else if (!when) {
        out << QStringLiteral("Event time unavailable; flight samples cannot be aligned.");
    } else {
        for (const auto &slot : lines) {
            const auto sample = QJsonDocument::fromJson(slot.trimmed()).object();
            if (!sample.contains("boot") && sample.value("s").toInteger(-1) >= 0 && sample.value("t").isDouble())
                samples.append(sample);
        }
        std::sort(samples.begin(), samples.end(), [](const QJsonObject &a, const QJsonObject &b) { return a.value("t").toDouble() < b.value("t").toDouble(); });
        int count = 0;
        for (const auto &s : samples) {
            const qint64 t = validTime(s.value("t").toDouble());
            if (!t || std::abs(t - when) > 180 || (count++ >= 100)) continue;
            append(t, QStringLiteral("SENSOR"), QStringLiteral("CPU %1°C · GPU %2°C · CPU %3 MHz · %4 W · load %5 · dGPU %6")
                  .arg(human(s.value("cpu_c")), human(s.value("gpu_c")), human(s.value("mhz")),
                                     human(s.value("pkg_w")), human(s.value("load")), human(s.value("dgpu"))));
        }
        if (!count) out << QStringLiteral("No sensor samples within ±180 seconds of the fault (sampling interval may be too long).");
    }
    // Transactions are recorded with boot IDs. Do not infer causality from a neighboring action.
    if (when && !identity.isEmpty()) {
        QList<Entry> transactions;
        diagsrc::scanSavedLog(diagsrc::SavedLog::SceneTransactions, 768 * 1024, [&](const QString &line, bool) {
            const auto rec = QJsonDocument::fromJson(line.toUtf8()).object();
            const qint64 t = validTime(rec.value("t").toDouble());
            if (!t || !rec.value("scene").isString()) return false;
            if (canonicalBoot(rec.value("boot").toString()) != identity || std::abs(t - when) > 1800) return true;
            const auto outcome = rec.value("outcome").isString() ? rec.value("outcome").toString()
                : rec.value("ok").isBool() ? (rec.value("ok").toBool() ? QStringLiteral("legacy helper success, unverified")
                                                                     : QStringLiteral("failure / rollback")) : QStringLiteral("unknown");
            transactions.append({t, QStringLiteral("SCENE"), QStringLiteral("%1 · %2").arg(rec.value("scene").toString(), outcome)});
            return true;
        });
        appendNearest(transactions, 50);
    }
    if (when && !identity.isEmpty()) {
        QList<Entry> healthEvents;
        QSet<quint64> seen;
        const auto selected = event.value("seq").toInteger(-1);
        diagsrc::scanSavedLog(diagsrc::SavedLog::Health, 8 * 1024 * 1024, [&](const QString &line, bool) {
            const auto record = diagsrc::parseLine(line);
            if (!record || record->kind.isEmpty()) return false;
            if (record->boot != identity || (selected >= 0 && record->seq == quint64(selected))) return true;
            const auto timestamp = QDateTime::fromString(record->time, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
            if (!timestamp.isValid()) return false;
            const auto t = timestamp.toSecsSinceEpoch();
            if (std::abs(t - when) <= 300 && !seen.contains(record->seq)) {
                seen.insert(record->seq);
                healthEvents.append({t, QStringLiteral("HEALTH"), QStringLiteral("%1 · %2").arg(record->title, record->text.left(180))});
            }
            return true;
        });
        appendNearest(healthEvents, 12);
    }
    std::stable_sort(timeline.begin(), timeline.end(), [](const Entry &a,const Entry &b){return a.time<b.time;});
    if (!timeline.isEmpty()) out << QString() << QStringLiteral("CHRONOLOGICAL RECORDS (source / offset from selected event)");
    for (const auto &entry : timeline) out << QStringLiteral("%1 [%2 s] %3: %4")
        .arg(QDateTime::fromSecsSinceEpoch(entry.time).toString(Qt::ISODate)).arg(entry.time-when).arg(entry.source,entry.detail);
    out << QString() << QStringLiteral("Missing data means unknown, not normal. Incident history is local and no information is uploaded.");
    return out;
}
} // namespace insights
