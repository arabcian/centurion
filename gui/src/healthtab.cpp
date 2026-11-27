#include "insights.h"
#include "healthtab.h"
#include "kernelwalltime.h"
#include "backuppage.h"
#include "batterypage.h"
#include "notifydialog.h"
#include "notifyprefs.h"
#include "bootpage.h"
#include "coolingpage.h"
#include "curvepage.h"
#include "eventinfo.h"
#include "diagsources.h"
#include "diagnostichistory.h"
#include "sleeppage.h"
#include "klogpage.h"
#include "kmsgwake.h"
#include "kernelparamspage.h"
#include "lazywidget.h"
#include "dgpupage.h"
#include "netpage.h"
#include "overviewpage.h"
#include "smartpage.h"
#include "charts.h"
#include "userhelper.h"

#include "privileged.h"
#include "sysinfo.h"
#include "systools.h"
#include "diagnosticspage.h"
#include "advancedhealth.h"
#include "advanceddiagnostics.h"
#include "theme.h"
#include "workspacestate.h"
#include "toolcatalog.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QTextBrowser>
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
#include <QUrl>
#include <ctime>
#include <QSocketNotifier>
#include <QTabWidget>
#include <QStackedWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QSplitter>
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
#include <QRegularExpression>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QSignalBlocker>
#include <QGridLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QKeySequence>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QToolButton>
#include <algorithm>

#include <cerrno>
#include <cstring>
#include <memory>
#include <functional>
#include <utility>
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

// A System tool's tooltip: what it shows, then its badges.
static QString toolDescription(QWidget *page, const QString &key) {
    QString tip = page->property("toolAbout").toString();
    const QStringList badges = toolcatalog::badges(toolcatalog::tool(key).flags);
    if (!badges.isEmpty()) tip += (tip.isEmpty() ? QString() : QStringLiteral("\n\n")) + badges.join(QStringLiteral(" · "));
    return tip;
}

// A diagnostic entry is a real, content-sized card.  Using a QPushButton with
// child QLabel layouts caused descriptions to be cropped in compact grid cells:
// QPushButton's own sizeHint does not reliably account for those child layouts.
// This focusable QFrame calculates its height from its label layout and supports
// mouse and keyboard activation without imposing a fixed or maximum height.
class DiagnosticEntryCard final : public QFrame {
public:
    void setActivation(std::function<void()> activation) { activation_ = std::move(activation); }

protected:
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) event->accept();
        else QFrame::mousePressEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
            event->accept();
            const auto action = activation_;  // the action may navigate away from this card
            if (action) action();
            return;
        }
        QFrame::mouseReleaseEvent(event);
    }
    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Space) {
            event->accept();
            const auto action = activation_;
            if (action) action();
            return;
        }
        QFrame::keyPressEvent(event);
    }

private:
    std::function<void()> activation_;
};

// One line per page for the navigation's tooltips: what the page answers.
static QString pageAbout(const QString &key) {
    static const QHash<QString, const char *> ABOUT = {
        {QStringLiteral("overview"), QT_TRANSLATE_NOOP("HealthTab", "One card per area with its level, and suggested next steps.")},
        {QStringLiteral("diagnostics"), QT_TRANSLATE_NOOP("HealthTab", "What went wrong: every boot and its faults, this boot's evidence, and checks that compare boots and versions.")},
        {QStringLiteral("evidence"), QT_TRANSLATE_NOOP("HealthTab", "MCE topology, AER changes, NVIDIA Xid cases, F2FS failures and radio firmware faults.")},
        {QStringLiteral("investigations"), QT_TRANSLATE_NOOP("HealthTab", "Read-only boot regression, NVMe, USB, sleep, CO, GPU and incident investigations.")},
        {QStringLiteral("monitor"), QT_TRANSLATE_NOOP("HealthTab", "Hardware and driver errors from the kernel, explained, with saved history per day.")},
        {QStringLiteral("boot"), QT_TRANSLATE_NOOP("HealthTab", "Every boot: how it ended, its kernel events, the curves in effect, crash records and patterns.")},
        {QStringLiteral("capabilities"), QT_TRANSLATE_NOOP("HealthTab", "Why a feature is missing: read-only kernel, helper and firmware capability checks.")},
        {QStringLiteral("sleep"), QT_TRANSLATE_NOOP("HealthTab", "Suspend statistics, battery used while asleep, wake sources and why the hardware does not sleep.")},
        {QStringLiteral("battery"), QT_TRANSLATE_NOOP("HealthTab", "Time left, drain per scene, capacity (wear) history and forecast.")},
        {QStringLiteral("dgpu"), QT_TRANSLATE_NOOP("HealthTab", "Whether the discrete GPU sleeps, what keeps it awake and what it costs.")},
        {QStringLiteral("cooling"), QT_TRANSLATE_NOOP("HealthTab", "Does the laptop still cool as well as it did: a repeatable load test against a baseline.")},
        {QStringLiteral("smart"), QT_TRANSLATE_NOOP("HealthTab", "Drive status, temperature, wear, media errors and a wear projection.")},
        {QStringLiteral("network"), QT_TRANSLATE_NOOP("HealthTab", "Connections per program, open ports, blocked Windows programs, traffic per day and rules.")},
        {QStringLiteral("backup"), QT_TRANSLATE_NOOP("HealthTab", "Configuration backups and system images, with defaults for this distribution.")},
        {QStringLiteral("curves"), QT_TRANSLATE_NOOP("HealthTab", "Stability of the CPU/GPU curves: faults seen while each was in effect.")},
        {QStringLiteral("klog"), QT_TRANSLATE_NOOP("HealthTab", "Kernel warnings and errors saved across reboots, with filters and explanations.")},
        {QStringLiteral("tools"), QT_TRANSLATE_NOOP("HealthTab", "Detailed device reports: sensors, CPU, memory, PCI, USB, firmware.")},
        {QStringLiteral("kernelparams"), QT_TRANSLATE_NOOP("HealthTab", "Live values of kernel settings, scheduler debugfs included (read-only).")},
    };
    const auto it = ABOUT.constFind(key);
    return it == ABOUT.constEnd() ? QString() : HealthTab::tr(it.value());
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
    theme::setLiveSheet(railTitle, [] { return QStringLiteral("color:%1;font-weight:700;letter-spacing:1px;padding:5px").arg(theme::FG_DIM); });
    railLayout->addWidget(railTitle);
    railTitle->setVisible(!theme::experimentalEnabled());  // Studio: the top bar already names the page
    // Back / forward through the pages and tools opened in this session (also Alt+Left / Alt+Right).
    auto *searchRow = new QHBoxLayout;
    searchRow->setSpacing(2);
    back_ = new QToolButton;
    back_->setArrowType(Qt::LeftArrow);
    back_->setAutoRaise(true);
    back_->setToolTip(tr("Back (Alt+Left)"));
    forward_ = new QToolButton;
    forward_->setArrowType(Qt::RightArrow);
    forward_->setAutoRaise(true);
    forward_->setToolTip(tr("Forward (Alt+Right)"));
    back_->setEnabled(false);
    forward_->setEnabled(false);
    connect(back_, &QToolButton::clicked, this, [this] { stepHistory(-1); });
    connect(forward_, &QToolButton::clicked, this, [this] { stepHistory(1); });
    for (auto [keys, delta] : {std::pair{QKeySequence(Qt::ALT | Qt::Key_Left), -1}, std::pair{QKeySequence(Qt::ALT | Qt::Key_Right), 1}}) {
        auto *sc = new QShortcut(keys, this);
        sc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(sc, &QShortcut::activated, this, [this, delta] { stepHistory(delta); });
    }
    navSearch_ = new QLineEdit;
    auto *navSearch = navSearch_;
    navSearch->setPlaceholderText(tr("Search…"));
    navSearch->setToolTip(tr("Searches page and tool names and the words around them: error codes (Xid 79), symptoms (flicker, overheating), "
                             "parts (fan, battery). Turkish or English."));
    navSearch->setClearButtonEnabled(true);
    navSearch->setObjectName(QStringLiteral("healthNavSearch"));
    searchRow->addWidget(back_);
    searchRow->addWidget(forward_);
    searchRow->addWidget(navSearch, 1);
    railLayout->addLayout(searchRow);
    navigation_ = new QTreeWidget;
    navigation_->setObjectName(QStringLiteral("healthNav"));  // Studio: same current-page marker as the main rail
    navigation_->setHeaderHidden(true);
    navigation_->setRootIsDecorated(false);
    navigation_->setIndentation(0);  // flat list under small-caps headings: an indent strip was painted as a second selection block
    navigation_->setUniformRowHeights(true);
    navigation_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    navigation_->setFrameShape(QFrame::NoFrame);
    navigation_->setStyleSheet(QStringLiteral("QTreeWidget{background:transparent;border:0}QTreeWidget::item{padding:4px 6px}QTreeWidget::branch{background:transparent}"));
    railLayout->addWidget(navigation_, 1);
    split->addWidget(rail);
    auto *pageArea = new QWidget;
    auto *pageLayout = new QVBoxLayout(pageArea);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);
    relatedBar_ = new QWidget;
    relatedBar_->setLayout(new QHBoxLayout);
    relatedBar_->layout()->setContentsMargins(8, 4, 8, 0);
    relatedBar_->setVisible(false);
    pageLayout->addWidget(relatedBar_);
    pages_ = new QStackedWidget;
    pageLayout->addWidget(pages_, 1);
    split->addWidget(pageArea);
    split->setStretchFactor(1, 1);
    split->setSizes({178, 880});
    workspace::rememberSplitter(split, workspace::key(theme::experimentalEnabled(), QStringLiteral("health/navigation")));

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
        item->setToolTip(0, pageAbout(key));
        pageItems_.insert(page, item);
    };
    auto *general = addGroup(tr("Start"));
    auto *problems = addGroup(tr("Problems"));
    auto *hardware = addGroup(tr("Hardware"));
    auto *maintenance = addGroup(tr("Maintenance"));
    auto *advanced = addGroup(tr("Advanced"));

    overview_ = new OverviewPage;
    connect(overview_, &OverviewPage::worstChanged, this, &HealthTab::levelChanged);
    connect(overview_, &OverviewPage::worstChanged, this, [this](int) {
        if (pages_ && pages_->currentWidget() == toolDirectory_ && toolMode_ && toolMode_->currentIndex() == 0)
            rebuildToolCatalog();
    });
    addPage(general, overview_, tr("System Overview"), QStringLiteral("overview"));
    auto *monitor = new QWidget;
    addPage(problems, monitor, tr("Events & Alerts"), QStringLiteral("monitor"));
    // Boot History, this boot's evidence and the checks that compare boots: one page, three tabs.
    diagnostics_ = new DiagnosticsPage;
    investigations_ = diagnostics_->findings();
    boot_ = diagnostics_->boots();
    addPage(problems, diagnostics_, tr("Diagnostics"), QStringLiteral("diagnostics"));
    investigations_->setTargetLabel([this](const QString &target) { return targetLabel(target); });
    connect(investigations_, &AdvancedHealthPage::openTarget, this, [this](const QString &target) { navigateTo(target); });
    connect(overview_, &OverviewPage::triage, this, [this](const QString &category) {
        showDiagnostics(DiagnosticsPage::Findings);
        investigations_->startTriage(category);
    });
    addPage(problems, insights::capabilityPage(), tr("Capability Doctor"), QStringLiteral("capabilities"));

    auto *curves = new CurvePage;
    curves_ = curves;
    connect(curves, &CurvePage::alert, this, [this](const QString &t, const QString &m) { Q_EMIT alert(notifyprefs::CURVE, QString(), t, m); });
    connect(curves, &CurvePage::curveReset, this, &HealthTab::curveReset);
    connect(this, &HealthTab::fault, curves, &CurvePage::onFault);
    addPage(advanced, curves, tr("Curve Stability"), QStringLiteral("curves"));
    klog_ = new KlogPage;
    addPage(advanced, klog_, tr("Kernel Log"), QStringLiteral("klog"));
    addPage(advanced, new LazyWidget([] { return new KernelParamsPage; }), tr("Kernel Parameters"), QStringLiteral("kernelparams"));
    addPage(hardware, new LazyWidget([] { return new SleepPage; }), tr("Sleep & Wake"), QStringLiteral("sleep"));
    if (BatteryPage::present()) {
        auto *battery = new BatteryPage;
        connect(battery, &BatteryPage::dgpuAwake, this, &HealthTab::dgpuAwake);
        addPage(hardware, battery, tr("Battery Use"), QStringLiteral("battery"));
    }
    if (DgpuPage::present()) addPage(hardware, new DgpuPage, tr("dGPU Power"), QStringLiteral("dgpu"));
    cooling_ = new CoolingPage;
    addPage(hardware, cooling_, tr("Cooling"), QStringLiteral("cooling"));

    // System tools: a catalog (search, pinned, recent, tools by area) and a tool view with a path back.
    // No second navigation tree: the rail stays the only list, and tools keep their LazyWidget (nothing runs
    // until a tool is opened by the user).
    toolDirectory_ = new QWidget;
    auto *directoryLayout = new QVBoxLayout(toolDirectory_);
    directoryLayout->setContentsMargins(10, 9, 10, 9);
    directoryLayout->setSpacing(7);
    toolMode_ = new QStackedWidget;
    directoryLayout->addWidget(toolMode_, 1);

    auto *catalogView = new QWidget;
    auto *catalogLayout = new QVBoxLayout(catalogView);
    catalogLayout->setContentsMargins(0, 0, 0, 0);
    catalogLayout->setSpacing(7);
    // Keep search in a stable position while the directory itself scrolls. A complete tool can
    // still be opened in one click; browsing never launches commands or wakes the dGPU.
    auto *directoryEyebrow = new QLabel(tr("HEALTH CENTER  /  SYSTEM TOOLS"));
    theme::setLiveSheet(directoryEyebrow, [] { return QStringLiteral("color:%1;font-size:8pt;font-weight:700;letter-spacing:1px")
                                   .arg(theme::MUTED); });
    catalogLayout->addWidget(directoryEyebrow);
    auto *directoryHeader = new QLabel(tr("System Tools"));
    directoryHeader->setObjectName(QStringLiteral("healthDirectoryTitle"));
    QFont directoryFont = directoryHeader->font();
    directoryFont.setBold(true);
    directoryFont.setPointSize(directoryFont.pointSize() + 8);
    directoryHeader->setFont(directoryFont);
    catalogLayout->addWidget(directoryHeader);
    auto *directoryHint = new QLabel(tr("Find the right diagnostic by symptom, or browse tools by hardware area. "
                                        "Nothing runs until you open a tool."));
    directoryHint->setWordWrap(true);
    directoryHint->setProperty("role", "muted");
    catalogLayout->addWidget(directoryHint);
    toolSearch_ = new QLineEdit;
    toolSearch_->setPlaceholderText(tr("Search tools, error codes or symptoms — e.g. Xid 154, flicker, NVMe, firmware…"));
    toolSearch_->setClearButtonEnabled(true);
    toolSearch_->setObjectName(QStringLiteral("healthToolSearch"));
    toolSearch_->setMinimumHeight(35);
    theme::setLiveSheet(toolSearch_, [] { return QStringLiteral("QLineEdit#healthToolSearch{border:1px solid %1;border-radius:9px;"
                                               "padding:7px 12px;background:%2;}"
                                               "QLineEdit#healthToolSearch:focus{border:1px solid %3;}")
                               .arg(theme::BORDER, theme::WELL, theme::ACCENT); });
    catalogLayout->addWidget(toolSearch_);
    auto *catalogFocus = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), this);
    catalogFocus->setContext(Qt::WidgetWithChildrenShortcut);
    connect(catalogFocus, &QShortcut::activated, toolSearch_, [this] {
        showPage(toolDirectory_);
        showToolCatalog();
        toolSearch_->setFocus();
        toolSearch_->selectAll();
    });
    toolTerms_ = new QLabel;
    toolTerms_->setWordWrap(true);
    toolTerms_->setProperty("role", "muted");
    toolTerms_->setVisible(false);
    catalogLayout->addWidget(toolTerms_);
    toolScroll_ = new QScrollArea;
    toolScroll_->setWidgetResizable(true);
    toolScroll_->setFrameShape(QFrame::NoFrame);
    toolScroll_->viewport()->installEventFilter(this);  // the number of columns follows the width
    catalogLayout->addWidget(toolScroll_, 1);
    toolMode_->addWidget(catalogView);

    auto *toolView = new QWidget;
    auto *toolLayout = new QVBoxLayout(toolView);
    toolLayout->setContentsMargins(0, 0, 0, 0);
    toolLayout->setSpacing(4);
    auto *toolBar = new QHBoxLayout;
    // Same arrow as the rail's Back button, so it reads as "back" without depending on the font.
    auto *allTools = new QToolButton;
    allTools->setArrowType(Qt::LeftArrow);
    allTools->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    allTools->setText(tr("All Tools"));
    allTools->setObjectName(QStringLiteral("allTools"));
    theme::setLiveSheet(allTools, [] { return QStringLiteral("QToolButton#allTools{background:%1;border:1px solid %2;border-radius:7px;padding:3px 9px 3px 5px}"
                                           "QToolButton#allTools:hover{border-color:%3}").arg(theme::BG3, theme::BORDER, theme::ACCENT); });
    allTools->setToolTip(tr("Back to the tool catalog. The open tool keeps its results."));
    connect(allTools, &QToolButton::clicked, this, [this] { showToolCatalog(); });
    toolBar->addWidget(allTools);
    toolPath_ = new QLabel;
    QFont pathFont = toolPath_->font();
    pathFont.setBold(true);
    toolPath_->setFont(pathFont);
    toolBar->addWidget(toolPath_, 1);
    toolBadges_ = new QLabel;
    theme::setLiveSheet(toolBadges_, [] { return QStringLiteral("color:%1").arg(theme::WARN); });
    toolBar->addWidget(toolBadges_);
    toolPin_ = new QPushButton;
    toolPin_->setCheckable(true);
    toolPin_->setToolTip(tr("Favorites are listed at the top of the catalog."));
    connect(toolPin_, &QPushButton::clicked, this, [this] { togglePin(); });
    toolBar->addWidget(toolPin_);
    toolLayout->addLayout(toolBar);
    toolPages_ = new QStackedWidget;
    toolLayout->addWidget(toolPages_, 1);
    toolMode_->addWidget(toolView);
    addPage(general, toolDirectory_, tr("System Tools"), QStringLiteral("tools"));

    QTabWidget toolsRegistry;
    systools::addPages(&toolsRegistry);
    QWidget *smartTool = nullptr;
    while (toolsRegistry.count()) {
        QWidget *page = toolsRegistry.widget(0);
        const QString label = toolsRegistry.tabText(0);
        // Keyed by the page's untranslated name: the tab text is translated.
        const QString key = page->property("toolKey").toString();
        toolsRegistry.removeTab(0);
        // The drive summary is a frequent action: a page of its own in the rail (Drive health).
        if (key == QLatin1String("SMART")) { smartTool = page; continue; }
        addTool(page, label, key);
    }
    if (smartTool) addPage(hardware, smartTool, tr("Drive Health"), QStringLiteral("smart"));
    connect(toolSearch_, &QLineEdit::textChanged, this, [this] { rebuildToolCatalog(); });
    auto *net = new NetPage;
    connect(net, &NetPage::alert, this, [this](const QString &t, const QString &m) { Q_EMIT alert(notifyprefs::NETWORK, QString(), t, m); });
    addPage(maintenance, net, tr("Network Protection"), QStringLiteral("network"));
    backup_ = new BackupPage;
    connect(backup_, &BackupPage::alert, this, [this](const QString &t, const QString &m) { Q_EMIT alert(notifyprefs::BACKUP, QString(), t, m); });
    addPage(maintenance, backup_, tr("Backups"), QStringLiteral("backup"));
    // Search results only: matching System tools, shown in the rail while a search is typed.
    toolResults_ = addGroup(tr("Tool Results"));
    toolResults_->setHidden(true);
    for (const Tool &t : std::as_const(tools_)) {
        auto *item = new QTreeWidgetItem(toolResults_, {t.label});
        item->setData(0, Qt::UserRole, QStringLiteral("tool:") + t.key);
        item->setToolTip(0, toolDescription(t.page, t.key));
        item->setHidden(true);
    }
    connect(navigation_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *item, QTreeWidgetItem *) {
        if (!item || !item->parent()) return;
        if (item->parent() == toolResults_) {
            // A tool found by the rail's search: open it, then leave the search so the rail shows where it is.
            const QString target = item->data(0, Qt::UserRole).toString();
            QTimer::singleShot(0, this, [this, target] { navigateTo(target); });
            return;
        }
        for (auto it = pageItems_.cbegin(); it != pageItems_.cend(); ++it) {
            if (it.value() == item) {
                // Explicitly selecting System tools goes to the directory, not a stale tool.
                // openTool() sets replaying_ while selecting this rail item: keep that tool intact.
                if (it.key() == toolDirectory_ && !replaying_ && toolMode_->currentIndex() != 0)
                    toolMode_->setCurrentIndex(0);
                pages_->setCurrentWidget(it.key());
                if (it.key() == toolDirectory_ && toolMode_->currentIndex() == 0)
                    rebuildToolCatalog();  // refresh recorded indicators after returning from Health
                const QString key = item->data(0, Qt::UserRole).toString();
                updateRelated(key);
                guiSettings().setValue(QStringLiteral("health/page"), key);
                const Tool *open = it.key() == toolDirectory_ && toolMode_->currentIndex() == 1 ? toolOf(toolPages_->currentWidget()) : nullptr;
                record(open ? QStringLiteral("tool:") + open->key : QStringLiteral("page:") + key);
                break;
            }
        }
    });
    connect(navigation_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item, int) {
        if (item == pageItems_.value(toolDirectory_) && toolMode_->currentIndex() != 0)
            showToolCatalog();  // clicking the currently selected rail item is also a navigation action
    });
    connect(navSearch, &QLineEdit::textChanged, this, &HealthTab::filterNavigation);
    QString lastPage = guiSettings().value(QStringLiteral("health/page"), QStringLiteral("overview")).toString();
    if (const int tab = DiagnosticsPage::tabForKey(lastPage); tab >= 0) {   // a page of an earlier version
        diagnostics_->showTab(static_cast<DiagnosticsPage::Tab>(tab));
        lastPage = QStringLiteral("diagnostics");
    }
    QTreeWidgetItem *initial = pageItems_.value(overview_);
    for (auto it = pageItems_.cbegin(); it != pageItems_.cend(); ++it) {
        if (it.value()->data(0, Qt::UserRole).toString() == lastPage) { initial = it.value(); break; }
    }
    navigation_->setCurrentItem(initial);
    // currentItemChanged selects the page synchronously; no tool is run in the background. System tools
    // opens on its catalog: the last tool is not reopened (it could run a command or wake the dGPU).
    rebuildToolCatalog();

    connect(overview_, &OverviewPage::openPage, this, [monitor, this](const QString &target) {
        QWidget *page = nullptr;
        if (target == QLatin1String("monitor")) page = monitor;
        else if (target == QLatin1String("boot")) { diagnostics_->showTab(DiagnosticsPage::Boots); page = diagnostics_; }
        else if (target == QLatin1String("backup")) page = backup_;
        else if (target == QLatin1String("battery")) page = findChild<BatteryPage *>();
        else if (target == QLatin1String("cooling")) page = cooling_;
        else if (target == QLatin1String("dgpu")) page = findChild<DgpuPage *>();
        else if (target == QLatin1String("sleep")) page = pageByKey(target);
        else if (target == QLatin1String("klog")) page = klog_;
        else if (target == QLatin1String("tools")) {
            showToolCatalog();
            page = toolDirectory_;
        }
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
    theme::setLiveSheet(band_, [] { return QStringLiteral("QFrame{background:%1;border:1px solid %2;border-radius:6px}QLabel{border:none}").arg(theme::BG2, theme::DANGER_SOFT); });
    auto *bandRow = new QHBoxLayout(band_);
    bandRow->setContentsMargins(8, 4, 8, 4);
    bandText_ = new QLabel;
    bandText_->setWordWrap(true);
    bandRow->addWidget(bandText_, 1);
    auto *bBandShow = new QPushButton(tr("Show"));
    connect(bBandShow, &QPushButton::clicked, this, [this] { showDiagnostics(DiagnosticsPage::Boots); });
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
    connect(boot_, &BootPage::openKernelLog, this, [this](const QString &bootId) { klog_->showBoot(bootId); showPage(klog_); });

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
    auto *timelineButton = new QPushButton(tr("Incident Timeline…"));
    timelineButton->setToolTip(tr("Correlate the selected incident with boot-matched sensor and scene history."));
    timelineButton->setEnabled(false);  // until an event is selected
    filterRow->addWidget(timelineButton);
    connect(timelineButton, &QPushButton::clicked, this, [this] {
        QTreeWidgetItem *selected = list_ ? list_->currentItem() : nullptr;
        const QJsonObject event = selected
            ? QJsonDocument::fromJson(selected->data(0, Qt::UserRole).toByteArray()).object()
            : QJsonObject{};
        if (event.isEmpty()) return;
        insights::show(this, tr("Hardware Incident Timeline"),
                       insights::incident(event, event.value("boot_id").toString()));
    });
    root->addLayout(filterRow);

    list_ = new QTreeWidget;
    list_->setColumnCount(4);
    list_->setHeaderLabels({tr("When"), tr("Level"), tr("Component"), tr("What Happened")});
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
        const QJsonObject e = eventOf(it);
        if (!e.isEmpty()) explainEvent(e);
        else QMessageBox::information(this, tr("Saved event"), it->toolTip(3));
    });
    // The selected event explained right under the list: what it means, its usual causes, what to do.
    auto *eventSplit = new QSplitter(Qt::Vertical);
    eventSplit->setChildrenCollapsible(true);
    eventSplit->addWidget(list_);
    eventDetail_ = new QTextBrowser;
    eventDetail_->setOpenLinks(false);
    eventDetail_->setPlaceholderText(tr("Select an event to see what it means, its usual causes and what to do."));
    auto *detailPane = new QWidget;
    auto *detailLayout = new QVBoxLayout(detailPane);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->addWidget(eventDetail_, 1);
    eventActions_ = new QWidget;
    eventActions_->setLayout(new QHBoxLayout);
    eventActions_->layout()->setContentsMargins(0, 0, 0, 0);
    eventActions_->setVisible(false);
    detailLayout->addWidget(eventActions_);
    eventSplit->addWidget(detailPane);
    eventSplit->setStretchFactor(0, 3);
    eventSplit->setStretchFactor(1, 2);
    connect(list_, &QTreeWidget::currentItemChanged, this, &HealthTab::showEventDetail);
    connect(list_, &QTreeWidget::currentItemChanged, timelineButton, [timelineButton](QTreeWidgetItem *item) {
        timelineButton->setEnabled(item && !QJsonDocument::fromJson(item->data(0, Qt::UserRole).toByteArray()).object().isEmpty());
    });
    root->addWidget(eventSplit, 1);

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
        const bool cleared = diagsrc::clearSavedLog(diagsrc::SavedLog::Health);
        updateLogInfo();
        refreshDays();
        loadHistoryDay();
        applyDayFilter();
        if (!cleared) logInfo_->setText(tr("Could not clear all saved log files."));
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
        savedBoot_ = QString::fromStdString(diagnostichistory::fullBoot(diagsrc::bootId().toStdString()));
        if (!savedBoot_.isEmpty()) {
            const auto tail = diagsrc::savedLog(diagsrc::SavedLog::Health, 10000, TAIL_BYTES);
            for (const auto &record : tail.lines)
                if (record.boot == savedBoot_) savedSeq_ = std::max(savedSeq_, record.seq);
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
    // The wake list is shared with the Kernel log page: see kmsgwake.h.
    char buf[8192];
    bool relevant = false;
    for (int i = 0; i < 512; ++i) {  // bounded: a flood must not pin the GUI thread
        const ssize_t n = ::read(kmsgFd_, buf, sizeof buf - 1);
        if (n < 0 && errno == EPIPE) continue;  // record overwritten meanwhile
        if (n <= 0) break;                      // EAGAIN: drained
        if (relevant) continue;
        buf[n] = 0;
        if (kmsgwake::wakes(buf)) relevant = true;
    }
    if (relevant) debounce_->start();
}

void HealthTab::scan() {
    if (busy_) {
        if (!needsRoot_) debounce_->start();
        return;
    }
    if (!QFileInfo(helperPath()).isExecutable()) {
        source_->setText(QStringLiteral("tune-helper is not installed."));
        scanWarning_->setText(tr("Kernel event monitoring is unavailable: tune-profile-helper was not found."));
        scanWarning_->setVisible(true);
        return;
    }
    busy_ = true;
    const QJsonObject req{{QStringLiteral("op"), QStringLiteral("health")}, {QStringLiteral("since"), qint64(savedCaptureSeq_)}};
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
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus status) {
        busy_ = false;
        const QByteArray out = p->readAllStandardOutput().trimmed();
        p->deleteLater();
        const auto reply = QJsonDocument::fromJson(out.mid(out.lastIndexOf('\n') + 1)).object();
        if (status != QProcess::NormalExit || reply.isEmpty()
            || (code != 0 && reply.value(QStringLiteral("ok")).toBool())) {
            const auto error = tr("Kernel monitor stopped or returned an invalid reply.");
            onReply(QJsonObject{{QStringLiteral("error"), error}}, false);
            return;
        }
        onReply(reply, false);
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
    const auto reject = [this] {
        const auto error = tr("Kernel monitor returned invalid event data.");
        source_->setText(error);
        scanWarning_->setText(error);
        scanWarning_->setVisible(true);
    };
    const auto bootId = QString::fromStdString(diagnostichistory::fullBoot(
        r.value(QStringLiteral("boot_id")).toString().toStdString()));
    const auto cursor = r.value(QStringLiteral("last_seq")).toInteger(-1);
    const auto events = r.value(QStringLiteral("events"));
    if (bootId.isEmpty() || (!savedBoot_.isEmpty() && bootId != savedBoot_)
        || cursor < qint64(lastSeq_) || !events.isArray()) {
        reject();
        return;
    }
    auto ev = events.toArray();
    const auto clock = kernelwalltime::Anchor::capture();
    static const QRegularExpression token(QStringLiteral("\\A[A-Za-z0-9_-]+\\z"));
    qint64 previous = qint64(savedCaptureSeq_);
    for (const auto &v : ev) {
        const auto e = v.toObject();
        const auto sequence = e.value(QStringLiteral("seq")).toInteger(-1);
        if (sequence <= previous || sequence > cursor || e.value(QStringLiteral("ts_us")).toInteger(-1) < 0
            || !token.match(e.value(QStringLiteral("level")).toString()).hasMatch()
            || !token.match(e.value(QStringLiteral("kind")).toString()).hasMatch()
            || !e.value(QStringLiteral("title")).isString() || !e.value(QStringLiteral("text")).isString()) {
            reject();
            return;
        }
        previous = sequence;
    }
    for (qsizetype i = 0; i < ev.size(); ++i) {
        auto e = ev.at(i).toObject();
        e.insert(QStringLiteral("wall_ms"), clock ? clock->toWallMs(e.value("ts_us").toInteger(-1)) : 0);
        ev.replace(i, e);
    }
    const bool saved = persist(ev, bootId);
    if (saved) {
        savedCaptureSeq_ = quint64(cursor);
        if (!needsRoot_) timer_->stop();
    } else if (!timer_->isActive()) {
        // Retry on quiet systems even if the kernel produces no new events.
        timer_->start(POLL_ROOT_MS);
    }

    scanWarning_->setVisible(false);
    source_->setText(viaRoot
        ? QStringLiteral("Kernel log is restricted (kernel.dmesg_restrict=1): read through pkexec every %1 min. "
                         "sysctl kernel.dmesg_restrict=0 allows direct, faster checks.").arg(POLL_ROOT_MS / 60000)
        : QStringLiteral("Watching the kernel log directly (scans only when a relevant line appears)."));

    int newBad = 0;
    bool newCritical = false;
    QString firstBad;
    for (const QJsonValue &v : ev) {
        const QJsonObject e = v.toObject();
        if (e.value(QStringLiteral("seq")).toInteger() <= qint64(lastSeq_)) continue;
        addEvent(e, bootId);
        if (!first_) Q_EMIT fault(e);
        const QString lvl = e.value(QStringLiteral("level")).toString();
        if (lvl == QLatin1String("critical") || lvl == QLatin1String("error")) {
            if (lvl == QLatin1String("critical")) newCritical = true;
            if (!newBad++) firstBad = e.value(QStringLiteral("title")).toString();
        }
    }
    lastSeq_ = quint64(cursor);

    if (newBad) {
        const QString severity = newCritical ? notifyprefs::CRITICAL_LEVEL : notifyprefs::ERROR_LEVEL;
        if (first_) Q_EMIT alert(notifyprefs::KERNEL, severity, QStringLiteral("Health"), QStringLiteral("%1 hardware/driver error(s) since boot — see the Health tab.").arg(newBad));
        else Q_EMIT alert(notifyprefs::KERNEL, severity, QStringLiteral("Health"), newBad == 1 ? firstBad : QStringLiteral("%1 new errors — %2").arg(newBad).arg(firstBad));
    }
    first_ = false;

    QStringList aer;
    for (const QJsonValue &v : r.value(QStringLiteral("aer")).toArray()) {
        const QJsonObject a = v.toObject();
        const QJsonValue corrected = a.value(QStringLiteral("cor"));
        const QJsonValue nf = a.value(QStringLiteral("nonfatal"));
        const QJsonValue fatal = a.value(QStringLiteral("fatal"));
        if (corrected.isDouble() && nf.isDouble() && fatal.isDouble() &&
            corrected.toInteger() == 0 && nf.toInteger() == 0 && fatal.toInteger() == 0) continue;
        const qint64 bad = nf.toInteger() + fatal.toInteger();
        aer << QStringLiteral("<span style='color:%1'>%2 %3: %4 corrected, %5 uncorrectable</span>")
                   .arg(QString::fromLatin1(bad ? theme::DANGER : theme::FG_DIM))
                   .arg(a.value(QStringLiteral("what")).toString())
                   .arg(a.value(QStringLiteral("dev")).toString())
                   .arg(corrected.isDouble() ? QString::number(corrected.toInteger()) : QStringLiteral("Unknown"))
                   .arg(nf.isDouble() && fatal.isDouble() ? QString::number(bad) : QStringLiteral("Unknown"));
    }
    aer_->setText(aer.isEmpty() ? QStringLiteral("<span style='color:%1'>PCIe AER counters: no nonzero totals reported (some devices may be unsupported).</span>").arg(theme::MUTED)
                                : QStringLiteral("PCIe AER counters (since boot):<br>") + aer.join(QStringLiteral("<br>")));
    updateSummary();
    applyDayFilter();
    if (saved && !ev.isEmpty()) refreshDays();
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
        const QString pci = e.value(QStringLiteral("pci")).toString();
        if (code == 154) {
            if (!lastRootXid_.isEmpty() && !pci.isEmpty() && pci == lastRootXidPci_ &&
                bootId == lastRootXidBoot_ && tsUs >= lastRootXidUs_ && tsUs - lastRootXidUs_ <= 300e6)
                title += QStringLiteral("  (preceding same-GPU Xid, NOT proven cause: ") + lastRootXid_ + QLatin1Char(')');
        } else {
            lastRootXid_ = title;
            lastRootXidPci_ = pci;
            lastRootXidBoot_ = bootId;
            lastRootXidUs_ = tsUs;
        }
    } else if (kind == QLatin1String("gsp")) counter = &gsp_;
    else if (kind == QLatin1String("mce")) counter = &mce_;
    else if (kind == QLatin1String("aer")) counter = &aerN_;
    else if (kind == QLatin1String("lockup")) counter = &lockup_;
    else if (kind == QLatin1String("nvme")) counter = &nvme_;
    else if (kind == QLatin1String("fs") || kind == QLatin1String("f2fs")) counter = &fs_;
    else if (kind == QLatin1String("igpu")) counter = &igpu_;
    else if (kind == QLatin1String("wifi") || kind == QLatin1String("bluetooth")) counter = &wifi_;
    else if (kind == QLatin1String("thermal")) counter = &thermal_;
    else if (kind == QLatin1String("oom")) counter = &oom_;
    else if (kind == QLatin1String("oops")) counter = &oops_;
    const auto eventMs = e.value(QStringLiteral("wall_ms")).toInteger();
    const auto eventTime = eventMs ? QDateTime::fromMSecsSinceEpoch(eventMs) : QDateTime{};
    if (eventMs > seenMs_) ++*counter;  // only what is not marked seen counts

    // The same event again (same boot, kind, code and raw title) within GROUP_US: one row, with a count.
    const QString key = bootId + QLatin1Char('|') + kind + QLatin1Char('|') + QString::number(code) + QLatin1Char('|') + rawTitle;
    auto g = groups_.find(key);
    if (g != groups_.end() && tsUs - g->lastUs <= GROUP_US && list_->indexOfTopLevelItem(g->row) >= 0) {
        g->lastUs = tsUs;
        ++g->n;
        list_->insertTopLevelItem(0, list_->takeTopLevelItem(list_->indexOfTopLevelItem(g->row)));  // newest first
        g->row->setText(0, g->firstTime.toString(QStringLiteral("yyyy-MM-dd HH:mm")) +
                           QStringLiteral(" – ") + eventTime.toString(QStringLiteral("HH:mm")));
        g->row->setText(3, (technicalEvents_->isChecked() ? g->base : friendlyEvent(e)) + tr("  ×%1").arg(g->n));
        return;
    }

    const QString level = e.value(QStringLiteral("level")).toString();
    auto *it = new QTreeWidgetItem({eventTime.toString(QStringLiteral("yyyy-MM-dd HH:mm")),
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
    it->setData(0, Qt::UserRole + 2, eventTime.date().toString(Qt::ISODate));  // its day, for the day chart
    const QColor c(levelColor(e.value(QStringLiteral("level")).toString()));
    for (int i = 0; i < 4; ++i) it->setForeground(i, c);
    list_->insertTopLevelItem(0, it);  // newest first
    applyDayFilter();
    groups_[key] = Group{it, title, eventTime, tsUs, 1};
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
    if (toolOf(page)) { openTool(page); return; }
    if (!pageItems_.contains(page)) return;
    QTreeWidgetItem *item = pageItems_.value(page);
    if (item->isHidden() || item->parent()->isHidden()) {
        // Navigation initiated by an overview card must never be blocked by a
        // search filter; clear it before selecting the target.
        if (navSearch_) navSearch_->clear();
        item->setHidden(false);
        item->parent()->setHidden(false);
    }
    item->parent()->setExpanded(true);
    navigation_->setCurrentItem(item);
    pages_->setCurrentWidget(page);
}

void HealthTab::showDiagnostics(int tab) {
    diagnostics_->showTab(static_cast<DiagnosticsPage::Tab>(tab));
    showPage(diagnostics_);
}

void HealthTab::addToolPage(QWidget *page, const QString &label, const QString &key) {
    addTool(page, label, key);
    if (toolResults_) {
        auto *item = new QTreeWidgetItem(toolResults_, {label});
        item->setData(0, Qt::UserRole, QStringLiteral("tool:") + key);
        item->setToolTip(0, toolDescription(page, key));
        item->setHidden(true);
    }
    rebuildToolCatalog();
    if (QTreeWidgetItem *current = navigation_->currentItem())  // the open page may link to this tool
        updateRelated(current->data(0, Qt::UserRole).toString());
}

// ── System tools ───────────────────────────────────────────────────────────────

void HealthTab::addTool(QWidget *page, const QString &label, const QString &key) {
    tools_ << Tool{page, key, label};
    toolPages_->addWidget(page);
}

const HealthTab::Tool *HealthTab::toolOf(QWidget *page) const {
    for (const Tool &t : tools_)
        if (t.page == page) return &t;
    return nullptr;
}

const HealthTab::Tool *HealthTab::toolByKey(const QString &key) const {
    for (const Tool &t : tools_)
        if (t.key == key) return &t;
    return nullptr;
}

QStringList HealthTab::toolList(const QString &setting) const {
    QStringList keys;
    for (const QString &k : guiSettings().value(setting).toStringList())
        if (toolByKey(k)) keys << k;  // a tool this system no longer has is skipped, not shown as a dead button
    return keys;
}

void HealthTab::openTool(QWidget *page) {
    const Tool *t = toolOf(page);
    if (!t) return;
    const toolcatalog::Info info = toolcatalog::tool(t->key);
    toolPath_->setText(tr("System Tools › %1 › %2").arg(toolcatalog::areaName(info.area), t->label));
    toolBadges_->setText(toolcatalog::badges(info.flags).join(QStringLiteral(" · ")));
    const bool pinned = guiSettings().value(QStringLiteral("health/pinned_tools")).toStringList().contains(t->key);
    toolPin_->setChecked(pinned);
    toolPin_->setText(pinned ? tr("★ In Favorites") : tr("☆ Add to Favorites"));
    toolPages_->setCurrentWidget(page);  // a LazyWidget builds (and its tool runs) only now
    toolMode_->setCurrentIndex(1);
    QStringList recent = guiSettings().value(QStringLiteral("health/recent_tools")).toStringList();
    recent.removeAll(t->key);
    recent.prepend(t->key);
    guiSettings().setValue(QStringLiteral("health/recent_tools"), QStringList(recent.mid(0, 6)));
    const QString target = QStringLiteral("tool:") + t->key;
    {
        const bool was = replaying_;
        replaying_ = true;  // the rail's own "page:tools" step is part of this one
        showPage(toolDirectory_);
        replaying_ = was;
    }
    record(target);
    rebuildToolCatalog();
}

void HealthTab::showToolCatalog() {
    toolMode_->setCurrentIndex(0);
    rebuildToolCatalog();
    record(QStringLiteral("page:tools"));
}

void HealthTab::togglePin() {
    const Tool *t = toolOf(toolPages_->currentWidget());
    if (!t) return;
    QStringList pinned = guiSettings().value(QStringLiteral("health/pinned_tools")).toStringList();
    if (pinned.contains(t->key)) pinned.removeAll(t->key);
    else pinned << t->key;
    guiSettings().setValue(QStringLiteral("health/pinned_tools"), pinned);
    toolPin_->setChecked(pinned.contains(t->key));
    toolPin_->setText(pinned.contains(t->key) ? tr("★ In Favorites") : tr("☆ Add to Favorites"));
    rebuildToolCatalog();
}

// The directory deliberately presents information before actions. One whole card opens the
// existing page; its star is a separate, keyboard-focusable action and never opens that tool.
QWidget *HealthTab::toolButton(const Tool &t, bool featured) {
    const toolcatalog::Info info = toolcatalog::tool(t.key);
    // A content-sized clickable frame keeps the entire explanation visible and
    // leaves the separate star control independently focusable/clickable.
    auto *card = new DiagnosticEntryCard;
    card->setObjectName(featured ? QStringLiteral("healthFeaturedTool") : QStringLiteral("healthCatalogTool"));
    card->setAccessibleName(tr("Open %1").arg(t.label));
    card->setToolTip(toolDescription(t.page, t.key));
    card->setCursor(Qt::PointingHandCursor);
    card->setFocusPolicy(Qt::StrongFocus);
    theme::setLiveSheet(card, [card] { return QStringLiteral("QFrame#%1{background:%2;border:1px solid %3;border-radius:10px;}"
                                       "QFrame#%1:hover,QFrame#%1:focus{border-color:%4;}"
                                       "QFrame#%1 QLabel{background:transparent;border:0;}")
                        .arg(card->objectName(), theme::BG2, theme::BORDER, theme::ACCENT_SOFT); });
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    QWidget *page = t.page;
    card->setActivation([this, page] { openTool(page); });

    auto *row = new QHBoxLayout(card);
    row->setContentsMargins(featured ? 12 : 10, featured ? 11 : 9, 9, featured ? 11 : 9);
    row->setSpacing(7);
    auto *textArea = new QWidget;
    textArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    // No inner button: let the frame receive a click anywhere on the card body.
    textArea->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *contents = new QVBoxLayout(textArea);
    contents->setContentsMargins(0, 0, 0, 0);
    contents->setSpacing(5);
    auto *title = new QLabel(t.label);
    title->setTextFormat(Qt::PlainText);
    title->setWordWrap(true);
    theme::setLiveSheet(title, [] { return QStringLiteral("color:%1;font-weight:700;").arg(theme::FG); });
    contents->addWidget(title);
    const QString about = t.page->property("toolAbout").toString().simplified();
    if (!about.isEmpty()) {
        auto *description = new QLabel(about);
        description->setWordWrap(true);
        description->setTextFormat(Qt::PlainText);
        theme::setLiveSheet(description, [] { return QStringLiteral("color:%1;font-size:8pt;").arg(theme::FG_DIM); });
        contents->addWidget(description);  // no line/height cap: never cut off text
    }
    QStringList notes = toolcatalog::badges(info.flags);
    const bool passive = info.flags == 0;
    if (passive) notes << tr("Read-only");
    auto *badge = new QLabel(notes.join(QStringLiteral("  ·  ")));
    badge->setTextFormat(Qt::PlainText);
    badge->setWordWrap(true);
    theme::setLiveSheet(badge, [passive] { return QStringLiteral("color:%1;font-size:8pt;")
                         .arg(passive ? theme::FG_DIM : theme::WARN); });
    contents->addWidget(badge);
    row->addWidget(textArea, 1);

    auto *pin = new QToolButton;
    const bool pinned = toolList(QStringLiteral("health/pinned_tools")).contains(t.key);
    pin->setObjectName(QStringLiteral("healthCatalogPin"));
    pin->setText(pinned ? QStringLiteral("★") : QStringLiteral("☆"));
    pin->setToolTip(pinned ? tr("Remove from favorites") : tr("Add to favorites"));
    pin->setAccessibleName(pin->toolTip() + QStringLiteral(": ") + t.label);
    pin->setCursor(Qt::PointingHandCursor);
    theme::setLiveSheet(pin, [pinned] { return QStringLiteral("QToolButton#healthCatalogPin{background:transparent;border:none;color:%1;"
                                      "font-size:15pt;padding:4px;}"
                                      "QToolButton#healthCatalogPin:hover{color:%2;}")
                       .arg(pinned ? theme::ACCENT : theme::FG_DIM, theme::ACCENT); });
    connect(pin, &QToolButton::clicked, this, [this, key = t.key] {
        QStringList pinned = guiSettings().value(QStringLiteral("health/pinned_tools")).toStringList();
        if (pinned.contains(key)) pinned.removeAll(key);
        else pinned << key;
        guiSettings().setValue(QStringLiteral("health/pinned_tools"), pinned);
        rebuildToolCatalog();
    });
    row->addWidget(pin, 0, Qt::AlignTop);
    return card;
}

void HealthTab::rebuildToolCatalog() {
    if (!toolScroll_ || !toolSearch_) return;
    // Its cards are a few hundred widgets: built only where they can be seen. Hidden (another page, a
    // tool, the window in the tray), the catalog is marked stale and built when it is shown (eventFilter).
    if (!toolScroll_->isVisible()) { toolCatalogStale_ = true; return; }
    toolCatalogStale_ = false;
    const QString query = toolSearch_->text().trimmed();
    const int width = std::max(toolScroll_->viewport()->width(), 300);
    // A compact list has room for real explanations. Four columns of labels were hard to scan.
    toolColumns_ = std::clamp(width / 480, 1, 2);
    toolFeaturedColumns_ = std::clamp(width / 390, 1, 3);
    const QString terms = toolcatalog::explainTerms(query);
    toolTerms_->setText(terms);
    toolTerms_->setVisible(!terms.isEmpty());

    auto *catalog = new QWidget;
    catalog->setObjectName(QStringLiteral("healthToolCatalog"));
    auto *v = new QVBoxLayout(catalog);
    v->setContentsMargins(0, 3, 6, 7);
    v->setSpacing(13);

    auto heading = [this](const QString &text, const QString &hint) -> QWidget * {
        auto *w = new QWidget;
        auto *layout = new QHBoxLayout(w);
        layout->setContentsMargins(0, 5, 0, 1);
        layout->setSpacing(10);
        auto *title = new QLabel(text);
        QFont f = title->font();
        f.setBold(true);
        f.setPointSize(f.pointSize() + 2);
        title->setFont(f);
        layout->addWidget(title);
        auto *note = new QLabel(hint);
        note->setWordWrap(true);
        theme::setLiveSheet(note, [] { return QStringLiteral("color:%1;font-size:8pt;").arg(theme::FG_DIM); });
        layout->addWidget(note, 1);
        return w;
    };
    auto cardsGrid = [this](const QList<const Tool *> &list, int columns, bool featured) -> QWidget * {
        auto *container = new QWidget;
        auto *g = new QGridLayout(container);
        g->setContentsMargins(0, 0, 0, 0);
        g->setSpacing(7);
        for (int c = 0; c < columns; ++c) g->setColumnStretch(c, 1);
        for (int i = 0; i < list.size(); ++i)
            g->addWidget(toolButton(*list[i], featured), i / columns, i % columns);
        return container;
    };
    auto byKeys = [this](const QStringList &keys) {
        QList<const Tool *> result;
        QSet<QString> seen;
        for (const QString &k : keys) {
            if (seen.contains(k)) continue;
            if (const Tool *t = toolByKey(k)) { result << t; seen.insert(k); }
        }
        return result;
    };
    auto matchesFilter = [this, &query](const Tool &t) {
        const toolcatalog::Info info = toolcatalog::tool(t.key);
        if (!toolAreaFilter_.isEmpty() && toolcatalog::areaName(info.area) != toolAreaFilter_) return false;
        if (toolFilter_ == QLatin1String("safe") && info.flags != 0) return false;
        if (toolFilter_ == QLatin1String("changes") && !(info.flags & toolcatalog::ChangesSettings)) return false;
        if (toolFilter_ == QLatin1String("recent") && !toolList(QStringLiteral("health/recent_tools")).contains(t.key)) return false;
        if (toolFilter_ == QLatin1String("pinned") && !toolList(QStringLiteral("health/pinned_tools")).contains(t.key)) return false;
        return toolcatalog::matches(query, t.label + QLatin1Char('|') + toolcatalog::areaName(info.area) +
                                   QLatin1Char('|') + t.key, info.keywords);
    };

    // The catalog is for finding tools, not a second health dashboard.
    // System overview remains available in the Health navigation.
    if (query.isEmpty() && toolAreaFilter_.isEmpty() && toolFilter_ == QLatin1String("all")) {
        const QList<const Tool *> favorites = byKeys(toolList(QStringLiteral("health/pinned_tools")));
        v->addWidget(heading(tr("Favorites"), favorites.isEmpty()
            ? tr("No favorites yet. Star a tool in All tools to add it here.")
            : tr("Only tools you have added to favorites are shown here.")));
        if (!favorites.isEmpty())
            v->addWidget(cardsGrid(favorites, std::min(toolFeaturedColumns_, int(favorites.size())), true));

    }

    // The rail's search finds pages too: show the matching Health pages here as well.
    if (!query.isEmpty()) {
        QList<QTreeWidgetItem *> found;
        for (int g = 0; g < navigation_->topLevelItemCount(); ++g) {
            QTreeWidgetItem *group = navigation_->topLevelItem(g);
            if (group == toolResults_) continue;
            for (int n = 0; n < group->childCount(); ++n) {
                QTreeWidgetItem *item = group->child(n);
                if (pageItems_.key(item) == toolDirectory_) continue;
                if (toolcatalog::matches(query, item->text(0) + QLatin1Char('|') + group->text(0),
                                         toolcatalog::pageKeywords(item->data(0, Qt::UserRole).toString())))
                    found << item;
            }
        }
        if (!found.isEmpty()) {
            v->addWidget(heading(tr("Health Pages"), tr("Pages of the Health Center that match the search.")));
            auto *pagesGrid = new QWidget;
            auto *pg = new QGridLayout(pagesGrid);
            pg->setContentsMargins(0, 0, 0, 0);
            pg->setSpacing(7);
            for (int i = 0; i < found.size(); ++i) {
                auto *b = new QPushButton(found[i]->text(0));
                b->setToolTip(found[i]->toolTip(0));
                b->setCursor(Qt::PointingHandCursor);
                connect(b, &QPushButton::clicked, this, [this, page = pageItems_.key(found[i])] { showPage(page); });
                pg->addWidget(b, i / toolFeaturedColumns_, i % toolFeaturedColumns_);
            }
            v->addWidget(pagesGrid);
        }
    }

    // Stable numeric area IDs: names are translated and must never be used as settings keys. Every group starts expanded.
    const QStringList collapsed = guiSettings().value(QStringLiteral("health/catalog_collapsed_groups")).toStringList();
    auto *filtersTitle = heading(tr("All Tools"), tr("Open a category or filter the directory."));
    v->addWidget(filtersTitle);
    auto *filters = new QWidget;
    auto *frow = new QHBoxLayout(filters);
    frow->setContentsMargins(0, 0, 0, 0);
    frow->setSpacing(5);
    const QList<QPair<QString, QString>> choices = {
        {QStringLiteral("all"), tr("All")}, {QStringLiteral("safe"), tr("Read-only")},
        {QStringLiteral("changes"), tr("Changes settings")}, {QStringLiteral("recent"), tr("Recent")},
        {QStringLiteral("pinned"), tr("Favorites")},
    };
    for (const auto &choice : choices) {
        auto *b = new QPushButton(choice.second);
        const bool active = toolFilter_ == choice.first;
        b->setCheckable(true);
        b->setChecked(active);
        b->setCursor(Qt::PointingHandCursor);
        b->setMinimumHeight(29);
        theme::setLiveSheet(b, [] { return QStringLiteral("QPushButton{border:1px solid %1;background:%2;border-radius:8px;"
                                         "padding:4px 10px;}QPushButton:hover{border-color:%3;}"
                                         "QPushButton:checked{border-color:%3;background:%4;color:%5;font-weight:700;}")
                         .arg(theme::BORDER, theme::BG2, theme::ACCENT, theme::BG3, theme::ACCENT); });
        connect(b, &QPushButton::clicked, this, [this, filter = choice.first] {
            toolFilter_ = filter;
            rebuildToolCatalog();
            toolScroll_->verticalScrollBar()->setValue(0);
        });
        frow->addWidget(b);
    }
    frow->addStretch();
    auto *groupsButton = new QPushButton(collapsed.isEmpty() ? tr("Collapse All") : tr("Expand All"));
    groupsButton->setEnabled(query.isEmpty() && toolAreaFilter_.isEmpty());  // groups stay open while filtering
    connect(groupsButton, &QPushButton::clicked, this, [this, expand = !collapsed.isEmpty()] {
        QStringList ids;
        if (!expand)
            for (const toolcatalog::Area area : toolcatalog::areas()) ids << QString::number(static_cast<int>(area));
        guiSettings().setValue(QStringLiteral("health/catalog_collapsed_groups"), ids);
        rebuildToolCatalog();
    });
    frow->addWidget(groupsButton);
    auto *areaBox = new QComboBox;
    areaBox->setObjectName(QStringLiteral("healthToolAreaFilter"));
    areaBox->addItem(tr("All categories"), QString());
    for (const toolcatalog::Area area : toolcatalog::areas())
        areaBox->addItem(toolcatalog::areaName(area), toolcatalog::areaName(area));
    const int areaIndex = areaBox->findData(toolAreaFilter_);
    areaBox->setCurrentIndex(std::max(0, areaIndex));
    areaBox->setToolTip(tr("Limit the directory to one hardware category."));
    connect(areaBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, areaBox](int index) {
        toolAreaFilter_ = areaBox->itemData(index).toString();
        rebuildToolCatalog();
        toolScroll_->verticalScrollBar()->setValue(0);
    });
    frow->addWidget(areaBox);
    v->addWidget(filters);

    int shown = 0;
    for (const toolcatalog::Area area : toolcatalog::areas()) {
        QList<const Tool *> list;
        for (const Tool &t : std::as_const(tools_))
            if (toolcatalog::tool(t.key).area == area && matchesFilter(t)) list << &t;
        if (list.isEmpty()) continue;
        shown += list.size();
        const QString title = toolcatalog::areaName(area);
        const QString areaId = QString::number(static_cast<int>(area));
        const bool expanded = !query.isEmpty() || !collapsed.contains(areaId) || !toolAreaFilter_.isEmpty();
        auto *section = new QFrame;
        section->setObjectName(QStringLiteral("healthCatalogSection"));
        theme::setLiveSheet(section, [] { return QStringLiteral("QFrame#healthCatalogSection{background:%1;border:1px solid %2;"
                                            "border-radius:10px;}QFrame#healthCatalogSection QLabel{border:none;background:transparent;}")
                               .arg(theme::BG1, theme::BORDER); });
        auto *sectionLayout = new QVBoxLayout(section);
        sectionLayout->setContentsMargins(7, 6, 7, expanded ? 8 : 6);
        sectionLayout->setSpacing(7);
        auto *groupRow = new QWidget;
        auto *groupLayout = new QHBoxLayout(groupRow);
        groupLayout->setContentsMargins(0, 0, 6, 0);
        groupLayout->setSpacing(6);
        auto *group = new QPushButton(QStringLiteral("%1   %2")
                                      .arg(expanded ? QStringLiteral("▾") : QStringLiteral("▸"), title));
        group->setObjectName(QStringLiteral("healthToolCategory"));
        group->setAccessibleName(tr("%1 category, %2 tools, %3")
                                  .arg(title).arg(list.size()).arg(expanded ? tr("expanded") : tr("collapsed")));
        group->setCursor(Qt::PointingHandCursor);
        // Matching groups remain open for search/category filters; make this explicit instead
        // of presenting a collapse button that appears to do nothing.
        if (!query.isEmpty() || !toolAreaFilter_.isEmpty()) {
            group->setEnabled(false);
            group->setToolTip(tr("Groups stay expanded while search or category filtering is active."));
        }
        theme::setLiveSheet(group, [] { return QStringLiteral("QPushButton#healthToolCategory{border:0;background:transparent;"
                                            "font-weight:700;text-align:left;padding:5px 7px;}"
                                            "QPushButton#healthToolCategory:hover{color:%1;}").arg(theme::ACCENT); });
        groupLayout->addWidget(group, 1);
        auto *itemCount = new QLabel(tr("%1 tools").arg(list.size()));
        theme::setLiveSheet(itemCount, [] { return QStringLiteral("color:%1;font-size:8pt;").arg(theme::FG_DIM); });
        groupLayout->addWidget(itemCount);
        connect(group, &QPushButton::clicked, this, [this, areaId] {
            QStringList hidden = guiSettings().value(QStringLiteral("health/catalog_collapsed_groups")).toStringList();
            if (hidden.contains(areaId)) hidden.removeAll(areaId);
            else hidden << areaId;
            guiSettings().setValue(QStringLiteral("health/catalog_collapsed_groups"), hidden);
            rebuildToolCatalog();
        });
        sectionLayout->addWidget(groupRow);
        if (expanded) sectionLayout->addWidget(cardsGrid(list, toolColumns_, false));
        v->addWidget(section);
    }
    if (!shown) {
        auto *none = new QLabel(tr("No tool matches the current search or filter. Try a device, symptom or error code, or clear the filters."));
        none->setWordWrap(true);
        theme::setLiveSheet(none, [] { return QStringLiteral("color:%1;padding:12px;").arg(theme::FG_DIM); });
        v->addWidget(none);
        auto *reset = new QPushButton(tr("Clear search and filters"));
        connect(reset, &QPushButton::clicked, this, [this] {
            toolSearch_->clear();
            toolFilter_ = QStringLiteral("all");
            toolAreaFilter_.clear();
            rebuildToolCatalog();
        });
        v->addWidget(reset, 0, Qt::AlignLeft);
    }
    v->addStretch();
    // Keep the scroll position while starring a tool or expanding one group.
    const int scrollPos = toolScroll_->verticalScrollBar()->value();
    QWidget *old = toolScroll_->takeWidget();
    toolScroll_->setWidget(catalog);
    toolCatalog_ = catalog;
    // A click can originate inside the old catalog. Destroy it only after the
    // currently executing signal handler returns (prevents a use-after-free).
    if (old) old->deleteLater();
    toolScroll_->verticalScrollBar()->setValue(scrollPos);
}

bool HealthTab::eventFilter(QObject *watched, QEvent *event) {
    if (toolScroll_ && watched == toolScroll_->viewport() && event->type() == QEvent::Show && toolCatalogStale_)
        rebuildToolCatalog();
    if (toolScroll_ && watched == toolScroll_->viewport() && event->type() == QEvent::Resize &&
        (std::clamp(std::max(toolScroll_->viewport()->width(), 300) / 480, 1, 2) != toolColumns_ ||
         std::clamp(std::max(toolScroll_->viewport()->width(), 300) / 390, 1, 3) != toolFeaturedColumns_))
        QTimer::singleShot(0, this, [this] { rebuildToolCatalog(); });  // not while the viewport is being laid out
    return QWidget::eventFilter(watched, event);
}

// ── Navigation: search, targets, history ───────────────────────────────────────

void HealthTab::filterNavigation(const QString &query) {
    for (int i = 0; i < navigation_->topLevelItemCount(); ++i) {
        auto *group = navigation_->topLevelItem(i);
        if (group == toolResults_) continue;
        int visible = 0;
        for (int n = 0; n < group->childCount(); ++n) {
            auto *item = group->child(n);
            const bool match = toolcatalog::matches(query, item->text(0) + QLatin1Char('|') + group->text(0),
                                                    toolcatalog::pageKeywords(item->data(0, Qt::UserRole).toString()));
            item->setHidden(!match);
            if (match) ++visible;
        }
        group->setHidden(!visible);
        if (!query.isEmpty()) group->setExpanded(true);
    }
    int tools = 0;
    for (int n = 0; n < toolResults_->childCount(); ++n) {
        auto *item = toolResults_->child(n);
        const QString key = item->data(0, Qt::UserRole).toString().mid(5);
        const toolcatalog::Info info = toolcatalog::tool(key);
        const bool match = !query.trimmed().isEmpty() &&
                           toolcatalog::matches(query, item->text(0) + QLatin1Char('|') + toolcatalog::areaName(info.area) + QLatin1Char('|') + key, info.keywords);
        item->setHidden(!match);
        if (match) ++tools;
    }
    toolResults_->setHidden(!tools);
    toolResults_->setExpanded(true);
}

QList<HealthTab::NavigationEntry> HealthTab::navigationEntries() const {
    QList<NavigationEntry> entries;
    for (auto it = pageItems_.cbegin(); it != pageItems_.cend(); ++it) {
        const QString key = it.value()->data(0, Qt::UserRole).toString();
        if (!key.isEmpty()) entries.append({QStringLiteral("page:") + key, it.value()->text(0), toolcatalog::pageKeywords(key)});
    }
    for (const Tool &t : tools_) {
        const auto info = toolcatalog::tool(t.key);
        entries.append({QStringLiteral("tool:") + t.key, t.label, info.keywords + QStringList{t.key, toolcatalog::areaName(info.area)}});
    }
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
        const int cmp = QString::localeAwareCompare(a.label, b.label);
        return cmp == 0 ? a.target < b.target : cmp < 0;
    });
    return entries;
}

QWidget *HealthTab::pageByKey(const QString &key) const {
    if (DiagnosticsPage::tabForKey(key) >= 0) return diagnostics_;   // Boot History, Evidence Lab and Investigations are tabs now
    for (auto it = pageItems_.cbegin(); it != pageItems_.cend(); ++it)
        if (it.value()->data(0, Qt::UserRole).toString() == key) return it.key();
    return nullptr;
}

QString HealthTab::targetLabel(const QString &target) const {
    if (target.startsWith(QLatin1String("tool:"))) {
        const Tool *t = toolByKey(target.mid(5));
        return t ? t->label : QString();
    }
    if (target.startsWith(QLatin1String("page:"))) {
        if (const int tab = DiagnosticsPage::tabForKey(target.mid(5)); tab >= 0)
            return pageItems_.value(diagnostics_)->text(0) + QStringLiteral(" › ") + diagnostics_->tabTitle(tab);
        QWidget *page = pageByKey(target.mid(5));
        return page ? pageItems_.value(page)->text(0) : QString();
    }
    return {};
}

bool HealthTab::navigateTo(const QString &target) {
    if (target.startsWith(QLatin1String("tool:"))) {
        const Tool *t = toolByKey(target.mid(5));
        if (!t) return false;
        if (navSearch_ && !navSearch_->text().isEmpty()) navSearch_->clear();
        openTool(t->page);
        return true;
    }
    if (target.startsWith(QLatin1String("page:"))) {
        QWidget *page = pageByKey(target.mid(5));
        if (!page) return false;
        if (const int tab = DiagnosticsPage::tabForKey(target.mid(5)); tab >= 0)
            diagnostics_->showTab(static_cast<DiagnosticsPage::Tab>(tab));
        if (page == toolDirectory_ && toolMode_->currentIndex() != 0) {
            const bool was = replaying_;
            replaying_ = true;
            toolMode_->setCurrentIndex(0);
            replaying_ = was;
        }
        showPage(page);
        return true;
    }
    return false;
}

void HealthTab::record(const QString &target) {
    if (!replaying_ && (historyPos_ < 0 || history_.value(historyPos_) != target)) {
        history_ = history_.mid(0, historyPos_ + 1);
        history_ << target;
        if (history_.size() > 50) history_.removeFirst();
        historyPos_ = int(history_.size()) - 1;
    }
    if (back_) back_->setEnabled(historyPos_ > 0);
    if (forward_) forward_->setEnabled(historyPos_ + 1 < history_.size());
}

void HealthTab::stepHistory(int delta) {
    const int pos = historyPos_ + delta;
    if (pos < 0 || pos >= history_.size()) return;
    historyPos_ = pos;
    replaying_ = true;
    navigateTo(history_.at(pos));
    replaying_ = false;
    record(history_.at(pos));  // only updates the buttons while replaying_ is off and the entry matches
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
    dayHistoryLimited_ = false;
    if (dayFilter_.isEmpty()) return;
    // health.log is bounded and rotated; a selected historical day may have
    // fallen out of retention. Never claim that an absent row means no event.
    QSet<QString> seen;
    for (int i = 0; i < list_->topLevelItemCount(); ++i) {
        const auto *item = list_->topLevelItem(i);
        if (item->data(0, Qt::UserRole + 2).toString() != dayFilter_) continue;
        const auto event = QJsonDocument::fromJson(item->data(0, Qt::UserRole).toByteArray()).object();
        const auto boot = event.value(QStringLiteral("boot_id")).toString().toLower();
        const auto seq = event.value(QStringLiteral("seq")).toInteger(-1);
        if (seq >= 0 && !diagnostichistory::fullBoot(boot.toStdString()).empty())
            seen.insert(boot + QLatin1Char('/') + QString::number(seq));
    }
    const auto tail = diagsrc::savedLog(diagsrc::SavedLog::Health, MAX_ROWS, LOG_MAX_BYTES, dayFilter_);
    dayHistoryLimited_ = tail.omittedLines > 0 || std::any_of(tail.files.cbegin(), tail.files.cend(), [](const auto &file) {
        return file.omittedBytes > 0 || file.rejectedLines > 0 || file.incompleteTail
            || file.state == diagsrc::LogFileState::Unreadable;
    });
    for (auto it = tail.lines.crbegin(); it != tail.lines.crend(); ++it) {
        const auto recordedAt = QDateTime::fromString(it->time, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        if (!recordedAt.isValid()) { dayHistoryLimited_ = true; continue; }
        if (!diagnostichistory::fullBoot(it->boot.toStdString()).empty()) {
            const auto key = it->key();
            if (seen.contains(key)) continue;
            seen.insert(key);
        }
        const auto &level = it->level;
        const auto &original = it->title;
        const auto &raw = it->text;
        const QJsonObject e{{QStringLiteral("kind"), it->kind}, {QStringLiteral("title"), original},
                            {QStringLiteral("level"), level}, {QStringLiteral("text"), raw},
                            {QStringLiteral("boot_id"), it->boot},
                            {QStringLiteral("wall_ms"), double(recordedAt.toMSecsSinceEpoch())}};
        auto *item = new QTreeWidgetItem({it->time.left(16),
                                           level == QLatin1String("critical") ? tr("Critical") :
                                           level == QLatin1String("error") ? tr("Error") : tr("Notice"),
                                           it->kind.toUpper(), technicalEvents_->isChecked() ? original : friendlyEvent(e)});
        item->setToolTip(3, tr("Saved record (may be from a previous boot):\n%1\n%2").arg(original, raw));
        item->setData(0, Qt::UserRole + 2, dayFilter_);
        item->setData(0, Qt::UserRole + 3, true);
        item->setData(0, Qt::UserRole + 4, level);
        item->setData(0, Qt::UserRole, QJsonDocument(e).toJson(QJsonDocument::Compact));
        item->setData(0, Qt::UserRole + 5, QJsonDocument(e).toJson(QJsonDocument::Compact));  // for the explanation pane
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
    if (!dayFilter_.isEmpty() && dayHistoryLimited_)
        dayInfo_->setText(dayInfo_->text() + QLatin1Char(' ') + tr("Some saved history is outside the scan or display limits, or could not be read."));
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
    if (kind == QLatin1String("f2fs")) return tr("F2FS reported an integrity, checkpoint or I/O error");
    if (kind == QLatin1String("igpu")) return tr("Integrated graphics reported a problem");
    if (kind == QLatin1String("amdgpu")) return tr("AMD graphics hung and was reset");
    if (kind == QLatin1String("wifi")) return tr("Wireless device or firmware reported a problem");
    if (kind == QLatin1String("bluetooth")) return tr("Bluetooth firmware or controller reported a failure");
    if (kind == QLatin1String("thermal")) return tr("A temperature-related warning was recorded");
    if (kind == QLatin1String("oom")) return tr("System ran out of available memory");
    if (kind == QLatin1String("oops")) return tr("A kernel error or warning was recorded");
    if (kind == QLatin1String("d3cold")) return tr("GPU power-state transition reported a problem");
    return e.value(QStringLiteral("title")).toString();
}

QJsonObject HealthTab::eventOf(const QTreeWidgetItem *item) const {
    if (!item) return {};
    const QJsonObject live = QJsonDocument::fromJson(item->data(0, Qt::UserRole).toByteArray()).object();
    return live.isEmpty() ? QJsonDocument::fromJson(item->data(0, Qt::UserRole + 5).toByteArray()).object() : live;
}

void HealthTab::showEventDetail() {
    if (!eventDetail_) return;
    auto *row = static_cast<QHBoxLayout *>(eventActions_->layout());
    while (QLayoutItem *item = row->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    eventActions_->setVisible(false);
    const QJsonObject e = eventOf(list_->currentItem());
    if (e.isEmpty()) { eventDetail_->clear(); return; }
    eventDetail_->setHtml(eventinfo::html(e, friendlyEvent(e)));
    QList<QPair<QString, std::function<void()>>> actions;
    if (e.contains(QStringLiteral("seq")) && !e.value(QStringLiteral("boot_id")).toString().isEmpty())
        actions.append({tr("Show in kernel log"), [this, e] { showInKlog(e); }});
    actions << eventActions(e);
    for (const auto &[label, act] : std::as_const(actions)) {
        auto *b = new QPushButton(label);
        connect(b, &QPushButton::clicked, this, act);
        row->addWidget(b);
    }
    row->addStretch(1);
    eventActions_->setVisible(!actions.isEmpty());
}

QList<QPair<QString, std::function<void()>>> HealthTab::eventActions(const QJsonObject &e) {
    const QString kind = e.value(QStringLiteral("kind")).toString();
    const int code = e.value(QStringLiteral("code")).toInt();
    const QString title = e.value(QStringLiteral("title")).toString();
    QList<QPair<QString, std::function<void()>>> out;
    // The page the event points to, only when there is something to do there now.
    static const QList<int> gpuCurveXids{13, 31, 43, 62, 119, 120};
    if (kind == QLatin1String("d3cold") || (kind == QLatin1String("xid") && code == 79)) {
        if (auto *dgpu = findChild<DgpuPage *>()) out.append({tr("Open dGPU power"), [this, dgpu] { showPage(dgpu); }});
    } else if (kind == QLatin1String("xid") && gpuCurveXids.contains(code) && CurvePage::currentState(QStringLiteral("gpu")) != QLatin1String("stock")) {
        out.append({tr("Reset GPU curve to stock"), [this, title] { curves_->resetGpuCurve(title); }});
    } else if ((kind == QLatin1String("mce") || kind == QLatin1String("lockup")) && CurvePage::currentState(QStringLiteral("cpu")) != QLatin1String("stock")) {
        out.append({tr("Open the CPU curve tab"), [this] { Q_EMIT openTab(sysinfo::isIntel() ? QStringLiteral("Intel Undervolt") : QStringLiteral("Ryzen Curve")); }});
    } else if (kind == QLatin1String("nvme") || kind == QLatin1String("fs") || kind == QLatin1String("f2fs")) {
        if (auto *smart = findChild<SmartPage *>()) out.append({tr("Open SMART"), [this, smart] { showPage(smart); }});
    }
    return out;
}

void HealthTab::updateRelated(const QString &pageKey) {
    auto *row = static_cast<QHBoxLayout *>(relatedBar_->layout());
    while (QLayoutItem *item = row->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    int links = 0;
    for (const QString &target : toolcatalog::relatedForPage(pageKey)) {
        const QString name = targetLabel(target);
        if (name.isEmpty()) continue;  // not on this system
        if (!links) {
            auto *title = new QLabel(pageKey == QLatin1String("kernelparams") ? tr("Change these in:") : tr("Related:"));
            title->setProperty("role", "muted");
            row->addWidget(title);
        }
        auto *b = new QPushButton(name);
        b->setFlat(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(target.startsWith(QLatin1String("tool:")) ? tr("Opens this System tool; it reads its sources when it opens.")
                                                                : tr("Opens this Health page."));
        theme::setLiveSheet(b, [] { return QStringLiteral("QPushButton{color:%1;text-decoration:underline;padding:2px 4px}").arg(theme::ACCENT); });
        connect(b, &QPushButton::clicked, this, [this, target] { QTimer::singleShot(0, this, [this, target] { navigateTo(target); }); });
        row->addWidget(b);
        ++links;
    }
    row->addStretch(1);
    relatedBar_->setVisible(links > 0);
}

void HealthTab::explainEvent(const QJsonObject &e) {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Event Explanation"));
    dialog.resize(640, 520);
    auto *v = new QVBoxLayout(&dialog);
    auto *view = new QTextBrowser;
    view->setOpenLinks(false);
    view->setHtml(eventinfo::html(e, friendlyEvent(e)) +
                  QStringLiteral("<p style='color:%1'>%2</p>").arg(QLatin1String(theme::MUTED),
                      tr("Reported as: %1").arg(e.value(QStringLiteral("title")).toString()).toHtmlEscaped()));
    v->addWidget(view, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *copy = buttons->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    connect(copy, &QPushButton::clicked, &dialog, [view] { QApplication::clipboard()->setText(view->toPlainText()); });
    if (e.contains(QStringLiteral("seq")) && !e.value(QStringLiteral("boot_id")).toString().isEmpty()) {
        auto *klog = buttons->addButton(tr("Show in kernel log"), QDialogButtonBox::ActionRole);
        connect(klog, &QPushButton::clicked, &dialog, [this, e, &dialog] { dialog.accept(); showInKlog(e); });
    }
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    v->addWidget(buttons);
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
        const QJsonObject saved = eventOf(it);
        if (!saved.isEmpty()) {
            menu.addAction(tr("Explain this event"), this, [this, saved] { explainEvent(saved); });
            menu.addAction(tr("Hardware Incident Timeline…"), this, [this, saved] {
                insights::show(this, tr("Hardware Incident Timeline"),
                               insights::incident(saved, saved.value("boot_id").toString()));
            });
        }
        menu.addAction(tr("Copy saved record"), this, [it] {
            QApplication::clipboard()->setText(it->toolTip(3));
        });
        menu.exec(list_->viewport()->mapToGlobal(pos));
        return;
    }
    const QJsonObject e = QJsonDocument::fromJson(it->data(0, Qt::UserRole).toByteArray()).object();
    const QString kind = e.value(QStringLiteral("kind")).toString();
    QMenu menu(this);
    menu.addAction(tr("Explain this event"), this, [this, e] { explainEvent(e); });
    menu.addAction(tr("Show in kernel log"), this, [this, e] { showInKlog(e); });
    menu.addAction(tr("Hardware Incident Timeline…"), this, [this, e] {
        insights::show(this, tr("Hardware Incident Timeline"), insights::incident(e, e.value("boot_id").toString()));
    });
    menu.addAction(tr("Copy event and explanation"), this, [e] {
        QTextBrowser tmp;
        tmp.setHtml(eventinfo::html(e, friendlyEvent(e)));
        QApplication::clipboard()->setText(tmp.toPlainText());
    });
    menu.addAction(tr("Show only %1 events").arg(kind.toUpper()), this, [this, kind] { eventSearch_->setText(kind.toUpper()); });
    menu.addSeparator();
    for (const auto &[label, act] : eventActions(e)) menu.addAction(label, this, act);
    menu.exec(list_->viewport()->mapToGlobal(pos));
}

void HealthTab::updateSummary() {
    const int errors = xid_ + gsp_ + mce_ + aerN_ + lockup_ + other_ +
                       nvme_ + fs_ + igpu_ + wifi_ + thermal_ + oom_ + oops_;
    const QString label = errors ? tr("%1 reported event(s) to review").arg(errors) :
                                   tr("No new events to review");
    summary_->setText(label);
    theme::setLiveSheet(summary_, [errors] { return QStringLiteral("color:%1").arg(errors ? theme::WARN : theme::OK); });
    summary_->setToolTip(tr("Counts events since startup, or since they were last marked reviewed. "
                            "Reviewing does not resolve underlying issues or erase saved history."));
}

QString HealthTab::logPath() {
    return diagsrc::savedLogPath(diagsrc::SavedLog::Health);
}

bool HealthTab::persist(const QJsonArray &ev, const QString &bootId) {
    savedBoot_ = bootId;
    QStringList lines;
    quint64 top = savedSeq_;
    for (const QJsonValue &v : ev) {
        const QJsonObject e = v.toObject();
        const auto seq = quint64(e.value(QStringLiteral("seq")).toInteger());
        if (seq <= savedSeq_) continue;
        top = std::max(top, seq);
        const auto eventMs = e.value(QStringLiteral("wall_ms")).toInteger();
        if (!eventMs) {
            logInfo_->setText(tr("Could not determine the event time."));
            return false;
        }
        auto title = e.value(QStringLiteral("title")).toString();
        auto message = e.value(QStringLiteral("text")).toString();
        for (QString *field : {&title, &message}) {
            field->replace(QLatin1Char('\n'), QStringLiteral(" ⏎ "));
            field->replace(QLatin1Char('\r'), QStringLiteral(" ⏎ "));
        }
        lines << QStringLiteral("%1  boot=%2 seq=%3  %4 %5  %6  |  %7")
                     .arg(QDateTime::fromMSecsSinceEpoch(eventMs).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                     .arg(savedBoot_).arg(seq)
                     .arg(e.value(QStringLiteral("level")).toString().toUpper(), -8)
                     .arg(e.value(QStringLiteral("kind")).toString().toUpper(), -6)
                     .arg(title, message);
    }
    if (lines.isEmpty()) return true;
    const QByteArray bytes = (lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8();
    if (!diagsrc::appendSavedLog(diagsrc::SavedLog::Health, bytes, LOG_MAX_BYTES)) {
        logInfo_->setText(tr("Could not write the saved log."));
        return false;
    }
    savedSeq_ = top;
    updateLogInfo();
    return true;
}

void HealthTab::updateLogInfo() {
    const QFileInfo a(logPath()), b(logPath() + QStringLiteral(".1"));
    const qint64 sz = (a.exists() ? a.size() : 0) + (b.exists() ? b.size() : 0);
    logInfo_->setText(sz ? QStringLiteral("Saved log (all boots): %1 · %2 KiB").arg(a.absoluteFilePath()).arg((sz + 1023) / 1024)
                         : QStringLiteral("Saved log: empty — new kernel errors are appended to %1").arg(a.absoluteFilePath()));
}
