#include "healthtab.h"
#include "backuppage.h"
#include "batterypage.h"
#include "notifydialog.h"
#include "notifyprefs.h"
#include "bootpage.h"
#include "coolingpage.h"
#include "curvepage.h"
#include "sleeppage.h"
#include "klogpage.h"
#include "dgpupage.h"
#include "netpage.h"
#include "overviewpage.h"
#include "smartpage.h"
#include "charts.h"
#include "userhelper.h"

#include "privileged.h"
#include "sysinfo.h"
#include "systools.h"
#include "theme.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QJsonDocument>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>
#include <ctime>
#include <QSocketNotifier>
#include <QTabWidget>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cerrno>
#include <cstring>
#include <memory>
#include <fcntl.h>
#include <unistd.h>

static constexpr int DEBOUNCE_MS = 1500;     // one scan for a burst of kernel lines
static constexpr double GROUP_US = 300e6;    // the same event again within 5 minutes is one row (kernel µs)
static constexpr int POLL_ROOT_MS = 300000;  // restricted kernel log: pkexec, rarely
static constexpr int MAX_ROWS = 500;
static constexpr qint64 LOG_MAX_BYTES = 8 << 20;  // then health.log -> health.log.1
static constexpr qint64 TAIL_BYTES = 512 << 10;    // resume point: only the tail is scanned (same as KlogPage)

static QString helperPath() { return privileged::helperPath(QStringLiteral("tune-profile-helper")); }

static const char *levelColor(const QString &l) {
    if (l == QLatin1String("critical")) return theme::DANGER;
    if (l == QLatin1String("error")) return theme::WARN;
    return theme::FG_DIM;
}

static QSettings guiSettings() {
    return QSettings(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/centurion/gui.ini"), QSettings::IniFormat);
}

static QString uptime(quint64 us) {
    const quint64 s = us / 1000000;
    return QStringLiteral("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
}

/// Wall-clock time (ms since the epoch) of a kernel timestamp (µs since boot).
static qint64 wallMs(double tsUs) {
    timespec now{};
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    const qint64 bootMs = QDateTime::currentMSecsSinceEpoch() - (qint64(now.tv_sec) * 1000 + now.tv_nsec / 1000000);
    return bootMs + qint64(tsUs / 1000);
}

HealthTab::HealthTab(QWidget *parent) : QWidget(parent) {
    // Top level: Overview, Diagnostics, Power, Network, System info, Backup. Diagnostics, Power and System info
    // are groups (tab widgets of their own); showPage() reaches any page through them.
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *sub = new QTabWidget;
    sub_ = sub;
    outer->addWidget(sub);
    auto *diag = new QTabWidget;
    auto *power = new QTabWidget;
    auto *toolsTab = new QTabWidget;
    // Overview: one card per area, first. Its worst level feeds the Home badge and the tray.
    overview_ = new OverviewPage;
    connect(overview_, &OverviewPage::worstChanged, this, &HealthTab::levelChanged);
    // Diagnostics: the fault monitor, boot history, curve stability, kernel log.
    auto *monitor = new QWidget;
    diag->addTab(monitor, tr("Monitor"));
    // Boot history: loads when first shown; the start-up load below only feeds the "ended unexpectedly" band.
    boot_ = new BootPage;
    diag->addTab(boot_, tr("Boot history"));
    // Network: connections, IP blacklist, Wine/.exe guard. Built now (not lazily):
    // its inotify watch on the block log is what raises the tray alert.
    auto *net = new NetPage;
    connect(net, &NetPage::alert, this, [this](const QString &t, const QString &m) { Q_EMIT alert(notifyprefs::NETWORK, QString(), t, m); });
    // Curve stability: built now, its guard reacts to new kernel errors with the window hidden.
    auto *curves = new CurvePage;
    curves_ = curves;
    connect(curves, &CurvePage::alert, this, [this](const QString &t, const QString &m) { Q_EMIT alert(notifyprefs::CURVE, QString(), t, m); });
    connect(curves, &CurvePage::curveReset, this, &HealthTab::curveReset);
    connect(this, &HealthTab::fault, curves, &CurvePage::onFault);
    diag->addTab(curves, tr("Curve stability"));
    klog_ = new KlogPage;
    diag->addTab(klog_, tr("Kernel log"));
    // Power: sleep, battery, dGPU, cooling.
    power->addTab(new SleepPage, tr("Sleep"));
    if (BatteryPage::present()) {
        auto *battery = new BatteryPage;
        connect(battery, &BatteryPage::dgpuAwake, this, &HealthTab::dgpuAwake);
        power->addTab(battery, tr("Battery use"));
    }
    if (DgpuPage::present()) power->addTab(new DgpuPage, tr("dGPU power"));
    cooling_ = new CoolingPage;
    power->addTab(cooling_, tr("Cooling"));
    // System info: the SMART summary, then one page per system tool (lazy: a tool runs when its page opens).
    systools::addPages(toolsTab);
    // Backup: Centurion configuration + system image (tar). Idle until a button is pressed.
    backup_ = new BackupPage;
    connect(backup_, &BackupPage::alert, this, [this](const QString &t, const QString &m) { Q_EMIT alert(notifyprefs::BACKUP, QString(), t, m); });
    sub->addTab(overview_, tr("Overview"));
    sub->addTab(diag, tr("Diagnostics"));
    sub->addTab(power, tr("Power"));
    sub->addTab(net, tr("Network"));
    sub->addTab(toolsTab, tr("System info"));
    sub->addTab(backup_, tr("Backup"));
    // The sub-tab open last time, as "top,group" indexes in gui.ini (group -1: the top page is not a group).
    if (const QStringList last = guiSettings().value(QStringLiteral("health/tab")).toString().split(','); last.size() == 2) {
        const int top = last[0].toInt();
        if (top >= 0 && top < sub->count()) {
            sub->setCurrentIndex(top);
            auto *group = qobject_cast<QTabWidget *>(sub->currentWidget());
            const int page = last[1].toInt();
            if (group && page >= 0 && page < group->count()) group->setCurrentIndex(page);
        }
    }
    auto remember = [sub] {
        auto *group = qobject_cast<QTabWidget *>(sub->currentWidget());
        guiSettings().setValue(QStringLiteral("health/tab"), QStringLiteral("%1,%2").arg(sub->currentIndex()).arg(group ? group->currentIndex() : -1));
    };
    connect(sub, &QTabWidget::currentChanged, this, remember);
    for (QTabWidget *g : {diag, power, toolsTab}) connect(g, &QTabWidget::currentChanged, this, remember);
    // The Overview's "Show" buttons: the page they name, wherever it sits.
    connect(overview_, &OverviewPage::openPage, this, [monitor, this](const QString &target) {
        QWidget *page = nullptr;
        if (target == QLatin1String("monitor")) page = monitor;
        else if (target == QLatin1String("boot")) page = boot_;
        else if (target == QLatin1String("backup")) page = backup_;
        else if (target == QLatin1String("battery")) page = findChild<BatteryPage *>();
        else if (target == QLatin1String("cooling")) page = cooling_;
        else if (target == QLatin1String("dgpu")) page = findChild<DgpuPage *>();
        else if (target == QLatin1String("sleep")) page = findChild<SleepPage *>();
        showPage(page);
    });
    auto *root = new QVBoxLayout(monitor);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(6);

    // The previous boot never closed (crash, hang, power cut): one band, dismissible per boot.
    band_ = new QFrame;
    band_->setVisible(false);
    band_->setStyleSheet(QStringLiteral("QFrame{background:%1;border:1px solid %2;border-radius:6px}QLabel{border:none}").arg(theme::BG2, theme::DANGER_SOFT));
    auto *bandRow = new QHBoxLayout(band_);
    bandRow->setContentsMargins(8, 4, 8, 4);
    bandText_ = new QLabel;
    bandText_->setWordWrap(true);
    bandRow->addWidget(bandText_, 1);
    auto *bBandShow = new QPushButton(tr("Show"));
    connect(bBandShow, &QPushButton::clicked, this, [this] { showPage(boot_); });
    bandRow->addWidget(bBandShow);
    auto *bBandHide = new QPushButton(tr("Dismiss"));
    connect(bBandHide, &QPushButton::clicked, this, [this] {
        guiSettings().setValue(QStringLiteral("health/dismissed_unclean"), bandBoot_);
        band_->setVisible(false);
    });
    bandRow->addWidget(bBandHide);
    root->addWidget(band_);
    connect(boot_, &BootPage::previousUnclean, this, [this](const QString &id, qint64 end) {
        bandBoot_ = id;
        const bool show = !id.isEmpty() && guiSettings().value(QStringLiteral("health/dismissed_unclean")).toString() != id;
        if (show) bandText_->setText(end > 0 ? tr("The previous session ended unexpectedly (last seen alive %1).")
                                                   .arg(QDateTime::fromSecsSinceEpoch(end).toString(QStringLiteral("yyyy-MM-dd HH:mm")))
                                             : tr("The previous session ended unexpectedly."));
        band_->setVisible(show);
    });
    QTimer::singleShot(4000, boot_, &BootPage::reload);

    seenMs_ = guiSettings().value(QStringLiteral("health/seen_ms"), 0).toLongLong();
    auto *top = new QHBoxLayout;
    summary_ = new QLabel;
    summary_->setTextFormat(Qt::RichText);
    summary_->setWordWrap(true);  // up to 13 counters
    top->addWidget(summary_, 1);
    auto *bScan = new QPushButton(QStringLiteral("Scan now"));
    connect(bScan, &QPushButton::clicked, this, &HealthTab::scan);
    top->addWidget(bScan);
    auto *bSeen = new QPushButton(tr("Mark all seen"));
    bSeen->setToolTip(tr("The events shown so far stop counting as new (the Overview badge too). The list and the saved log stay as they are."));
    connect(bSeen, &QPushButton::clicked, this, &HealthTab::markSeen);
    top->addWidget(bSeen);
    auto *bNotify = new QPushButton(tr("Notifications…"));
    bNotify->setToolTip(tr("Which alerts reach the desktop, and whether they wait for the end of a game."));
    connect(bNotify, &QPushButton::clicked, this, [this] { NotifyDialog(this).exec(); });
    top->addWidget(bNotify);
    root->addLayout(top);

    source_ = new QLabel;
    source_->setWordWrap(true);
    source_->setStyleSheet(QStringLiteral("color:%1").arg(theme::MUTED));
    root->addWidget(source_);

    days_ = new DayBars;
    connect(days_, &DayBars::dayClicked, this, &HealthTab::filterDay);
    root->addWidget(days_);
    dayInfo_ = new QLabel;
    dayInfo_->setWordWrap(true);
    dayInfo_->setStyleSheet(QStringLiteral("color:%1").arg(theme::MUTED));
    root->addWidget(dayInfo_);

    list_ = new QTreeWidget;
    list_->setColumnCount(3);
    list_->setHeaderLabels({QStringLiteral("Uptime"), QStringLiteral("Kind"), QStringLiteral("Event")});
    list_->setRootIsDecorated(false);
    list_->setAlternatingRowColors(true);
    list_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    list_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    list_->header()->setStretchLastSection(true);
    // Double-click or right-click: the event in the kernel log, and the page its kind points to.
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list_, &QTreeWidget::customContextMenuRequested, this, &HealthTab::eventMenu);
    connect(list_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it, int) {
        showInKlog(QJsonDocument::fromJson(it->data(0, Qt::UserRole).toByteArray()).object());
    });
    root->addWidget(list_, 1);

    aer_ = new QLabel;
    aer_->setWordWrap(true);
    aer_->setTextFormat(Qt::RichText);
    root->addWidget(aer_);

    // Persistent log: every event, across boots, until cleared.
    auto *logRow = new QHBoxLayout;
    logInfo_ = new QLabel;
    logInfo_->setStyleSheet(QStringLiteral("color:%1").arg(theme::MUTED));
    logRow->addWidget(logInfo_, 1);
    auto *bOpen = new QPushButton(QStringLiteral("Open saved log"));
    connect(bOpen, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl::fromLocalFile(logPath())); });
    logRow->addWidget(bOpen);
    auto *bClear = new QPushButton(QStringLiteral("Clear saved log"));
    connect(bClear, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, QStringLiteral("Clear saved log"),
                QStringLiteral("Delete every saved kernel error (all boots)?")) != QMessageBox::Yes) return;
        QFile::remove(logPath());
        QFile::remove(logPath() + QStringLiteral(".1"));
        // Keep savedSeq_: events already seen this boot are not written again.
        updateLogInfo();
        refreshDays();
    });
    logRow->addWidget(bClear);
    root->addLayout(logRow);

    auto *hint = new QLabel(QStringLiteral(
        "Xid 154 only names the recovery the GPU needs — the cause is the Xid logged just before it. "
        "Xid 13/31/43/45 are usually the application (or an unstable GPU curve); 62/79/119/120 are "
        "firmware or power level. Machine checks and lockups point at CPU Curve Optimizer or RAM timings. "
        "Hover a row for the full kernel line."));
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:%1").arg(theme::MUTED));
    root->addWidget(hint);

    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(DEBOUNCE_MS);
    connect(debounce_, &QTimer::timeout, this, &HealthTab::scan);
    timer_ = new QTimer(this);
    timer_->setTimerType(Qt::VeryCoarseTimer);
    connect(timer_, &QTimer::timeout, this, &HealthTab::scan);

    // Wake only when the kernel logs something. SEEK_END: the start-up scan
    // below covers the history; the notifier only sees new records.
    kmsgFd_ = ::open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (kmsgFd_ >= 0) {
        ::lseek(kmsgFd_, 0, SEEK_END);
        auto *n = new QSocketNotifier(kmsgFd_, QSocketNotifier::Read, this);
        auto rate = std::make_shared<std::pair<QElapsedTimer, int>>();
        rate->first.start();
        connect(n, &QSocketNotifier::activated, this, [this, n, rate] {
            drainKmsg();
            if (rate->first.elapsed() > 1000) { rate->first.restart(); rate->second = 0; }
            if (++rate->second > 50) {      // fd stays readable: pause, scan once, resume later
                n->setEnabled(false);
                debounce_->start();
                QTimer::singleShot(5000, n, [this, n, rate] { drainKmsg(); rate->first.restart(); rate->second = 0; n->setEnabled(true); });
            }
        });
    } else {
        needsRoot_ = true;
        curves_->setKernelLog(false);
        timer_->start(POLL_ROOT_MS);
    }
    // Resume point: highest seq of the current boot already in the file, so a
    // restart does not write this boot's history twice.
    {
        QFile b(QStringLiteral("/proc/sys/kernel/random/boot_id"));
        if (b.open(QIODevice::ReadOnly)) savedBoot_ = QString::fromLatin1(b.readAll().trimmed());
        QFile f(logPath());
        const QString tag = QStringLiteral("boot=") + savedBoot_.left(8) + QStringLiteral(" seq=");
        if (!savedBoot_.isEmpty() && f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            // The whole file (up to 8 MiB) used to be decoded line by line on the GUI thread at every start.
            if (f.size() > TAIL_BYTES) { f.seek(f.size() - TAIL_BYTES); f.readLine(); }
            while (!f.atEnd()) {
                const QString l = QString::fromUtf8(f.readLine());
                const int i = l.indexOf(tag);
                if (i >= 0) savedSeq_ = std::max<quint64>(savedSeq_, l.mid(i + tag.size()).section(QLatin1Char(' '), 0, 0).toULongLong());
            }
        }
    }
    updateLogInfo();
    QTimer::singleShot(3000, this, &HealthTab::scan);
    // Overview: a first reading once the window is up; later ones on show, on faults and on Refresh.
    QTimer::singleShot(8000, overview_, &OverviewPage::refresh);
    connect(this, &HealthTab::fault, overview_, [o = overview_](const QJsonObject &) { o->scheduleRefresh(); });
    QTimer::singleShot(3500, this, &HealthTab::refreshDays);
    updateSummary();
}

HealthTab::~HealthTab() { if (kmsgFd_ >= 0) ::close(kmsgFd_); }

int HealthTab::worstLevel() const { return overview_->worst(); }

QList<QVariantMap> HealthTab::overviewCards() const {
    QList<QVariantMap> out;
    for (const QJsonValue &v : overview_->cards()) out << v.toObject().toVariantMap();
    return out;
}

QList<QVariantMap> HealthTab::eventsSince(quint64 seq) const {
    QList<QVariantMap> out;
    for (int i = list_->topLevelItemCount() - 1; i >= 0; --i) {  // the list is newest first
        const QJsonObject e = QJsonDocument::fromJson(list_->topLevelItem(i)->data(0, Qt::UserRole).toByteArray()).object();
        const quint64 s = quint64(e.value(QStringLiteral("seq")).toDouble());
        if (s <= seq) continue;
        out << QVariantMap{{QStringLiteral("seq"), qulonglong(s)}, {QStringLiteral("kind"), e.value(QStringLiteral("kind")).toString()},
                           {QStringLiteral("level"), e.value(QStringLiteral("level")).toString()},
                           {QStringLiteral("title"), e.value(QStringLiteral("title")).toString()},
                           {QStringLiteral("boot_id"), e.value(QStringLiteral("boot_id")).toString()}};
    }
    return out;
}

void HealthTab::drainKmsg() {
    // Must cover every kind health.rs::classify knows: a line that is not listed here never starts a scan.
    // ";xe " / ";BUG:" ... are anchored at the start of the message (the record is "pri,seq,ts,flags;message").
    static const char *const KEYS[] = {"NVRM", "Hardware Error", "achine check", "achine Check", "AER",
                                       "PCIe Bus Error", "lockup", "LOCKUP", "stall", "amdgpu",
                                       ";nvme nvme", "EXT4-fs error", "BTRFS error", "csum failed", ";XFS", "GPU HANG", ";xe ",
                                       "Microcode SW error", "mt79", "firmware crashed", "critical temperature",
                                       "Package temperature above", "Out of memory", ";Oops:", ";BUG:", ";WARNING: CPU:"};
    char buf[8192];
    bool relevant = false;
    for (int i = 0; i < 512; ++i) {  // bounded: a flood must not pin the GUI thread
        const ssize_t n = ::read(kmsgFd_, buf, sizeof buf - 1);
        if (n < 0 && errno == EPIPE) continue;  // record overwritten meanwhile
        if (n <= 0) break;                      // EAGAIN: drained
        if (relevant) continue;
        buf[n] = 0;
        for (const char *k : KEYS) if (std::strstr(buf, k)) { relevant = true; break; }
    }
    if (relevant) debounce_->start();
}

void HealthTab::scan() {
    if (busy_) return;
    if (!QFileInfo(helperPath()).isExecutable()) {
        source_->setText(QStringLiteral("tune-helper is not installed."));
        return;
    }
    busy_ = true;
    const QJsonObject req{{QStringLiteral("op"), QStringLiteral("health")}, {QStringLiteral("since"), qint64(lastSeq_)}};
    if (needsRoot_) {
        // health-reader: read-only, classified events only, silent polkit action at every
        // security level -- this runs at start-up and on a timer with the window hidden.
        privileged::run(privileged::helperPath(QStringLiteral("health-reader")), req, this, [this](const privileged::Result &r) {
            busy_ = false;
            if (!r.reached) { source_->setText(QStringLiteral("Kernel log restricted and pkexec failed: ") + r.message()); return; }
            onReply(r.json, true);
        });
        return;
    }
    auto *p = new QProcess(this);
    QPointer<QProcess> guard(p);
    connect(p, &QProcess::finished, this, [this, p](int, QProcess::ExitStatus) {
        busy_ = false;
        const QByteArray out = p->readAllStandardOutput().trimmed();
        p->deleteLater();
        onReply(QJsonDocument::fromJson(out.mid(out.lastIndexOf('\n') + 1)).object(), false);
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        busy_ = false;
        p->deleteLater();
    });
    QTimer::singleShot(10000, p, [guard] { if (guard && guard->state() != QProcess::NotRunning) guard->kill(); });
    p->start(helperPath(), {});
    p->write(QJsonDocument(req).toJson(QJsonDocument::Compact));
    p->closeWriteChannel();
}

void HealthTab::onReply(const QJsonObject &r, bool viaRoot) {
    if (!r.value(QStringLiteral("ok")).toBool()) {
        if (r.value(QStringLiteral("needs_root")).toBool() && !needsRoot_) {
            needsRoot_ = true;
            curves_->setKernelLog(false);
            timer_->start(POLL_ROOT_MS);
            QTimer::singleShot(0, this, &HealthTab::scan);
            return;
        }
        source_->setText(QStringLiteral("Scan failed: ") + r.value(QStringLiteral("error")).toString());
        return;
    }
    source_->setText(viaRoot
        ? QStringLiteral("Kernel log is restricted (kernel.dmesg_restrict=1): read through pkexec every %1 min. "
                         "sysctl kernel.dmesg_restrict=0 allows direct, faster checks.").arg(POLL_ROOT_MS / 60000)
        : QStringLiteral("Watching the kernel log directly (scans only when a relevant line appears)."));

    const QJsonArray ev = r.value(QStringLiteral("events")).toArray();
    const QString bootId = r.value(QStringLiteral("boot_id")).toString();
    int newBad = 0;
    bool newCritical = false;
    QString firstBad;
    for (const QJsonValue &v : ev) {
        const QJsonObject e = v.toObject();
        addEvent(e, bootId);
        if (!first_) Q_EMIT fault(e);
        const QString lvl = e.value(QStringLiteral("level")).toString();
        if (lvl == QLatin1String("critical") || lvl == QLatin1String("error")) {
            if (lvl == QLatin1String("critical")) newCritical = true;
            if (!newBad++) firstBad = e.value(QStringLiteral("title")).toString();
        }
    }
    persist(ev, r.value(QStringLiteral("boot_id")).toString());
    lastSeq_ = std::max<quint64>(lastSeq_, quint64(r.value(QStringLiteral("last_seq")).toDouble()));

    if (newBad) {
        const QString severity = newCritical ? notifyprefs::CRITICAL_LEVEL : notifyprefs::ERROR_LEVEL;
        if (first_) Q_EMIT alert(notifyprefs::KERNEL, severity, QStringLiteral("Health"), QStringLiteral("%1 hardware/driver error(s) since boot — see the Health tab.").arg(newBad));
        else Q_EMIT alert(notifyprefs::KERNEL, severity, QStringLiteral("Health"), newBad == 1 ? firstBad : QStringLiteral("%1 new errors — %2").arg(newBad).arg(firstBad));
    }
    first_ = false;

    QStringList aer;
    for (const QJsonValue &v : r.value(QStringLiteral("aer")).toArray()) {
        const QJsonObject a = v.toObject();
        const qint64 bad = a.value(QStringLiteral("nonfatal")).toInteger() + a.value(QStringLiteral("fatal")).toInteger();
        aer << QStringLiteral("<span style='color:%1'>%2 %3: %4 corrected, %5 uncorrectable</span>")
                   .arg(QString::fromLatin1(bad ? theme::DANGER : theme::FG_DIM))
                   .arg(a.value(QStringLiteral("what")).toString())
                   .arg(a.value(QStringLiteral("dev")).toString())
                   .arg(a.value(QStringLiteral("cor")).toInteger()).arg(bad);
    }
    aer_->setText(aer.isEmpty() ? QStringLiteral("<span style='color:%1'>PCIe AER counters: all zero.</span>").arg(theme::MUTED)
                                : QStringLiteral("PCIe AER counters (since boot):<br>") + aer.join(QStringLiteral("<br>")));
    updateSummary();
    applyDayFilter();
    if (!ev.isEmpty()) refreshDays();
}

void HealthTab::addEvent(const QJsonObject &e, const QString &bootId) {
    const QString kind = e.value(QStringLiteral("kind")).toString();
    const QString rawTitle = e.value(QStringLiteral("title")).toString();
    const int code = e.value(QStringLiteral("code")).toInt();
    const double tsUs = e.value(QStringLiteral("ts_us")).toDouble();
    QString title = rawTitle;
    int *counter = &other_;
    if (kind == QLatin1String("xid")) {
        counter = &xid_;
        if (code == 154) {
            if (!lastRootXid_.isEmpty()) title += QStringLiteral("  (cause: ") + lastRootXid_ + QLatin1Char(')');
        } else {
            lastRootXid_ = title;
        }
    } else if (kind == QLatin1String("gsp")) counter = &gsp_;
    else if (kind == QLatin1String("mce")) counter = &mce_;
    else if (kind == QLatin1String("aer")) counter = &aerN_;
    else if (kind == QLatin1String("lockup")) counter = &lockup_;
    else if (kind == QLatin1String("nvme")) counter = &nvme_;
    else if (kind == QLatin1String("fs")) counter = &fs_;
    else if (kind == QLatin1String("igpu")) counter = &igpu_;
    else if (kind == QLatin1String("wifi")) counter = &wifi_;
    else if (kind == QLatin1String("thermal")) counter = &thermal_;
    else if (kind == QLatin1String("oom")) counter = &oom_;
    else if (kind == QLatin1String("oops")) counter = &oops_;
    if (wallMs(tsUs) > seenMs_) ++*counter;  // only what is not marked seen counts

    // The same event again (same boot, kind, code and raw title) within GROUP_US: one row, with a count.
    const QString key = bootId + QLatin1Char('|') + kind + QLatin1Char('|') + QString::number(code) + QLatin1Char('|') + rawTitle;
    auto g = groups_.find(key);
    if (g != groups_.end() && tsUs - g->lastUs <= GROUP_US && list_->indexOfTopLevelItem(g->row) >= 0) {
        g->lastUs = tsUs;
        ++g->n;
        list_->insertTopLevelItem(0, list_->takeTopLevelItem(list_->indexOfTopLevelItem(g->row)));  // newest first
        g->row->setText(0, uptime(quint64(g->firstUs)) + QStringLiteral(" – ") + uptime(quint64(tsUs)));
        g->row->setText(2, g->base + QStringLiteral("  ×%1").arg(g->n));
        return;
    }

    auto *it = new QTreeWidgetItem({uptime(quint64(tsUs)), kind.toUpper(), title});
    it->setToolTip(2, e.value(QStringLiteral("text")).toString());
    // The record itself (with its boot and seq) travels with the row: the menu and double-click need them.
    QJsonObject stored = e;
    stored.insert(QStringLiteral("boot_id"), bootId);
    it->setData(0, Qt::UserRole, QJsonDocument(stored).toJson(QJsonDocument::Compact));
    it->setData(0, Qt::UserRole + 1, key);  // its group, so a row dropped below forgets it
    it->setData(0, Qt::UserRole + 2, QDateTime::fromMSecsSinceEpoch(wallMs(tsUs)).date().toString(Qt::ISODate));  // its day, for the day chart
    const QColor c(levelColor(e.value(QStringLiteral("level")).toString()));
    for (int i = 0; i < 3; ++i) it->setForeground(i, c);
    list_->insertTopLevelItem(0, it);  // newest first
    groups_[key] = Group{it, title, tsUs, tsUs, 1};
    while (list_->topLevelItemCount() > MAX_ROWS) {
        QTreeWidgetItem *old = list_->takeTopLevelItem(list_->topLevelItemCount() - 1);
        const QString oldKey = old->data(0, Qt::UserRole + 1).toString();
        if (groups_.value(oldKey).row == old) groups_.remove(oldKey);
        delete old;
    }
}

void HealthTab::markSeen() {
    seenMs_ = QDateTime::currentMSecsSinceEpoch();
    guiSettings().setValue(QStringLiteral("health/seen_ms"), seenMs_);
    xid_ = gsp_ = mce_ = aerN_ = lockup_ = other_ = 0;
    nvme_ = fs_ = igpu_ = wifi_ = thermal_ = oom_ = oops_ = 0;
    updateSummary();
    overview_->refresh();  // the Overview cards, and the Home badge through them, count the same way
}

void HealthTab::showPage(QWidget *page) {
    if (!page) return;
    for (QTabWidget *group : findChildren<QTabWidget *>()) {
        if (group != sub_ && group->indexOf(page) >= 0) {
            group->setCurrentWidget(page);
            sub_->setCurrentWidget(group);
            return;
        }
    }
    sub_->setCurrentWidget(page);
}

void HealthTab::openDgpuPage() { showPage(findChild<DgpuPage *>()); }

void HealthTab::refreshDays() {
    userhelper::run(QJsonObject{{QStringLiteral("op"), QStringLiteral("event_days")}}, this, [this](const QJsonObject &r) {
        QVector<DayBars::Day> days;
        for (const QJsonValue &v : r.value(QStringLiteral("days")).toArray()) {
            const QJsonObject o = v.toObject();
            DayBars::Day d{o.value(QStringLiteral("d")).toString(), {}};
            const QJsonObject kinds = o.value(QStringLiteral("kinds")).toObject();
            for (auto it = kinds.constBegin(); it != kinds.constEnd(); ++it) d.kinds.insert(it.key(), it.value().toInt());
            days << d;
        }
        days_->setVisible(!days.isEmpty());
        dayInfo_->setVisible(!days.isEmpty());
        days_->setDays(days);
    });
}

void HealthTab::filterDay(const QString &date) {
    dayFilter_ = dayFilter_ == date ? QString() : date;
    days_->setSelected(dayFilter_);
    applyDayFilter();
}

void HealthTab::applyDayFilter() {
    int shown = 0;
    for (int i = 0; i < list_->topLevelItemCount(); ++i) {
        QTreeWidgetItem *it = list_->topLevelItem(i);
        const bool hidden = !dayFilter_.isEmpty() && it->data(0, Qt::UserRole + 2).toString() != dayFilter_;
        it->setHidden(hidden);
        if (!hidden) ++shown;
    }
    if (dayFilter_.isEmpty()) dayInfo_->setText(tr("Last 30 days. Click a day to show only its events from this session."));
    else if (!shown) dayInfo_->setText(tr("%1: no events from this session. Older days are in the saved log.").arg(dayFilter_));
    else dayInfo_->setText(tr("%1: %2 rows from this session. Click the day again to show all.").arg(dayFilter_).arg(shown));
}

void HealthTab::showInKlog(const QJsonObject &e) {
    klog_->showAt(e.value(QStringLiteral("boot_id")).toString(), quint64(e.value(QStringLiteral("seq")).toDouble()));
    showPage(klog_);
}

void HealthTab::eventMenu(const QPoint &pos) {
    QTreeWidgetItem *it = list_->itemAt(pos);
    if (!it) return;
    const QJsonObject e = QJsonDocument::fromJson(it->data(0, Qt::UserRole).toByteArray()).object();
    const QString kind = e.value(QStringLiteral("kind")).toString();
    const int code = e.value(QStringLiteral("code")).toInt();
    const QString title = e.value(QStringLiteral("title")).toString();
    QMenu menu(this);
    menu.addAction(tr("Show in kernel log"), this, [this, e] { showInKlog(e); });
    // The page the event points to, only when there is something to do there now.
    static const QList<int> gpuCurveXids{13, 31, 43, 62, 119, 120};
    if (kind == QLatin1String("d3cold") || (kind == QLatin1String("xid") && code == 79)) {
        if (auto *dgpu = findChild<DgpuPage *>())
            menu.addAction(tr("Open dGPU power"), this, [this, dgpu] { showPage(dgpu); });
    } else if (kind == QLatin1String("xid") && gpuCurveXids.contains(code) && CurvePage::currentState(QStringLiteral("gpu")) != QLatin1String("stock")) {
        menu.addAction(tr("Reset GPU curve to stock"), this, [this, title] { curves_->resetGpuCurve(title); });
    } else if ((kind == QLatin1String("mce") || kind == QLatin1String("lockup")) && CurvePage::currentState(QStringLiteral("cpu")) != QLatin1String("stock")) {
        menu.addAction(tr("Open the CPU curve tab"), this, [this] { Q_EMIT openTab(sysinfo::isIntel() ? QStringLiteral("Intel Undervolt") : QStringLiteral("Ryzen Curve")); });
    } else if (kind == QLatin1String("nvme") || kind == QLatin1String("fs")) {
        if (auto *smart = findChild<SmartPage *>()) menu.addAction(tr("Open SMART"), this, [this, smart] { showPage(smart); });
    }
    menu.exec(list_->viewport()->mapToGlobal(pos));
}

void HealthTab::updateSummary() {
    auto part = [](const QString &name, int n) {
        return QStringLiteral("<span style='color:%1'><b>%2</b> %3</span>").arg(n ? theme::DANGER : theme::OK).arg(n).arg(name);
    };
    QStringList parts{
        part(QStringLiteral("NVIDIA Xid"), xid_), part(QStringLiteral("GSP timeout"), gsp_), part(QStringLiteral("machine check"), mce_),
        part(QStringLiteral("PCIe AER"), aerN_), part(QStringLiteral("lockup"), lockup_), part(QStringLiteral("other"), other_)};
    // The newer kinds appear only once seen: a permanent green "0 NVMe" on a machine without one is noise.
    const std::pair<QString, int> extra[] = {
        {tr("NVMe"), nvme_}, {tr("file system"), fs_}, {tr("Intel iGPU"), igpu_}, {tr("Wi-Fi"), wifi_},
        {tr("thermal"), thermal_}, {tr("out of memory"), oom_}, {tr("kernel oops/bug"), oops_}};
    for (const auto &[name, n] : extra) if (n) parts << part(name, n);
    summary_->setText((seenMs_ ? tr("Not seen yet: ") : tr("Since boot: ")) + parts.join(QStringLiteral(" &nbsp;·&nbsp; ")));
}

QString HealthTab::logPath() {
    QString dir = qEnvironmentVariable("XDG_STATE_HOME");  // GenericStateLocation needs Qt 6.7
    if (dir.isEmpty()) dir = QDir::homePath() + QStringLiteral("/.local/state");
    return dir + QStringLiteral("/centurion/health.log");
}

void HealthTab::persist(const QJsonArray &ev, const QString &bootId) {
    if (!bootId.isEmpty() && bootId != savedBoot_) { savedBoot_ = bootId; savedSeq_ = 0; }
    QStringList lines;
    // Kernel timestamps count from boot; wall time = now - uptime + ts.
    timespec now{};
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    const qint64 bootMs = QDateTime::currentMSecsSinceEpoch() - (qint64(now.tv_sec) * 1000 + now.tv_nsec / 1000000);
    quint64 top = savedSeq_;
    for (const QJsonValue &v : ev) {
        const QJsonObject e = v.toObject();
        const quint64 seq = quint64(e.value(QStringLiteral("seq")).toDouble());
        if (seq <= savedSeq_) continue;
        top = std::max(top, seq);
        const qint64 tsMs = qint64(e.value(QStringLiteral("ts_us")).toDouble() / 1000);
        lines << QStringLiteral("%1  boot=%2 seq=%3  %4 %5  %6  |  %7")
                     .arg(QDateTime::fromMSecsSinceEpoch(bootMs + tsMs).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                     .arg(savedBoot_.left(8)).arg(seq)
                     .arg(e.value(QStringLiteral("level")).toString().toUpper(), -8)
                     .arg(e.value(QStringLiteral("kind")).toString().toUpper(), -6)
                     .arg(e.value(QStringLiteral("title")).toString(), e.value(QStringLiteral("text")).toString());
    }
    if (lines.isEmpty()) return;
    const QString path = logPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    if (QFileInfo(path).size() > LOG_MAX_BYTES) { QFile::remove(path + QStringLiteral(".1")); QFile::rename(path, path + QStringLiteral(".1")); }
    QFile f(path);
    if (!f.open(QIODevice::Append | QIODevice::Text)) { logInfo_->setText(QStringLiteral("Saved log: cannot write ") + path); return; }
    QTextStream(&f) << lines.join(QLatin1Char('\n')) << '\n';
    savedSeq_ = top;
    updateLogInfo();
}

void HealthTab::updateLogInfo() {
    const QFileInfo a(logPath()), b(logPath() + QStringLiteral(".1"));
    const qint64 sz = (a.exists() ? a.size() : 0) + (b.exists() ? b.size() : 0);
    logInfo_->setText(sz ? QStringLiteral("Saved log (all boots): %1 · %2 KiB").arg(a.absoluteFilePath()).arg((sz + 1023) / 1024)
                         : QStringLiteral("Saved log: empty — new kernel errors are appended to %1").arg(a.absoluteFilePath()));
}
