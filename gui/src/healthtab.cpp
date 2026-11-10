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
#include <QStackedWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QSplitter>
#include <QRegularExpression>
#include <QSet>
#include <QFont>
#include <QApplication>
#include <QClipboard>
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
#include <QSignalBlocker>

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
    // A single, searchable navigation tree replaces nested tab bars. All original
    // functional pages remain; tool commands stay lazy until their page is opened.
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    // Studio keeps the same searchable section list as Classic: one click per
    // page, every page visible at once. Only its frame follows the Studio rail.
    auto *split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);
    outer->addWidget(split);
    auto *rail = new QWidget;
    rail->setMinimumWidth(158);
    rail->setMaximumWidth(230);
    auto *railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(6, 7, 5, 6);
    auto *railTitle = new QLabel(tr("HEALTH CENTER"));
    railTitle->setStyleSheet(QStringLiteral("color:%1;font-weight:700;letter-spacing:1px;padding:5px").arg(theme::FG_DIM));
    railLayout->addWidget(railTitle);
    railTitle->setVisible(!theme::experimentalEnabled());  // Studio: the top bar already names the page
    auto *navSearch = new QLineEdit;
    navSearch->setPlaceholderText(tr("Find a health tool…"));
    navSearch->setClearButtonEnabled(true);
    navSearch->setObjectName(QStringLiteral("healthNavSearch"));
    railLayout->addWidget(navSearch);
    navigation_ = new QTreeWidget;
    navigation_->setHeaderHidden(true);
    navigation_->setRootIsDecorated(false);
    navigation_->setIndentation(12);
    navigation_->setUniformRowHeights(true);
    navigation_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    navigation_->setFrameShape(QFrame::NoFrame);
    navigation_->setStyleSheet(QStringLiteral("QTreeWidget{background:transparent;border:0}QTreeWidget::item{padding:3px 3px}"));
    railLayout->addWidget(navigation_, 1);
    split->addWidget(rail);
    pages_ = new QStackedWidget;
    split->addWidget(pages_);
    split->setStretchFactor(1, 1);
    split->setSizes({178, 880});

    auto addGroup = [this](const QString &label) {
        auto *group = new QTreeWidgetItem(navigation_, {label.toUpper()});
        group->setFlags(group->flags() & ~Qt::ItemIsSelectable);
        group->setForeground(0, QColor(theme::MUTED));
        QFont font = group->font(0);
        font.setBold(true);
        font.setPointSize(std::max(8, font.pointSize() - 1));
        group->setFont(0, font);
        group->setExpanded(true);
        return group;
    };
    auto addPage = [this](QTreeWidgetItem *group, QWidget *page, const QString &label, const QString &key) {
        pages_->addWidget(page);
        auto *item = new QTreeWidgetItem(group, {label});
        item->setData(0, Qt::UserRole, key);
        pageItems_.insert(page, item);
    };
    auto *general = addGroup(tr("At a glance"));
    auto *problems = addGroup(tr("Problems"));
    auto *hardware = addGroup(tr("Hardware"));
    auto *maintenance = addGroup(tr("Maintenance"));
    auto *advanced = addGroup(tr("Advanced"));

    overview_ = new OverviewPage;
    connect(overview_, &OverviewPage::worstChanged, this, &HealthTab::levelChanged);
    addPage(general, overview_, tr("System overview"), QStringLiteral("overview"));
    auto *monitor = new QWidget;
    addPage(problems, monitor, tr("Events & alerts"), QStringLiteral("monitor"));
    boot_ = new BootPage;
    addPage(problems, boot_, tr("Boot history"), QStringLiteral("boot"));

    auto *curves = new CurvePage;
    curves_ = curves;
    connect(curves, &CurvePage::alert, this, [this](const QString &t, const QString &m) { Q_EMIT alert(notifyprefs::CURVE, QString(), t, m); });
    connect(curves, &CurvePage::curveReset, this, &HealthTab::curveReset);
    connect(this, &HealthTab::fault, curves, &CurvePage::onFault);
    addPage(advanced, curves, tr("Curve stability"), QStringLiteral("curves"));
    klog_ = new KlogPage;
    addPage(advanced, klog_, tr("Kernel log"), QStringLiteral("klog"));
    auto *sleep = new SleepPage;
    addPage(hardware, sleep, tr("Sleep & wake"), QStringLiteral("sleep"));
    if (BatteryPage::present()) {
        auto *battery = new BatteryPage;
        connect(battery, &BatteryPage::dgpuAwake, this, &HealthTab::dgpuAwake);
        addPage(hardware, battery, tr("Battery use"), QStringLiteral("battery"));
    }
    if (DgpuPage::present()) addPage(hardware, new DgpuPage, tr("dGPU power"), QStringLiteral("dgpu"));
    cooling_ = new CoolingPage;
    addPage(hardware, cooling_, tr("Cooling"), QStringLiteral("cooling"));

    // A searchable catalog keeps all 16+ specialist tools available without
    // making the primary health navigation feel like a configuration console.
    toolDirectory_ = new QWidget;
    auto *directoryLayout = new QVBoxLayout(toolDirectory_);
    directoryLayout->setContentsMargins(10, 9, 10, 9);
    directoryLayout->setSpacing(9);
    auto *directoryHeader = new QLabel(tr("System tools"));
    QFont directoryFont = directoryHeader->font();
    directoryFont.setBold(true);
    directoryFont.setPointSize(directoryFont.pointSize() + 4);
    directoryHeader->setFont(directoryFont);
    directoryLayout->addWidget(directoryHeader);
    auto *directoryHint = new QLabel(tr("Explore detailed device reports when needed. Tools run only when opened; "
                                        "some may request privileges or wake a sleeping GPU."));
    directoryHint->setWordWrap(true);
    directoryHint->setProperty("role", "muted");
    directoryLayout->addWidget(directoryHint);
    auto *toolSearch = new QLineEdit;
    toolSearch->setPlaceholderText(tr("Search all system tools…"));
    toolSearch->setClearButtonEnabled(true);
    toolSearch->setObjectName(QStringLiteral("healthToolSearch"));
    directoryLayout->addWidget(toolSearch);
    auto *toolSplit = new QSplitter(Qt::Horizontal);
    toolSplit->setChildrenCollapsible(false);
    toolNavigation_ = new QTreeWidget;
    toolNavigation_->setHeaderHidden(true);
    toolNavigation_->setRootIsDecorated(false);
    toolNavigation_->setIndentation(12);
    toolNavigation_->setMinimumWidth(150);
    toolNavigation_->setMaximumWidth(260);
    toolNavigation_->setStyleSheet(QStringLiteral("QTreeWidget::item{padding:3px 3px}"));
    toolPages_ = new QStackedWidget;
    toolSplit->addWidget(toolNavigation_);
    toolSplit->addWidget(toolPages_);
    toolSplit->setStretchFactor(1, 1);
    toolSplit->setSizes({195, 640});
    directoryLayout->addWidget(toolSplit, 1);
    addPage(advanced, toolDirectory_, tr("System tools"), QStringLiteral("tools"));
    auto *deviceTools = new QTreeWidgetItem(toolNavigation_, {tr("Devices")});
    auto *connectivityTools = new QTreeWidgetItem(toolNavigation_, {tr("Connections")});
    auto *advancedTools = new QTreeWidgetItem(toolNavigation_, {tr("Kernel & firmware")});
    for (QTreeWidgetItem *group : {deviceTools, connectivityTools, advancedTools}) {
        group->setFlags(group->flags() & ~Qt::ItemIsSelectable);
        group->setForeground(0, QColor(theme::MUTED));
        group->setExpanded(true);
    }
    QTabWidget toolsRegistry;
    systools::addPages(&toolsRegistry);
    QWidget *firstTool = nullptr;
    QWidget *smartTool = nullptr;
    while (toolsRegistry.count()) {
        QWidget *page = toolsRegistry.widget(0);
        const QString label = toolsRegistry.tabText(0);
        toolsRegistry.removeTab(0);
        QTreeWidgetItem *group = advancedTools;
        if (label == QLatin1String("SMART") || label == QLatin1String("SMART details") ||
            label == QLatin1String("Sensors") || label == QLatin1String("CPU") ||
            label == QLatin1String("Memory") || label == QLatin1String("Storage") ||
            label == QLatin1String("Thermal") || label == QLatin1String("Battery") ||
            label == QLatin1String("NVIDIA") || label == QLatin1String("Graphics")) group = deviceTools;
        else if (label == QLatin1String("PCI") || label == QLatin1String("PCIe detail") ||
                 label == QLatin1String("USB")) group = connectivityTools;
        auto *item = new QTreeWidgetItem(group, {label});
        toolItems_.insert(page, item);
        toolPages_->addWidget(page);
        if (!firstTool) firstTool = page;
        if (label == QLatin1String("SMART")) smartTool = page;
    }
    if (smartTool) {
        // The drive summary is a frequent action; give it a direct shortcut
        // while keeping the complete device tool catalog available.
        toolPages_->removeWidget(smartTool);
        toolItems_.remove(smartTool);
        for (int i = deviceTools->childCount() - 1; i >= 0; --i) {
            if (deviceTools->child(i)->text(0) == QLatin1String("SMART")) delete deviceTools->takeChild(i);
        }
        addPage(hardware, smartTool, tr("Drive health"), QStringLiteral("smart"));
        if (firstTool == smartTool) firstTool = toolPages_->count() ? toolPages_->widget(0) : nullptr;
    }
    if (firstTool) {
        toolPages_->setCurrentWidget(firstTool);
        toolNavigation_->setCurrentItem(toolItems_.value(firstTool));
    }
    connect(toolNavigation_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item, QTreeWidgetItem *) {
        if (!item || !item->parent()) return;
        for (auto it = toolItems_.cbegin(); it != toolItems_.cend(); ++it) {
            if (it.value() == item) { toolPages_->setCurrentWidget(it.key()); break; }
        }
    });
    connect(toolSearch, &QLineEdit::textChanged, this, [this](const QString &query) {
        for (int i = 0; i < toolNavigation_->topLevelItemCount(); ++i) {
            auto *group = toolNavigation_->topLevelItem(i);
            int visible = 0;
            for (int n = 0; n < group->childCount(); ++n) {
                auto *item = group->child(n);
                const bool match = query.isEmpty() || item->text(0).contains(query, Qt::CaseInsensitive) ||
                                    group->text(0).contains(query, Qt::CaseInsensitive);
                item->setHidden(!match);
                if (match) ++visible;
            }
            group->setHidden(!visible);
            if (!query.isEmpty()) group->setExpanded(true);
        }
    });
    auto *net = new NetPage;
    connect(net, &NetPage::alert, this, [this](const QString &t, const QString &m) { Q_EMIT alert(notifyprefs::NETWORK, QString(), t, m); });
    addPage(maintenance, net, tr("Network protection"), QStringLiteral("network"));
    backup_ = new BackupPage;
    connect(backup_, &BackupPage::alert, this, [this](const QString &t, const QString &m) { Q_EMIT alert(notifyprefs::BACKUP, QString(), t, m); });
    addPage(maintenance, backup_, tr("Backups"), QStringLiteral("backup"));
    connect(navigation_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *item, QTreeWidgetItem *) {
        if (!item || !item->parent()) return;
        for (auto it = pageItems_.cbegin(); it != pageItems_.cend(); ++it) {
            if (it.value() == item) {
                pages_->setCurrentWidget(it.key());
                guiSettings().setValue(QStringLiteral("health/page"), item->data(0, Qt::UserRole).toString());
                break;
            }
        }
    });
    connect(navSearch, &QLineEdit::textChanged, this, [this](const QString &query) {
        for (int i = 0; i < navigation_->topLevelItemCount(); ++i) {
            auto *group = navigation_->topLevelItem(i);
            int visible = 0;
            for (int n = 0; n < group->childCount(); ++n) {
                auto *item = group->child(n);
                const bool match = query.isEmpty() || item->text(0).contains(query, Qt::CaseInsensitive) ||
                                    group->text(0).contains(query, Qt::CaseInsensitive);
                item->setHidden(!match);
                if (match) ++visible;
            }
            group->setHidden(!visible);
            if (!query.isEmpty()) group->setExpanded(true);
        }
    });
    const QString lastPage = guiSettings().value(QStringLiteral("health/page"), QStringLiteral("overview")).toString();
    QTreeWidgetItem *initial = pageItems_.value(overview_);
    for (auto it = pageItems_.cbegin(); it != pageItems_.cend(); ++it) {
        if (it.value()->data(0, Qt::UserRole).toString() == lastPage) { initial = it.value(); break; }
    }
    navigation_->setCurrentItem(initial);
    // currentItemChanged selects the page synchronously; no tool is run in the background.

    connect(overview_, &OverviewPage::openPage, this, [monitor, this](const QString &target) {
        QWidget *page = nullptr;
        if (target == QLatin1String("monitor")) page = monitor;
        else if (target == QLatin1String("boot")) page = boot_;
        else if (target == QLatin1String("backup")) page = backup_;
        else if (target == QLatin1String("battery")) page = findChild<BatteryPage *>();
        else if (target == QLatin1String("cooling")) page = cooling_;
        else if (target == QLatin1String("dgpu")) page = findChild<DgpuPage *>();
        else if (target == QLatin1String("sleep")) page = findChild<SleepPage *>();
        else if (target == QLatin1String("klog")) page = klog_;
        else if (target == QLatin1String("tools")) page = toolDirectory_;
        else if (target == QLatin1String("smart")) {
            for (auto it = pageItems_.cbegin(); it != pageItems_.cend(); ++it)
                if (it.value()->data(0, Qt::UserRole).toString() == QStringLiteral("smart")) {
                    page = it.key(); break;
                }
        }
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
    summary_->setWordWrap(true);
    QFont monitorFont = summary_->font();
    monitorFont.setBold(true);
    monitorFont.setPointSize(monitorFont.pointSize() + 2);
    summary_->setFont(monitorFont);
    top->addWidget(summary_, 1);
    auto *bScan = new QPushButton(tr("Check now"));
    connect(bScan, &QPushButton::clicked, this, &HealthTab::scan);
    top->addWidget(bScan);
    auto *bSeen = new QPushButton(tr("Mark reviewed"));
    bSeen->setToolTip(tr("Acknowledge existing event notifications without deleting logs or fixing the underlying causes."));
    connect(bSeen, &QPushButton::clicked, this, &HealthTab::markSeen);
    top->addWidget(bSeen);
    auto *bNotify = new QPushButton(tr("Notifications…"));
    bNotify->setToolTip(tr("Which alerts reach the desktop, and whether they wait for the end of a game."));
    connect(bNotify, &QPushButton::clicked, this, [this] { NotifyDialog(this).exec(); });
    top->addWidget(bNotify);
    root->addLayout(top);

    scanWarning_ = new QLabel;
    scanWarning_->setWordWrap(true);
    scanWarning_->setVisible(false);
    scanWarning_->setProperty("tone", "danger");
    root->addWidget(scanWarning_);
    source_ = new QLabel;
    source_->setWordWrap(true);
    source_->setProperty("tone", "muted");
    root->addWidget(source_);
    source_->setVisible(false);

    days_ = new DayBars;
    connect(days_, &DayBars::dayClicked, this, &HealthTab::filterDay);
    root->addWidget(days_);
    dayInfo_ = new QLabel;
    dayInfo_->setWordWrap(true);
    dayInfo_->setProperty("tone", "muted");
    root->addWidget(dayInfo_);

    auto *filterRow = new QHBoxLayout;
    eventSearch_ = new QLineEdit;
    eventSearch_->setClearButtonEnabled(true);
    eventSearch_->setPlaceholderText(tr("Search events, codes or devices…"));
    filterRow->addWidget(eventSearch_, 1);
    severityFilter_ = new QComboBox;
    severityFilter_->addItem(tr("All levels"), QStringLiteral("all"));
    severityFilter_->addItem(tr("Critical"), QStringLiteral("critical"));
    severityFilter_->addItem(tr("Errors"), QStringLiteral("error"));
    severityFilter_->addItem(tr("Warnings / other"), QStringLiteral("other"));
    filterRow->addWidget(severityFilter_);
    technicalEvents_ = new QCheckBox(tr("Technical view"));
    technicalEvents_->setToolTip(tr("Show raw event titles, kernel read mode, PCIe counters and saved-log controls."));
    filterRow->addWidget(technicalEvents_);
    root->addLayout(filterRow);

    list_ = new QTreeWidget;
    list_->setColumnCount(4);
    list_->setHeaderLabels({tr("When"), tr("Level"), tr("Component"), tr("What happened")});
    list_->setRootIsDecorated(false);
    list_->setAlternatingRowColors(true);
    list_->setUniformRowHeights(true);
    list_->setSortingEnabled(false);
    list_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    list_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    list_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    list_->header()->setStretchLastSection(true);
    connect(eventSearch_, &QLineEdit::textChanged, this, &HealthTab::applyDayFilter);
    connect(severityFilter_, &QComboBox::currentIndexChanged, this, [this](int) { applyDayFilter(); });
    connect(technicalEvents_, &QCheckBox::toggled, this, [this](bool technical) {
        source_->setVisible(technical);
        aer_->setVisible(technical);
        for (int i = 0; i < list_->topLevelItemCount(); ++i) {
            auto *item = list_->topLevelItem(i);
            if (item->data(0, Qt::UserRole + 3).toBool()) continue;
            const QJsonObject e = QJsonDocument::fromJson(item->data(0, Qt::UserRole).toByteArray()).object();
            if (!e.isEmpty()) {
                const QString title = e.value(QStringLiteral("title")).toString();
                const QString key = item->data(0, Qt::UserRole + 1).toString();
                const int repeats = groups_.value(key).n;
                item->setText(3, (technical ? title : friendlyEvent(e)) +
                                 (repeats > 1 ? tr("  ×%1").arg(repeats) : QString()));
            }
        }
        applyDayFilter();
    });
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list_, &QTreeWidget::customContextMenuRequested, this, &HealthTab::eventMenu);
    connect(list_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it, int) {
        const QJsonObject e = QJsonDocument::fromJson(it->data(0, Qt::UserRole).toByteArray()).object();
        if (!e.isEmpty()) explainEvent(e);
        else QMessageBox::information(this, tr("Saved event"), it->toolTip(3));
    });
    root->addWidget(list_, 1);

    aer_ = new QLabel;
    aer_->setWordWrap(true);
    aer_->setTextFormat(Qt::RichText);
    root->addWidget(aer_);
    aer_->setVisible(false);

    // Persistent log: every event, across boots, until cleared.
    auto *technicalPanel = new QWidget;
    auto *technicalLayout = new QVBoxLayout(technicalPanel);
    technicalLayout->setContentsMargins(0, 0, 0, 0);
    auto *logRow = new QHBoxLayout;
    logInfo_ = new QLabel;
    logInfo_->setProperty("tone", "muted");
    logRow->addWidget(logInfo_, 1);
    auto *bOpen = new QPushButton(tr("Open saved log"));
    connect(bOpen, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl::fromLocalFile(logPath())); });
    logRow->addWidget(bOpen);
    auto *bClear = new QPushButton(tr("Clear saved log"));
    connect(bClear, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Clear saved log"),
                tr("Delete all locally saved kernel errors from previous and current boots?")) != QMessageBox::Yes) return;
        QFile::remove(logPath());
        QFile::remove(logPath() + QStringLiteral(".1"));
        updateLogInfo();
        refreshDays();
        loadHistoryDay();
        applyDayFilter();
    });
    logRow->addWidget(bClear);
    technicalLayout->addLayout(logRow);
    auto *hint = new QLabel(tr("Technical note: Xid 154 reports recovery, not the original cause. "
                              "Earlier Xid codes, machine checks and PCIe events need contextual diagnosis. "
                              "Right-click an event to open the related tool or inspect its raw log."));
    hint->setWordWrap(true);
    hint->setProperty("tone", "muted");
    technicalLayout->addWidget(hint);
    root->addWidget(technicalPanel);
    technicalPanel->setVisible(false);
    connect(technicalEvents_, &QCheckBox::toggled, technicalPanel, &QWidget::setVisible);

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
        if (list_->topLevelItem(i)->data(0, Qt::UserRole + 3).toBool()) continue;
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
        scanWarning_->setText(tr("Kernel event monitoring is unavailable: tune-profile-helper was not found."));
        scanWarning_->setVisible(true);
        return;
    }
    busy_ = true;
    const QJsonObject req{{QStringLiteral("op"), QStringLiteral("health")}, {QStringLiteral("since"), qint64(lastSeq_)}};
    if (needsRoot_) {
        // health-reader: read-only, classified events only, silent polkit action at every
        // security level -- this runs at start-up and on a timer with the window hidden.
        privileged::run(privileged::helperPath(QStringLiteral("health-reader")), req, this, [this](const privileged::Result &r) {
            busy_ = false;
            if (!r.reached) {
                source_->setText(QStringLiteral("Kernel log restricted and pkexec failed: ") + r.message());
                scanWarning_->setText(tr("Kernel monitoring is unavailable: %1").arg(r.message()));
                scanWarning_->setVisible(true);
                return;
            }
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
        scanWarning_->setText(tr("Kernel monitor could not start."));
        scanWarning_->setVisible(true);
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
        scanWarning_->setText(tr("Kernel monitoring failed: %1").arg(r.value(QStringLiteral("error")).toString()));
        scanWarning_->setVisible(true);
        return;
    }
    scanWarning_->setVisible(false);
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
        g->row->setText(0, QDateTime::fromMSecsSinceEpoch(wallMs(g->firstUs)).toString(QStringLiteral("yyyy-MM-dd HH:mm")) +
                           QStringLiteral(" – ") + QDateTime::fromMSecsSinceEpoch(wallMs(tsUs)).toString(QStringLiteral("HH:mm")));
        g->row->setText(3, (technicalEvents_->isChecked() ? g->base : friendlyEvent(e)) + tr("  ×%1").arg(g->n));
        return;
    }

    const QString level = e.value(QStringLiteral("level")).toString();
    auto *it = new QTreeWidgetItem({QDateTime::fromMSecsSinceEpoch(wallMs(tsUs)).toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                                    level == QLatin1String("critical") ? tr("Critical") :
                                    level == QLatin1String("error") ? tr("Error") : tr("Notice"),
                                    kind.toUpper(), technicalEvents_->isChecked() ? title : friendlyEvent(e)});
    it->setToolTip(3, tr("Original: %1\nKernel uptime: %2\n%3")
                          .arg(title, uptime(quint64(tsUs)), e.value(QStringLiteral("text")).toString()));
    it->setData(0, Qt::UserRole + 4, level);
    // The record itself (with its boot and seq) travels with the row: the menu and double-click need them.
    QJsonObject stored = e;
    stored.insert(QStringLiteral("boot_id"), bootId);
    it->setData(0, Qt::UserRole, QJsonDocument(stored).toJson(QJsonDocument::Compact));
    it->setData(0, Qt::UserRole + 1, key);  // its group, so a row dropped below forgets it
    it->setData(0, Qt::UserRole + 2, QDateTime::fromMSecsSinceEpoch(wallMs(tsUs)).date().toString(Qt::ISODate));  // its day, for the day chart
    const QColor c(levelColor(e.value(QStringLiteral("level")).toString()));
    for (int i = 0; i < 4; ++i) it->setForeground(i, c);
    list_->insertTopLevelItem(0, it);  // newest first
    applyDayFilter();
    groups_[key] = Group{it, title, tsUs, tsUs, 1};
    while (list_->topLevelItemCount() - historyRows_.size() > MAX_ROWS) {
        int lastLive = list_->topLevelItemCount() - 1;
        while (lastLive >= 0 && list_->topLevelItem(lastLive)->data(0, Qt::UserRole + 3).toBool()) --lastLive;
        if (lastLive < 0) break;
        QTreeWidgetItem *old = list_->takeTopLevelItem(lastLive);
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
    if (!page || !pages_) return;
    if (toolItems_.contains(page)) {
        auto *toolItem = toolItems_.value(page);
        if (toolItem->isHidden() || toolItem->parent()->isHidden()) {
            if (auto *search = findChild<QLineEdit *>(QStringLiteral("healthToolSearch"))) search->clear();
        }
        toolItem->parent()->setExpanded(true);
        if (toolNavigation_) toolNavigation_->setCurrentItem(toolItem);
        page = toolDirectory_;
    }
    if (!pageItems_.contains(page)) return;
    QTreeWidgetItem *item = pageItems_.value(page);
    if (item->isHidden() || item->parent()->isHidden()) {
        // Navigation initiated by an overview card must never be blocked by a
        // search filter; clear it before selecting the target.
        if (auto *search = findChild<QLineEdit *>(QStringLiteral("healthNavSearch"))) search->clear();
        item->setHidden(false);
        item->parent()->setHidden(false);
    }
    item->parent()->setExpanded(true);
    navigation_->setCurrentItem(item);
    pages_->setCurrentWidget(page);
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
    loadHistoryDay();
    applyDayFilter();
}

void HealthTab::loadHistoryDay() {
    for (QTreeWidgetItem *it : historyRows_) {
        if (list_->indexOfTopLevelItem(it) >= 0) delete list_->takeTopLevelItem(list_->indexOfTopLevelItem(it));
    }
    historyRows_.clear();
    if (dayFilter_.isEmpty()) return;
    // health.log is bounded and rotated; a selected historical day may have
    // fallen out of retention. Never claim that an absent row means no event.
    const QRegularExpression pattern(QStringLiteral(
        R"(^([0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2})\s+boot=([a-fA-F0-9]+)\s+seq=([0-9]+)\s+(\S+)\s+(\S+)\s+(.*?)\s+\|\s+(.*)$)"));
    QSet<QString> seen;
    for (int i = 0; i < list_->topLevelItemCount(); ++i) {
        auto *it = list_->topLevelItem(i);
        if (it->data(0, Qt::UserRole + 2).toString() != dayFilter_) continue;
        const QJsonObject e = QJsonDocument::fromJson(it->data(0, Qt::UserRole).toByteArray()).object();
        if (e.isEmpty()) continue;
        seen.insert(e.value(QStringLiteral("boot_id")).toString().left(8) + QLatin1Char(':') +
                    QString::number(quint64(e.value(QStringLiteral("seq")).toDouble())));
    }
    QList<QByteArray> matches;
    for (const QString &path : {logPath() + QStringLiteral(".1"), logPath()}) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        while (!file.atEnd()) {
            const QByteArray line = file.readLine();
            if (!line.startsWith((dayFilter_ + QLatin1Char(' ')).toLatin1())) continue;
            matches.append(line);
            if (matches.size() > MAX_ROWS) matches.removeFirst();
        }
    }
    // Keep the most recent 500 records, not the oldest entries of a busy day.
    for (auto it = matches.crbegin(); it != matches.crend(); ++it) {
        const auto match = pattern.match(QString::fromUtf8(*it));
        if (!match.hasMatch()) continue;
        const QString key = match.captured(2) + QLatin1Char(':') + match.captured(3);
        if (seen.contains(key)) continue;
        seen.insert(key);
        const QString level = match.captured(4).toLower();
        const QString kind = match.captured(5);
        const QString original = match.captured(6);
        const QString raw = match.captured(7).trimmed();
        const QJsonObject e{{QStringLiteral("kind"), kind.toLower()}, {QStringLiteral("title"), original},
                            {QStringLiteral("level"), level}, {QStringLiteral("text"), raw}};
        auto *item = new QTreeWidgetItem({match.captured(1).left(16),
                                           level == QLatin1String("critical") ? tr("Critical") :
                                           level == QLatin1String("error") ? tr("Error") : tr("Notice"),
                                           kind, technicalEvents_->isChecked() ? original : friendlyEvent(e)});
        item->setToolTip(3, tr("Saved record (may be from a previous boot):\n%1\n%2").arg(original, raw));
        item->setData(0, Qt::UserRole + 2, dayFilter_);
        item->setData(0, Qt::UserRole + 3, true);
        item->setData(0, Qt::UserRole + 4, level);
        for (int c = 0; c < 4; ++c) item->setForeground(c, QColor(levelColor(level)));
        list_->addTopLevelItem(item);
        historyRows_.append(item);
    }
}

void HealthTab::applyDayFilter() {
    int shown = 0;
    const QString needle = eventSearch_ ? eventSearch_->text().trimmed() : QString();
    const QString severity = severityFilter_ ? severityFilter_->currentData().toString() : QStringLiteral("all");
    for (int i = 0; i < list_->topLevelItemCount(); ++i) {
        QTreeWidgetItem *it = list_->topLevelItem(i);
        const QString level = it->data(0, Qt::UserRole + 4).toString();
        const bool dayMatch = dayFilter_.isEmpty() || it->data(0, Qt::UserRole + 2).toString() == dayFilter_;
        const bool levelMatch = severity == QLatin1String("all") || severity == level ||
                                (severity == QLatin1String("other") && level != QLatin1String("critical") && level != QLatin1String("error"));
        const bool textMatch = needle.isEmpty() ||
                               (it->text(2) + QLatin1Char(' ') + it->text(3) + QLatin1Char(' ') + it->toolTip(3))
                                   .contains(needle, Qt::CaseInsensitive);
        const bool show = dayMatch && levelMatch && textMatch;
        it->setHidden(!show);
        if (show) ++shown;
    }
    if (dayFilter_.isEmpty())
        dayInfo_->setText(tr("%1 events shown from this session. Select a day to inspect saved history.").arg(shown));
    else if (shown == 0)
        dayInfo_->setText(tr("%1: No matching saved records are available. Logs may have rotated or been cleared.").arg(dayFilter_));
    else
        dayInfo_->setText(tr("%1: %2 matching records (saved history and current session). Click the day again to clear the date filter.")
                             .arg(dayFilter_).arg(shown));
}

QString HealthTab::friendlyEvent(const QJsonObject &e) {
    const QString kind = e.value(QStringLiteral("kind")).toString().toLower();
    const int code = e.value(QStringLiteral("code")).toInt();
    if (kind == QLatin1String("xid")) {
        if (code == 79 || e.value(QStringLiteral("title")).toString().contains(QStringLiteral("Xid 79")))
            return tr("NVIDIA GPU communication was lost");
        if (code == 154) return tr("NVIDIA GPU entered recovery (see previous error)");
        return tr("NVIDIA graphics driver reported a GPU error");
    }
    if (kind == QLatin1String("gsp")) return tr("NVIDIA GPU firmware stopped responding");
    if (kind == QLatin1String("mce")) return tr("Processor reported a hardware error");
    if (kind == QLatin1String("aer")) return tr("A PCIe device reported a connection error");
    if (kind == QLatin1String("lockup")) return tr("A CPU core or kernel task stopped responding");
    if (kind == QLatin1String("nvme")) return tr("Storage device reported an error");
    if (kind == QLatin1String("fs")) return tr("File system reported a data or metadata error");
    if (kind == QLatin1String("igpu")) return tr("Integrated graphics reported a problem");
    if (kind == QLatin1String("wifi")) return tr("Wireless device or firmware reported a problem");
    if (kind == QLatin1String("thermal")) return tr("A temperature-related warning was recorded");
    if (kind == QLatin1String("oom")) return tr("System ran out of available memory");
    if (kind == QLatin1String("oops")) return tr("A kernel error or warning was recorded");
    if (kind == QLatin1String("d3cold")) return tr("GPU power-state transition reported a problem");
    return e.value(QStringLiteral("title")).toString();
}

void HealthTab::explainEvent(const QJsonObject &e) {
    const QString kind = e.value(QStringLiteral("kind")).toString();
    QString guidance = tr("Review the raw log and check whether the event repeats before changing system settings.");
    if (kind == QLatin1String("xid") || kind == QLatin1String("gsp"))
        guidance = tr("Check NVIDIA driver and GPU power status. If a custom GPU curve is active, review its stability. The code alone does not prove a hardware fault.");
    else if (kind == QLatin1String("mce") || kind == QLatin1String("lockup"))
        guidance = tr("Inspect CPU curve and RAM stability if they are tuned. Also check temperatures and associated kernel events.");
    else if (kind == QLatin1String("nvme") || kind == QLatin1String("fs"))
        guidance = tr("Back up important files, then check drive SMART information and the full kernel log.");
    else if (kind == QLatin1String("thermal"))
        guidance = tr("Inspect cooling, temperatures and the workload running when the event occurred.");
    else if (kind == QLatin1String("wifi"))
        guidance = tr("Check wireless firmware, driver version and whether the device reconnects normally.");
    QMessageBox dialog(this);
    dialog.setWindowTitle(tr("Event explanation"));
    dialog.setIcon(QMessageBox::Information);
    dialog.setTextFormat(Qt::PlainText);
    dialog.setText(friendlyEvent(e));
    dialog.setInformativeText(guidance);
    dialog.setDetailedText(tr("Reported: %1\n\nRaw kernel record:\n%2")
                           .arg(e.value(QStringLiteral("title")).toString(), e.value(QStringLiteral("text")).toString()));
    dialog.exec();
}

void HealthTab::showInKlog(const QJsonObject &e) {
    klog_->showAt(e.value(QStringLiteral("boot_id")).toString(), quint64(e.value(QStringLiteral("seq")).toDouble()));
    showPage(klog_);
}

void HealthTab::eventMenu(const QPoint &pos) {
    QTreeWidgetItem *it = list_->itemAt(pos);
    if (!it) return;
    if (it->data(0, Qt::UserRole + 3).toBool()) {
        QMenu menu(this);
        menu.addAction(tr("Copy saved record"), this, [it] {
            QApplication::clipboard()->setText(it->toolTip(3));
        });
        menu.exec(list_->viewport()->mapToGlobal(pos));
        return;
    }
    const QJsonObject e = QJsonDocument::fromJson(it->data(0, Qt::UserRole).toByteArray()).object();
    const QString kind = e.value(QStringLiteral("kind")).toString();
    const int code = e.value(QStringLiteral("code")).toInt();
    const QString title = e.value(QStringLiteral("title")).toString();
    QMenu menu(this);
    menu.addAction(tr("Explain this event"), this, [this, e] { explainEvent(e); });
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
    const int errors = xid_ + gsp_ + mce_ + aerN_ + lockup_ + other_ +
                       nvme_ + fs_ + igpu_ + wifi_ + thermal_ + oom_ + oops_;
    const QString label = errors ? tr("%1 reported event(s) to review").arg(errors) :
                                   tr("No new events to review");
    summary_->setText(label);
    summary_->setStyleSheet(QStringLiteral("color:%1").arg(errors ? theme::WARN : theme::OK));
    summary_->setToolTip(tr("Counts events since startup, or since they were last marked reviewed. "
                            "Reviewing does not resolve underlying issues or erase saved history."));
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
