#include "healthevidence.h"
#include "privileged.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QHash>
#include <QSet>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {
QString path() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
           QStringLiteral("/centurion/aer-last-sample.json");
}
QString fmtTime(const QJsonValue &ts) {
    if (!ts.isDouble()) return QStringLiteral("(timestamp unknown)");
    return QStringLiteral("t+%1s").arg(ts.toDouble() / 1000000.0, 0, 'f', 1);
}
QString toTxt(const QJsonValue &v) {
    if (v.isNull() || v.isUndefined()) return QStringLiteral("Unknown");
    const QString value = v.toVariant().toString();
    return value.isEmpty() ? QStringLiteral("Unknown") : value;
}
QStringList recentSaved(const QStringList &kinds, int limit = 50) {
    // Existing Health event ledger, including previous boots, is already
    // bounded/rotated. Show only the newest matching entries, never mutate it.
    QStringList lines;
    QString state = qEnvironmentVariable("XDG_STATE_HOME");
    if (state.isEmpty()) state = QDir::homePath() + QStringLiteral("/.local/state");
    const QString base = state + QStringLiteral("/centurion/health.log");
    for (const QString &name : {base + QStringLiteral(".1"), base}) {
        QFile f(name);
        if (!f.open(QIODevice::ReadOnly)) continue;
        // Seek near tail of large rotated files, keeping reads bounded.
        if (f.size() > (2 << 20)) f.seek(f.size() - (2 << 20));
        while (!f.atEnd()) {
            const QString line = QString::fromUtf8(f.readLine(4096)).trimmed();
            for (const QString &kind : kinds) {
                if (line.contains(QStringLiteral(" ") + kind.toUpper() + QLatin1Char(' '))) {
                    lines << line.left(500);
                    if (lines.size() > limit) lines.removeFirst();
                    break;
                }
            }
        }
    }
    return lines;
}
QString aerReport(const QJsonObject &reply) {
    QStringList lines = {QStringLiteral("H03 — PCIe AER DELTA TRACKER\n"
        "AER sysfs counters are cumulative within a boot, NOT per-event timestamps.\n"
        "Negative delta means reset/rollover/re-enumeration: never interpret it as a negative error rate.\n")};
    const QString boot = reply.value(QStringLiteral("boot_id")).toString();
    QJsonObject old;
    QFile f(path());
    if (f.open(QIODevice::ReadOnly)) {
        const QByteArray raw = f.read(256*1024 + 1);
        if (raw.size() <= 256*1024) old = QJsonDocument::fromJson(raw).object();
    }
    const bool sameBoot = !boot.isEmpty() && old.value(QStringLiteral("boot_id")).toString() == boot;
    const QJsonArray previous = old.value(QStringLiteral("counters")).toArray();
    QHash<QString, QJsonObject> before;
    if (sameBoot) for (const QJsonValue &v : previous) {
        const QJsonObject a = v.toObject();
        before.insert(a.value(QStringLiteral("dev")).toString(), a);
    }
    const QJsonArray current = reply.value(QStringLiteral("aer")).toArray();
    lines << QStringLiteral("Boot: %1\nPrevious sample: %2\n")
                 .arg(boot, sameBoot ? old.value(QStringLiteral("taken_at")).toString() : QStringLiteral("no comparable sample (first scan / different boot)"));
    QSet<QString> currentDevs;
    for (const QJsonValue &v : current) {
        const QJsonObject a = v.toObject();
        const QString dev = a.value(QStringLiteral("dev")).toString();
        currentDevs.insert(dev);
        const QJsonObject oldDevice = before.value(dev);
        const bool verifiedIdentity = !oldDevice.value(QStringLiteral("vendor")).toString().isEmpty() &&
            !oldDevice.value(QStringLiteral("device")).toString().isEmpty() &&
            !a.value(QStringLiteral("vendor")).toString().isEmpty() &&
            !a.value(QStringLiteral("device")).toString().isEmpty() &&
            oldDevice.value(QStringLiteral("vendor"))==a.value(QStringLiteral("vendor")) &&
            oldDevice.value(QStringLiteral("device"))==a.value(QStringLiteral("device"));
        QStringList counters;
        for (const QString &field : {"cor", "nonfatal", "fatal"}) {
            const QJsonValue count = a.value(field);
            if (!count.isDouble()) { counters << field + QStringLiteral("=Unknown"); continue; }
            const qint64 now = count.toInteger();
            QString value = QString::number(now);
            if (sameBoot && before.contains(dev)) {
                if (!verifiedIdentity) value += QStringLiteral(" (endpoint identity changed/unverified; delta Unknown)");
                else if (oldDevice.value(field).isDouble()) {
                    const qint64 then = oldDevice.value(field).toInteger();
                    value += (now < then ? QStringLiteral(" (reset/decrease: delta Unknown)") :
                              QStringLiteral(" (delta +%1)").arg(now - then));
                } else value += QStringLiteral(" (previous count unavailable; delta Unknown)");
            } else value += QStringLiteral(" (no previous sample)");
            counters << field + QStringLiteral("=") + value;
        }
        lines << QStringLiteral("%1 %2\n  %3").arg(dev, a.value(QStringLiteral("what")).toString(), counters.join(QStringLiteral("; ")));
    }
    if (current.isEmpty()) lines << QStringLiteral("No AER-capable PCI devices exposed in sysfs; NOT proof of zero errors.");
    if (sameBoot) for (auto it = before.cbegin(); it != before.cend(); ++it)
        if (!currentDevs.contains(it.key())) lines << QStringLiteral("Device disappeared/unsupported: %1 (cannot infer delta)").arg(it.key());
    if (!boot.isEmpty()) {
        QJsonObject record{{QStringLiteral("boot_id"),boot},
                           {QStringLiteral("taken_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                           {QStringLiteral("counters"),current}};
        if (QDir().mkpath(QFileInfo(path()).absolutePath())) {
            QSaveFile save(path());
            const QByteArray raw = QJsonDocument(record).toJson(QJsonDocument::Compact);
            if (raw.size() > 256*1024 || !save.open(QIODevice::WriteOnly) ||
                !save.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
                save.write(raw) != raw.size() || !save.commit())
                lines << QStringLiteral("Warning: last AER sample could not be saved");
            else lines << QStringLiteral("AER sample securely saved for next on-demand comparison.");
        } else lines << QStringLiteral("Warning: cannot create AER sample directory");
    }
    lines << QStringLiteral("\nSamples are only saved after Scan; PCIe idle link downclocking is normal.");
    return lines.join(QLatin1Char('\n'));
}
}  // namespace

HealthEvidencePage::HealthEvidencePage(QWidget *parent) : QWidget(parent) {
    auto t = [](const char *s) { return QCoreApplication::translate("HealthEvidencePage", s); };
    auto *layout = new QVBoxLayout(this);
    auto *top = new QHBoxLayout;
    auto *info = new QLabel(t("On-demand diagnostic evidence from the current boot. The scan is read-only and may require polkit authorization."));
    info->setWordWrap(true);
    top->addWidget(info, 1);
    scan_ = new QPushButton(t("Scan evidence"));
    auto *copy = new QPushButton(t("Copy current"));
    top->addWidget(scan_); top->addWidget(copy);
    layout->addLayout(top);
    status_ = new QLabel(t("Not scanned yet. No monitoring process is running."));
    status_->setWordWrap(true);
    layout->addWidget(status_);
    tabs_ = new QTabWidget(this);
    const QStringList names = {t("H02 · Ryzen MCE Core/CCD"), t("H03 · PCIe AER Delta"), t("H04 · NVIDIA Xid Cases"),
                               t("H05 · F2FS Integrity"), t("H09 · Wi-Fi / Bluetooth")};
    for (int i = 0; i < 5; ++i) {
        views_[i] = new QPlainTextEdit;
        views_[i]->setReadOnly(true);
        views_[i]->setLineWrapMode(QPlainTextEdit::NoWrap);
        views_[i]->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        views_[i]->setPlainText(t("Press Scan evidence to view this diagnostic."));
        tabs_->addTab(views_[i], names.at(i));
    }
    layout->addWidget(tabs_, 1);
    connect(scan_, &QPushButton::clicked, this, [this] { collect(); });
    connect(copy, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(views_[tabs_->currentIndex()]->toPlainText());
    });
}

void HealthEvidencePage::collect() {
    if (busy_) return;
    busy_ = true;
    scan_->setEnabled(false);
    status_->setText(QCoreApplication::translate("HealthEvidencePage", "Scanning classified kernel events and read-only topology…"));
    privileged::run(privileged::helperPath(QStringLiteral("health-reader")),
                    QJsonObject{{QStringLiteral("op"), QStringLiteral("diagnostics")}}, this,
                    [this](const privileged::Result &r) {
        busy_ = false;
        scan_->setEnabled(true);
        if (!r.ok()) {
            status_->setText(QStringLiteral("Scan unavailable: %1").arg(r.message()));
            return;  // Do not replace earlier results or claim healthy on an unreadable log.
        }
        display(r.json);
    }, 20000);
}

void HealthEvidencePage::display(const QJsonObject &r) {
    const QString boot = r.value(QStringLiteral("boot_id")).toString();
    status_->setText(QStringLiteral("Boot %1 · Latest %2 classified events (bounded). Empty results are UNKNOWN, not healthy.")
                    .arg(boot.left(12)).arg(r.value(QStringLiteral("event_count")).toInt()));
    QStringList mce = {QStringLiteral("H02 — RYZEN MCE CORE / CCD ATLAS\n"
        "A CPU number maps to a CURRENT Linux logical CPU. L3-sharing may approximate CCD groups on some Ryzen designs,"
        " but no CCD index or unstable CO value is inferred. APIC ID alone is NOT a Linux CPU index.\n")};
    for (const QJsonValue &v : r.value(QStringLiteral("mce")).toArray()) {
        const QJsonObject e = v.toObject();
        const QJsonObject topo = e.value(QStringLiteral("topology")).toObject();
        mce << QStringLiteral("%1  CPU=%2  Bank=%3  APIC=%4  severity=%5\n"
               "  package=%6 core=%7 die=%8 threads=%9 L3-sharing=%10\n  Confidence: %11\n  %12\n")
               .arg(fmtTime(e.value(QStringLiteral("ts_us"))), toTxt(e.value(QStringLiteral("cpu"))),
                    toTxt(e.value(QStringLiteral("bank"))), toTxt(e.value(QStringLiteral("apic_id"))),
                    toTxt(e.value(QStringLiteral("severity"))), toTxt(topo.value(QStringLiteral("package"))),
                    toTxt(topo.value(QStringLiteral("core_id"))), toTxt(topo.value(QStringLiteral("die_id"))),
                    toTxt(topo.value(QStringLiteral("threads"))), toTxt(topo.value(QStringLiteral("l3_shared_cpus"))),
                    e.value(QStringLiteral("confidence")).toString(), e.value(QStringLiteral("text")).toString());
    }
    if (r.value(QStringLiteral("mce")).toArray().isEmpty()) mce << QStringLiteral("No classified MCE records captured in the available log window.");
    mce << QStringLiteral("\nCurrent logical CPU topology (L3 groups are NOT guaranteed CCD IDs):");
    for (const QJsonValue &v : r.value(QStringLiteral("topology")).toArray()) {
        const QJsonObject a = v.toObject();
        mce << QStringLiteral("cpu%1 package=%2 core=%3 die=%4 L3-shared=%5")
           .arg(toTxt(a.value(QStringLiteral("logical_cpu"))),toTxt(a.value(QStringLiteral("package"))),
                toTxt(a.value(QStringLiteral("core_id"))),toTxt(a.value(QStringLiteral("die_id"))),
                toTxt(a.value(QStringLiteral("l3_shared_cpus"))));
    }
    views_[0]->setPlainText(mce.join(QLatin1Char('\n')));
    views_[1]->setPlainText(aerReport(r));
    QStringList xid = {QStringLiteral("H04 — NVIDIA Xid CASE ANALYZER\n"
        "Xid 154 is a recovery directive, NOT a root cause. Only earlier events within five minutes"
        " are shown, tagged same-GPU or device-unverified. This is temporal correlation.\n")};
    for (const QJsonValue &v : r.value(QStringLiteral("xid_cases")).toArray()) {
        const QJsonObject c = v.toObject();
        xid << QStringLiteral("Xid 154 at %1, GPU PCI %2\n  %3")
           .arg(fmtTime(c.value(QStringLiteral("ts_us"))),
                c.value(QStringLiteral("pci")).toString().isEmpty() ? QStringLiteral("Unknown") : c.value(QStringLiteral("pci")).toString(),
                c.value(QStringLiteral("assessment")).toString());
        for (const QJsonValue &pv : c.value(QStringLiteral("preceding")).toArray()) {
            const QJsonObject p = pv.toObject();
            xid << QStringLiteral("  %1 [%2] %3 — %4")
               .arg(fmtTime(p.value(QStringLiteral("ts_us"))), p.value(QStringLiteral("scope")).toString(),
                    p.value(QStringLiteral("kind")).toString(), p.value(QStringLiteral("title")).toString());
        }
    }
    if (r.value(QStringLiteral("xid_cases")).toArray().isEmpty()) xid << QStringLiteral("No Xid 154 cases in the available classified event window.");
    views_[2]->setPlainText(xid.join(QLatin1Char('\n')));
    QStringList f2fs = {QStringLiteral("H05 — F2FS INTEGRITY GUARD\n"
        "Only explicit kernel integrity, checkpoint and I/O failures are classified. No fsck, remount,"
        " write, SMART self-test or storage stress action is performed. Back up before attempting repair.\n")};
    for (const QJsonValue &v : r.value(QStringLiteral("f2fs_mounts")).toArray()) {
        const QJsonObject m = v.toObject();
        f2fs << QStringLiteral("Mounted: %1 at %2; options %3")
           .arg(m.value(QStringLiteral("source")).toString(),m.value(QStringLiteral("mount")).toString(),m.value(QStringLiteral("options")).toString());
    }
    if (r.value(QStringLiteral("f2fs_mounts")).toArray().isEmpty()) f2fs << QStringLiteral("No F2FS mount visible from the helper namespace.");
    for (const QJsonValue &v : r.value(QStringLiteral("f2fs_events")).toArray()) {
        const QJsonObject e = v.toObject();
        f2fs << fmtTime(e.value(QStringLiteral("ts_us"))) + QStringLiteral("  ") + e.value(QStringLiteral("text")).toString();
    }
    if (r.value(QStringLiteral("f2fs_events")).toArray().isEmpty()) f2fs << QStringLiteral("No classified F2FS errors in current scan window (NOT a guarantee of integrity).");
    const QStringList fsHist = recentSaved({QStringLiteral("f2fs")}, 40);
    f2fs << QStringLiteral("\nSaved Health event history (including prior boots; bounded/rotated):");
    f2fs << (fsHist.isEmpty() ? QStringList{QStringLiteral("No matching saved events available.")} : fsHist);
    views_[3]->setPlainText(f2fs.join(QLatin1Char('\n')));
    QStringList radio = {QStringLiteral("H09 — WI-FI / BLUETOOTH FAILURE LEDGER\n"
        "Classifies firmware load failures, timeouts, controller faults; normal reconnection is not a fault."
        " Some driver messages are ambiguous and no single failure proves hardware damage.\n")};
    radio << QStringLiteral("Current-boot failures grouped by observed adapter/controller:");
    for (const QJsonValue &v : r.value(QStringLiteral("radio_summary")).toArray()) {
        const QJsonObject e = v.toObject();
        radio << QStringLiteral("  %1 — %2 event(s)").arg(e.value(QStringLiteral("device")).toString()).arg(e.value(QStringLiteral("errors")).toInt());
    }
    radio << QStringLiteral("\nTimeline:");
    for (const QJsonValue &v : r.value(QStringLiteral("radio_events")).toArray()) {
        const QJsonObject e = v.toObject();
        radio << QStringLiteral("%1 [%2] %3\n  %4")
            .arg(fmtTime(e.value(QStringLiteral("ts_us"))),e.value(QStringLiteral("kind")).toString(),
                 e.value(QStringLiteral("title")).toString(),e.value(QStringLiteral("text")).toString());
    }
    if (r.value(QStringLiteral("radio_events")).toArray().isEmpty()) radio << QStringLiteral("No classified radio firmware failures in current scan window.");
    radio << QStringLiteral("\nSaved Health event ledger (newest matching lines, includes previous boots):");
    const QStringList hist = recentSaved({QStringLiteral("wifi"), QStringLiteral("bluetooth")}, 80);
    radio << (hist.isEmpty() ? QStringList{QStringLiteral("No matching saved events available.")} : hist);
    views_[4]->setPlainText(radio.join(QLatin1Char('\n')));
}
