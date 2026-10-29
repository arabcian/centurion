#include "sleeppage.h"
#include "privileged.h"
#include "sleepadvice.h"
#include "theme.h"
#include "userhelper.h"
#include <QDateTime>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

static const char *SLEEP_LOG = "/var/log/centurion/sleep.log";
static constexpr int MAX_ROWS = 200;

// gui.ini, where the bag warning's limit is kept (LidWatch reads it).
static QSettings guiSettings() {
    return QSettings(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/centurion/gui.ini"), QSettings::IniFormat);
}

static QString rd(const QString &p, qint64 max = 4096) {
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.read(max)).trimmed() : QString();
}

static QLabel *muted(const QString &t) {
    auto *l = new QLabel(t);
    l->setWordWrap(true);
    l->setTextFormat(Qt::RichText);
    l->setStyleSheet(QStringLiteral("color:%1").arg(theme::MUTED));
    return l;
}

static QTreeWidget *table(const QStringList &head) {
    auto *t = new QTreeWidget;
    t->setColumnCount(head.size());
    t->setHeaderLabels(head);
    t->setRootIsDecorated(false);
    t->setAlternatingRowColors(true);
    for (int i = 0; i < head.size() - 1; ++i) t->header()->setSectionResizeMode(i, QHeaderView::ResizeToContents);
    t->header()->setStretchLastSection(true);
    return t;
}

SleepPage::SleepPage(QWidget *parent) : QWidget(parent) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(6);
    auto *top = new QHBoxLayout;
    summary_ = new QLabel;
    summary_->setWordWrap(true);
    summary_->setTextFormat(Qt::RichText);
    top->addWidget(summary_, 1);
    auto *bRefresh = new QPushButton(QStringLiteral("Refresh"));
    connect(bRefresh, &QPushButton::clicked, this, &SleepPage::refresh);
    top->addWidget(bRefresh, 0, Qt::AlignTop);
    root->addLayout(top);

    root->addWidget(new QLabel(QStringLiteral("<b>Recent sleeps</b>")));
    sleeps_ = table({QStringLiteral("Went to sleep"), QStringLiteral("Slept"), QStringLiteral("Mode"), QStringLiteral("Battery used"),
                     QStringLiteral("Per hour"), QStringLiteral("HW sleep"), QStringLiteral("dGPU in → out"), QStringLiteral("Woken by")});
    root->addWidget(sleeps_, 2);
    hint_ = muted(QString());
    root->addWidget(hint_);
    advice_ = new QWidget;
    new QVBoxLayout(advice_);
    advice_->layout()->setContentsMargins(0, 0, 0, 0);
    root->addWidget(advice_);
    diag_ = new QPushButton(tr("Why doesn't the hardware sleep?"));
    diag_->setVisible(false);
    connect(diag_, &QPushButton::clicked, this, &SleepPage::diagnose);
    root->addWidget(diag_, 0, Qt::AlignLeft);
    diagOut_ = muted(QString());
    diagOut_->setVisible(false);
    root->addWidget(diagOut_);
    // Read-only, as the user (tune-profile-helper "hibernate_check"): no password.
    hib_ = new QPushButton(tr("Can this machine hibernate?"));
    hib_->setVisible(QFileInfo::exists(QStringLiteral("/sys/power/state")));
    connect(hib_, &QPushButton::clicked, this, &SleepPage::checkHibernate);
    root->addWidget(hib_, 0, Qt::AlignLeft);
    hibOut_ = muted(QString());
    hibOut_->setTextFormat(Qt::RichText);
    hibOut_->setVisible(false);
    root->addWidget(hibOut_);

    root->addWidget(new QLabel(QStringLiteral("<b>Wake sources (ACPI)</b>")));
    wake_ = table({QStringLiteral("Device"), QStringLiteral("Sleep state"), QStringLiteral("May wake"), QStringLiteral("Sysfs node")});
    root->addWidget(wake_, 1);
    sources_ = muted(QString());
    root->addWidget(sources_);
    root->addWidget(muted(QStringLiteral(
        "Untick a device to stop it from waking the machine (e.g. a USB controller whose mouse wakes the laptop in a bag, "
        "or a PCIe port that ends sleep at once). The choice is remembered and restored at boot by the centurion-tune service. "
        "Keep the lid (LID*) and the power button enabled.")));

    // A laptop shut in a bag but still awake gets one tray warning after this long (LidWatch; 0 = never).
    auto *bag = new QHBoxLayout;
    bag->addWidget(new QLabel(tr("Warn when the lid has been closed this long and the machine is still awake (minutes, 0 = never):")));
    auto *minutes = new QSpinBox;
    minutes->setRange(0, 240);
    minutes->setValue(guiSettings().value(QStringLiteral("sleep/bag_warn_min"), 10).toInt());
    connect(minutes, &QSpinBox::valueChanged, this, [](int v) { guiSettings().setValue(QStringLiteral("sleep/bag_warn_min"), v); });
    bag->addWidget(minutes);
    bag->addStretch(1);
    root->addLayout(bag);

    connect(wake_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *it, int col) {
        if (filling_ || col != 2) return;
        const QString dev = it->text(0);
        const bool on = it->checkState(2) == Qt::Checked;
        if (!on && dev.startsWith(QLatin1String("LID"))
            && QMessageBox::question(this, QStringLiteral("Wake sources"), QStringLiteral("Opening the lid will no longer wake the machine. Continue?")) != QMessageBox::Yes) {
            fillWake();
            return;
        }
        setWake(dev, on);
    });
}

void SleepPage::setWake(const QString &dev, bool on) {
    privileged::run(privileged::helperPath(QStringLiteral("tune-helper")), QJsonObject{{"op", "wakeup_set"}, {"device", dev}, {"enabled", on}}, this,
                    [this, dev](const privileged::Result &r) {
                        if (!r.ok()) QMessageBox::warning(this, QStringLiteral("Wake sources"), dev + QStringLiteral(": ") + r.message());
                        refresh();
                    });
}

void SleepPage::diagnose() {
    diag_->setEnabled(false);
    diagOut_->setText(tr("Reading the sleep path…"));
    diagOut_->setVisible(true);
    // Read-only (health-reader "sleep_diag"); the debugfs counters need root, the rest is sysfs.
    privileged::run(privileged::helperPath(QStringLiteral("health-reader")), QJsonObject{{QStringLiteral("op"), QStringLiteral("sleep_diag")}}, this,
                    [this](const privileged::Result &r) {
        diag_->setEnabled(true);
        if (r.ok()) showDiagnosis(r.json);
        else diagOut_->setText(tr("Could not read the sleep path: %1").arg(r.message()));
    });
}

void SleepPage::showDiagnosis(const QJsonObject &r) {
    QStringList parts;
    for (const QJsonValue &v : r.value(QStringLiteral("findings")).toArray()) {
        const QJsonObject f = v.toObject();
        const QString text = findingText(f.value(QStringLiteral("code")).toString(), f.value(QStringLiteral("detail")));
        if (text.isEmpty()) continue;
        const bool warn = f.value(QStringLiteral("severity")).toString() == QLatin1String("warn");
        parts << QStringLiteral("<span style='color:%1'>%2</span>").arg(QLatin1String(warn ? theme::WARN : theme::FG_DIM), text.toHtmlEscaped());
    }
    // The raw counters, for whoever wants to compare them with the Intel or AMD documentation.
    const QJsonObject dbg = r.value(QStringLiteral("readings")).toObject().value(QStringLiteral("debugfs")).toObject();
    const QList<QPair<QString, QString>> raw{{QStringLiteral("amd_s0ix"), tr("AMD S0ix statistics")},
                                              {QStringLiteral("amd_idlemask"), tr("AMD idle mask")},
                                              {QStringLiteral("intel_substates"), tr("Intel package C-state residency")},
                                              {QStringLiteral("intel_ltr"), tr("Intel latency tolerance (LTR)")}};
    for (const auto &[key, label] : raw) {
        const QString v = dbg.value(key).toString();
        if (!v.isEmpty()) parts << QStringLiteral("<b>%1</b><pre style='margin:0'>%2</pre>").arg(label, v.toHtmlEscaped());
    }
    diagOut_->setText(parts.isEmpty() ? tr("Nothing to report.") : parts.join(QStringLiteral("<br>")));
}

QString SleepPage::findingText(const QString &code, const QJsonValue &detail) {
    if (code == QLatin1String("deep_sleep"))
        return tr("This machine suspends with deep (S3) sleep. The hardware sleep counter stays at zero in that mode, so the hardware-sleep share says nothing here.");
    if (code == QLatin1String("aspm_performance"))
        return tr("PCIe ASPM is set to performance: PCIe links never enter their low-power states, which keeps the processor package awake. "
                  "The kernel option pcie_aspm.policy=powersave lifts it.");
    if (code == QLatin1String("nvme_apst_off"))
        return tr("NVMe power states are disabled (nvme_core.default_ps_max_latency_us=0). A drive that cannot enter its low-power states keeps the platform awake.");
    if (code == QLatin1String("nvme_pm_on"))
        return tr("NVMe runtime power management is on for %1. Its power/control should be auto.").arg(detail.toString());
    if (code == QLatin1String("usb_pm_on"))
        return tr("%1 USB device(s) keep runtime power management on (power/control = on): %2. "
                  "A device that cannot sleep keeps the platform awake; its power/control should be auto.")
            .arg(detail.toObject().value(QStringLiteral("count")).toInt()).arg(detail.toObject().value(QStringLiteral("devices")).toString());
    if (code == QLatin1String("dgpu_active"))
        return tr("The NVIDIA GPU (%1) is powered on right now. While it is, the platform cannot reach its deepest sleep. "
                  "The dGPU power page shows how long it stays awake and what holds it.").arg(detail.toString());
    if (code == QLatin1String("debugfs_unreadable"))
        return tr("The AMD and Intel power counters cannot be read: debugfs is not mounted, or this machine has none of them.");
    if (code == QLatin1String("nothing_found"))
        return tr("No setting above explains it. The raw counters are shown below for comparison.");
    return {};
}

void SleepPage::checkHibernate() {
    hib_->setEnabled(false);
    userhelper::run(QJsonObject{{QStringLiteral("op"), QStringLiteral("hibernate_check")}}, this, [this](const QJsonObject &r) {
        hib_->setEnabled(true);
        hibOut_->setVisible(true);
        if (!r.value(QStringLiteral("ok")).toBool()) {
            hibOut_->setText(tr("Could not check: %1").arg(r.value(QStringLiteral("error")).toString().toHtmlEscaped()));
            return;
        }
        QStringList parts;
        for (const QJsonValue &v : r.value(QStringLiteral("findings")).toArray()) {
            const QJsonObject f = v.toObject();
            const QString text = hibernateText(f.value(QStringLiteral("code")).toString(), f.value(QStringLiteral("detail")));
            if (text.isEmpty()) continue;
            const bool warn = f.value(QStringLiteral("severity")).toString() == QLatin1String("warn");
            parts << QStringLiteral("<span style='color:%1'>%2</span>").arg(QLatin1String(warn ? theme::WARN : theme::OK), text.toHtmlEscaped());
        }
        hibOut_->setText(parts.join(QStringLiteral("<br>")));
    });
}

QString SleepPage::hibernateText(const QString &code, const QJsonValue &detail) {
    const QJsonObject d = detail.toObject();
    if (code == QLatin1String("no_disk_state"))
        return tr("The kernel does not offer hibernation (no \"disk\" in /sys/power/state): it was built without it or it is switched off.");
    if (code == QLatin1String("lockdown"))
        return tr("Kernel lockdown is in force (%1, usually because of Secure Boot): the kernel refuses to hibernate.").arg(detail.toString());
    if (code == QLatin1String("no_swap"))
        return d.value(QStringLiteral("zram")).toBool()
            ? tr("The only swap is zram, which lives in memory: hibernation needs a swap partition or swap file on disk.")
            : tr("There is no swap: hibernation needs a swap partition or swap file on disk.");
    if (code == QLatin1String("swap_small"))
        return tr("Swap on disk (%1 GiB) is smaller than the memory (%2 GiB). Hibernation can fail when much of the memory is in use.")
            .arg(d.value(QStringLiteral("swap_kib")).toDouble() / 1048576.0, 0, 'f', 1).arg(d.value(QStringLiteral("mem_kib")).toDouble() / 1048576.0, 0, 'f', 1);
    if (code == QLatin1String("no_resume"))
        return tr("No resume= on the kernel command line, and systemd did not record a HibernateLocation: the next boot would not find the image.");
    if (code == QLatin1String("swap_plain_crypt"))
        return tr("Swap %1 is plain dm-crypt, usually with a new random key each boot: an image written there cannot be read back. Use LUKS with a fixed key instead.").arg(detail.toString());
    if (code == QLatin1String("swap_luks"))
        return tr("Swap %1 is LUKS-encrypted: the initramfs must unlock it before resuming.").arg(detail.toString());
    if (code == QLatin1String("ready"))
        return tr("Nothing found that would stop a hibernate.");
    return {};
}

void SleepPage::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    refresh();
}

void SleepPage::refresh() {
    const QString s = QStringLiteral("/sys/power/suspend_stats/");
    QStringList p;
    const QString modes = rd(QStringLiteral("/sys/power/mem_sleep"));
    const int a = modes.indexOf('['), b = modes.indexOf(']');
    const QString mode = a >= 0 && b > a ? modes.mid(a + 1, b - a - 1) : QString();
    if (!mode.isEmpty())
        p << QStringLiteral("Suspend mode: <b>%1</b>%2").arg(mode == QLatin1String("s2idle") ? QStringLiteral("s2idle (modern standby)") : mode == QLatin1String("deep") ? QStringLiteral("deep (S3)") : mode,
                                                           modes.split(' ').size() > 1 ? QStringLiteral(" (offered: %1)").arg(QString(modes).remove('[').remove(']')) : QString());
    if (const QString ok = rd(s + "success"); !ok.isEmpty())
        p << QStringLiteral("since boot: %1 suspends, <span style='color:%2'>%3 failed</span>").arg(ok, rd(s + "fail").toInt() ? theme::DANGER : theme::OK, rd(s + "fail"));
    if (const int fails = rd(s + "fail").toInt(); fails > 0) {
        QString last = rd(s + "last_failed_dev");
        if (const QString step = rd(s + "last_failed_step"); !step.isEmpty()) last += (last.isEmpty() ? QString() : QStringLiteral(" · ")) + step;
        if (const QString err = rd(s + "last_failed_errno"); !err.isEmpty() && err != QLatin1String("0")) last += QStringLiteral(" · errno ") + err;
        if (!last.isEmpty()) p << QStringLiteral("last failure: ") + last.toHtmlEscaped();
    }
    if (const QString hw = rd(s + "total_hw_sleep"); !hw.isEmpty())
        p << QStringLiteral("hardware sleep since boot: %1 h (longest %2 h)").arg(hw.toDouble() / 3.6e9, 0, 'f', 1).arg(rd(s + "max_hw_sleep").toDouble() / 3.6e9, 0, 'f', 1);
    summary_->setText(p.isEmpty() ? QStringLiteral("This kernel reports no suspend statistics.") : p.join(QStringLiteral(" &nbsp;·&nbsp; ")));
    fillSleeps();
    fillWake();
}

void SleepPage::fillSleeps() {
    sleeps_->clear();
    QFile f(QString::fromLatin1(SLEEP_LOG));
    if (!f.open(QIODevice::ReadOnly)) {
        hint_->setText(QStringLiteral("No sleep log yet. It is written by the centurion-sleep-log system-sleep hook (installed by install.sh / the ebuild) "
                                      "from the next suspend on."));
        fillAdvice({});
        diag_->setVisible(false);
        return;
    }
    if (f.size() > (1 << 20)) f.seek(f.size() - (1 << 20));
    QList<QJsonObject> rows;
    for (const QByteArray &l : f.readAll().split('\n')) {
        const QJsonObject o = QJsonDocument::fromJson(l).object();
        if (!o.isEmpty()) rows << o;
    }
    fillAdvice(rows);
    QList<sleepadvice::Sleep> parsed;
    for (const QJsonObject &o : rows) parsed << sleepadvice::parse(o);
    diag_->setVisible(sleepadvice::hwSleepIdle(parsed));
    std::reverse(rows.begin(), rows.end());
    double drainSum = 0, hoursSum = 0;
    for (const QJsonObject &o : rows.mid(0, MAX_ROWS)) {
        const qint64 t0 = o.value("start").toInteger(), t1 = o.value("end").toInteger();
        const double h = (t1 - t0) / 3600.0;
        const QString dur = t1 > t0 ? (h >= 1 ? QStringLiteral("%1 h %2 min").arg(int(h)).arg(int(h * 60) % 60) : QStringLiteral("%1 min").arg((t1 - t0 + 30) / 60)) : QStringLiteral("—");
        const double e0 = o.value("energy_before").toDouble(), e1 = o.value("energy_after").toDouble(), full = o.value("full").toDouble();
        const bool onBattery = o.value("status_before").toString() == QLatin1String("Discharging");
        QString used = QStringLiteral("on AC"), per = QStringLiteral("—");
        if (onBattery && full > 0 && e0 >= e1) {
            const double pct = 100.0 * (e0 - e1) / full;
            used = QStringLiteral("%1%").arg(pct, 0, 'f', 1);
            if (o.value("unit").toString() == QLatin1String("uWh")) used += QStringLiteral(" (%1 Wh)").arg((e0 - e1) / 1e6, 0, 'f', 2);
            if (h >= 0.25) { per = QStringLiteral("%1 %/h").arg(pct / h, 0, 'f', 2); drainSum += pct; hoursSum += h; }
        } else if (!onBattery && o.value("status_before").toString().isEmpty()) {
            used = QStringLiteral("—");
        }
        const bool hwKnown = !o.value("hw_sleep_us").toString().isEmpty();
        const double hwUs = o.value("hw_sleep_us").toString().toDouble();
        const QString hw = (hwKnown && t1 > t0) ? QStringLiteral("%1%").arg(std::min(100.0, hwUs / 1e6 / double(t1 - t0) * 100), 0, 'f', 0) : QStringLiteral("—");
        QString woke = o.value("woke_by").toString();
        woke.replace(',', QStringLiteral(", "));
        if (woke.isEmpty() && !o.value("wake_irq").toString().isEmpty()) woke = QStringLiteral("IRQ ") + o.value("wake_irq").toString();
        auto *it = new QTreeWidgetItem({QDateTime::fromSecsSinceEpoch(t0).toString(QStringLiteral("yyyy-MM-dd HH:mm")), dur,
                                        o.value("mode").toString().isEmpty() ? o.value("kind").toString() : o.value("mode").toString(),
                                        used, per, hw, o.value("dgpu_before").toString() + QStringLiteral(" → ") + o.value("dgpu_after").toString(), woke});
        // A sleep that kept the SoC mostly awake, or drained fast, stands out.
        if ((hwKnown && t1 - t0 > 600 && hwUs / 1e6 / double(t1 - t0) < 0.5) || (per != QLatin1String("—") && per.section(' ', 0, 0).toDouble() > 2.0))
            for (int i = 0; i < 8; ++i) it->setForeground(i, QColor(QLatin1String(theme::WARN)));
        if (o.value("dgpu_after").toString() == QLatin1String("error"))
            for (int i = 0; i < 8; ++i) it->setForeground(i, QColor(QLatin1String(theme::DANGER)));
        sleeps_->addTopLevelItem(it);
    }
    hint_->setText(hoursSum > 0 ? QStringLiteral("Average on battery: %1 %/h over %2 h of sleep. Orange rows: hardware sleep under half the time, or more than 2 %/h. "
                                                 "Red: the dGPU did not come back.").arg(drainSum / hoursSum, 0, 'f', 2).arg(hoursSum, 0, 'f', 1)
                                : QStringLiteral("Battery drain shows for sleeps of 15 min or more that started on battery."));
}

void SleepPage::fillWake() {
    filling_ = true;
    wake_->clear();
    const QStringList lines = rd(QStringLiteral("/proc/acpi/wakeup"), 64 * 1024).split('\n');
    for (int i = 1; i < lines.size(); ++i) {
        const QStringList f = lines.at(i).simplified().split(' ');
        if (f.size() < 3) continue;
        const bool on = f.at(2).contains(QLatin1String("enabled"));
        auto *it = new QTreeWidgetItem({f.at(0), f.at(1), QString(), f.size() > 3 ? f.at(3) : QString()});
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(2, on ? Qt::Checked : Qt::Unchecked);
        wake_->addTopLevelItem(it);
    }
    if (!wake_->topLevelItemCount()) wake_->addTopLevelItem(new QTreeWidgetItem({QStringLiteral("/proc/acpi/wakeup is not available."), {}, {}, {}}));
    filling_ = false;
    // Who actually woke the machine most (kernel wakeup sources, all of them, since boot).
    QList<QPair<qint64, QString>> top;
    for (const QString &w : QDir(QStringLiteral("/sys/class/wakeup")).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System)) {
        const qint64 n = rd("/sys/class/wakeup/" + w + "/wakeup_count").toLongLong();
        if (n > 0) top.append({n, rd("/sys/class/wakeup/" + w + "/name")});
    }
    std::sort(top.begin(), top.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    QStringList l;
    for (const auto &[n, name] : top.mid(0, 6)) l << QStringLiteral("%1 ×%2").arg(name.toHtmlEscaped()).arg(n);
    sources_->setText(l.isEmpty() ? QStringLiteral("No wakeup events counted since boot.") : QStringLiteral("Wakeups since boot by source: ") + l.join(QStringLiteral(", ")));
}

/// Kernel wakeup-source name → the enabled ACPI wake device (/proc/acpi/wakeup) above it, "" if none:
/// a PCI controller matches by its address, a USB device behind it through its sysfs ancestors.
static std::function<QString(const QString &)> wakeResolver() {
    QHash<QString, QString> byNode;  // "0000:00:14.0" → "XHC0"; enabled devices only
    const QStringList lines = rd(QStringLiteral("/proc/acpi/wakeup"), 64 * 1024).split('\n');
    for (int i = 1; i < lines.size(); ++i) {
        const QStringList f = lines.at(i).simplified().split(' ');
        if (f.size() >= 4 && f.at(2).contains(QLatin1String("enabled"))) byNode.insert(f.at(3).section(':', 1), f.at(0));
    }
    QHash<QString, QString> pathOf;  // wakeup-source name → its sysfs path (under the device that owns it)
    for (const QString &w : QDir(QStringLiteral("/sys/class/wakeup")).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System))
        pathOf.insert(rd("/sys/class/wakeup/" + w + "/name").replace(' ', '_'),  // as the sleep hook logs it
                      QFileInfo("/sys/class/wakeup/" + w).canonicalFilePath());
    return [byNode, pathOf](const QString &name) -> QString {
        if (const QString d = byNode.value(name); !d.isEmpty()) return d;
        for (QString p = pathOf.value(name); p.size() > 1; p = QFileInfo(p).path())
            if (const QString d = byNode.value(QFileInfo(p).fileName()); !d.isEmpty()) return d;
        return {};
    };
}

void SleepPage::fillAdvice(const QList<QJsonObject> &rowsOldestFirst) {
    qDeleteAll(advice_->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly));
    QList<sleepadvice::Sleep> rows;
    for (const QJsonObject &o : rowsOldestFirst) rows << sleepadvice::parse(o);
    for (const sleepadvice::Advice &a : sleepadvice::analyse(rows, wakeResolver())) {
        auto *line = new QWidget;
        auto *h = new QHBoxLayout(line);
        h->setContentsMargins(0, 0, 0, 0);
        const QString cost = a.awakePct > 0
            ? QStringLiteral("; afterwards the machine spent <b>%1%</b> of the battery awake before sleeping again").arg(a.awakePct, 0, 'f', 1)
            : QString();
        auto *l = new QLabel(QStringLiteral("<span style='color:%1'>Suggestion:</span> <b>%2</b> woke the machine %3 times with the lid closed%4. "
                                            "Switch it off as a wake source?")
                                 .arg(theme::WARN, a.device.toHtmlEscaped()).arg(a.wakes).arg(cost));
        l->setWordWrap(true);
        l->setTextFormat(Qt::RichText);
        l->setToolTip(QStringLiteral("Counted from the sleep log: sleeps that ended without the lid or the power button among the wake sources. "
                                     "Battery cost is measured only when the machine was on battery and fell asleep again within %1 h.")
                          .arg(sleepadvice::MAX_AWAKE_S / 3600));
        auto *b = new QPushButton(QStringLiteral("Disable %1").arg(a.device));
        connect(b, &QPushButton::clicked, this, [this, dev = a.device] { setWake(dev, false); });
        h->addWidget(l, 1);
        h->addWidget(b, 0, Qt::AlignTop);
        advice_->layout()->addWidget(line);
    }
}
