#include "mainwindow.h"
#include <QFile>
#include <QFrame>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QLayoutItem>
#include <QPushButton>
#include <QFont>
#include <QScreen>
#include <QFontMetrics>
#include <QHash>
#include "acpievents.h"
#include "amdgputab.h"
#include "fwattrtab.h"
#include "hometab.h"
#include "inteltab.h"
#include "lighting.h"
#include "lightingtab.h"
#include "nvidiatab.h"
#include "optimizetab.h"
#include "ryzentab.h"
#include "scenes.h"
#include "scenestab.h"
#include "shortcuts.h"
#include "sysinfo.h"
#include "theme.h"
#include "tray.h"
#include "zone4tab.h"
#include "healthtab.h"
#include "launcherwatch.h"
#include "driftwatch.h"
#include "smartwatch.h"
#include "coolingpage.h"
#include "lidwatch.h"
#include "firstrunwizard.h"
#include "mangohud.h"
#include "platformprofile.h"
#include "trialbar.h"
#include "backuppage.h"
#include "cosearchdialog.h"
#include "cpustressbox.h"
#include "gpustressbox.h"
#include "memtestbox.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QPointer>
#include "uinavigator.h"
#include "experimentalui.h"
#include <QApplication>
#include <QProcess>
#include <QStandardPaths>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QAction>
#include <QKeySequence>
#include <QMenu>
#include <QSettings>
#include <QSplitter>
#include <QSplitterHandle>
#include <QSet>
#include <QCursor>
#include <QShortcut>
#include <QStandardPaths>
#include <QToolButton>
#include <QLabel>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QDateTime>
#include <QPlainTextEdit>
#include <QStyle>
#include <utility>
#include <malloc.h>
#include <algorithm>

// glibc keeps heap freed by the window's tabs, dialogs and previews mapped;
// once it is back in the tray, hand that memory to the kernel.
static void trimHeap() { ::malloc_trim(0); }

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("Centurion");
    setWindowIcon(appIcon(128));
    // A compact, independent shell in experimental mode. The old shell remains
    // completely intact under Classic; the device pages are exactly the same.
    const QSize preferred(theme::experimentalEnabled() ? 1480 : 1080, theme::experimentalEnabled() ? 720 : 700);
    const QScreen *screen = QApplication::primaryScreen();
    resize(screen ? preferred.boundedTo(screen->availableGeometry().size() - QSize(24, 48)) : preferred);
    setMinimumSize(620, 420);

    tabs_ = new QTabWidget;
    tabs_->setObjectName("mainTabs");  // header-band styling in theme.cpp
    tabs_->setAttribute(Qt::WA_StyledBackground, true);  // paint the band behind the tab bar
    tabs_->tabBar()->setAttribute(Qt::WA_StyledBackground, true);  // ...and the selector rail behind the tabs
    tabs_->setDocumentMode(true);
    tabs_->setUsesScrollButtons(true);
    if (!theme::experimentalEnabled()) setCentralWidget(tabs_);
    if (!theme::experimentalEnabled()) {
        // Classic retains its original header, tab strip and positioning.
        mark_ = new QWidget(tabs_);
        mark_->setObjectName("appMark");
        mark_->setToolTip(QStringLiteral("Centurion " CENTURION_VERSION));
        auto *markRow = new QHBoxLayout(mark_);
        markRow->setContentsMargins(0, 0, 0, 0);
        markRow->setSpacing(theme::BRAND_GAP);
        auto *markIcon = new QLabel;
        markIcon->setPixmap(appIcon(64).pixmap(QSize(theme::BRAND_ICON, theme::BRAND_ICON)));
        auto *wordmark = new QLabel(QStringLiteral("Centurion"));
        wordmark->setObjectName("appWordmark");
        markRow->addWidget(markIcon);
        markRow->addWidget(wordmark);
    }

    home_ = new HomeTab;
    tabs_->addTab(home_, tr("Home"));
    connect(home_, &HomeTab::openPageRequested, this, &MainWindow::showTab);
    connect(home_, &HomeTab::navigationPreferencesRequested, this, &MainWindow::showNavigationPreferences);
    const int scenesAt = tabs_->count();  // Scenes sits right after Home; built last (it reads every tab)
    // Every tab below exists only where its hardware/firmware interface does,
    // so nobody is offered a control their machine can't honour.
    if (FwattrTab::present()) {
        fwattr_ = new FwattrTab;
        tabs_->addTab(fwattr_, tr("Firmware"));
        // Switching to Custom on Home unlocks this tab immediately (not after its poll).
        connect(home_, &HomeTab::profileChanged, fwattr_, &FwattrTab::refreshLockState);
    }
    // One CPU voltage tab per vendor: Curve Optimizer (AMD SMU) or the
    // OC-mailbox undervolt tab (Intel). The other one is never created, so
    // nothing polls ryzen_smu on Intel or touches MSR 0x150 on AMD.
    // Curve Optimizer exists from Zen 3 mobile on (family 0x19 Zen 3/4, 0x1A Zen 5);
    // Renoir/Picasso (0x17) have no per-core CO, so no tab there.
    const int cpuFamily = [] {
        QFile f(QStringLiteral("/proc/cpuinfo"));
        if (!f.open(QIODevice::ReadOnly)) return 0;
        // procfs reports size 0, so QFile::atEnd() is true before the first
        // read — a readLine() loop never ran and the tab vanished. The field
        // is in the first processor block, well within 4 KiB.
        for (const QByteArray &l : f.read(4096).split('\n'))
            if (l.startsWith("cpu family")) return l.mid(l.indexOf(':') + 1).trimmed().toInt();
        return 0;
    }();
    if (sysinfo::isAmd() && cpuFamily >= 0x19) {
        ryzen_ = new RyzenTab;
        tabs_->addTab(ryzen_, tr("Ryzen Curve"));
    } else if (sysinfo::isIntel()) {
        intel_ = new IntelTab;
        tabs_->addTab(intel_, tr("Intel Undervolt"));
    }
    if (NvidiaTab::present()) {
        nvidia_ = new NvidiaTab;
        tabs_->addTab(nvidia_, tr("NVIDIA Curve"));
    }
    // AMD GPU tuning: discrete Radeon or the CPU's integrated graphics (Hybrid mode).
    if (AmdGpuTab::present()) {
        amdgpu_ = new AmdGpuTab;
        tabs_->addTab(amdgpu_, tr("AMD GPU"));
    }
    optimize_ = new OptimizeTab;
    tabs_->addTab(optimize_, tr("Optimizations"));
    // Per-key RGB keyboard (Legion Gen10 Spectrum controller) — only when present.
    if (lighting::present()) {
        lighting_ = new LightingTab;
        tabs_->addTab(lighting_, tr("Lighting"));
        // Fn+Space cycles the brightness inside the EC; its WMI notification (d0 and e8, both per press) is the
        // only sign of it. Coalesced, and late enough for the controller to report the new level.
        auto *acpi = new AcpiEvents(this);
        auto *fnSpace = new QTimer(this);
        fnSpace->setSingleShot(true);
        fnSpace->setInterval(300);
        connect(fnSpace, &QTimer::timeout, lighting_, &LightingTab::refreshIfClean);
        connect(acpi, &AcpiEvents::event, fnSpace, [fnSpace](const QString &, const QString &, uint type, uint) {
            if (type == 0xd0 || type == 0xe8) fnSpace->start();
        });
    } else if (lighting::zone4Present()) {
        // Older 4-zone RGB keyboard: its own, much smaller page under the same tab name.
        tabs_->addTab(new Zone4Tab, tr("Lighting"));
    }
    health_ = new HealthTab;
    tabs_->addTab(health_, tr("Health"));
    // Settings that calibration and Autotune never touch live with the system tools, not on Optimizations.
    health_->addToolPage(optimize_->homePage(QStringLiteral("network")), tr("Network Tuning"), QStringLiteral("Network tuning"));
    health_->addToolPage(optimize_->homePage(QStringLiteral("storage")), tr("Storage Tuning"), QStringLiteral("Storage tuning"));
    health_->addToolPage(optimize_->homePage(QStringLiteral("kernel")), tr("Kernel Tuning"), QStringLiteral("Kernel tuning"));
    // Cooling's fan max test drives the fans through the Home tab's own controls (the same calls as the tray menu).
    {
        HomeTab *home = home_;
        health_->cooling()->setFanControls(FanControls{
            [home] {
                QList<QPair<QString, int>> out;
                for (const HomeTab::FanInfo &f : home->fanInfo()) out << qMakePair(f.key, f.target);
                return out;
            },
            [home] { return home->fansMaxMode(); },
            [home] { return home->fanBusy(); },
            [home] { home->setAllFansMax(); },
            [home] { home->setAllFansAuto(); },
            [home](const QString &key, int rpm) { home->setFanTarget(key, rpm); },
        });
    }
    connect(health_, &HealthTab::levelChanged, home_, &HomeTab::setHealthLevel);
    connect(health_, &HealthTab::openTab, this, &MainWindow::showTab);

    // The dGPU can come back after start-up (iGPU-only → Default, "Bring NVIDIA back").
    connect(home_, &HomeTab::dgpuRestored, this, &MainWindow::ensureNvidiaTab);
    connect(home_, &HomeTab::setupWizardRequested, this, [this] { FirstRunWizard::showFor(this); });

    scenes_ = new SceneEngine(this);
    home_->setSceneEngine(scenes_);
    optimize_->setSceneEngine(scenes_);
    shortcuts_ = new GlobalShortcuts(scenes_, this);
    shortcuts_->sync();  // talks to the portal only when a scene has a shortcut
    tabs_->insertTab(scenesAt, new ScenesTab(this), tr("Scenes"));
    connect(scenes_, &SceneEngine::transactionState, this, [this](const QString &id, const QString &scene, const QString &step, const QString &state) {
        operationHistory_ << QStringLiteral("%1 · %2 · %3 · %4 · %5").arg(QDateTime::currentDateTime().toString(Qt::ISODate), id, scene, step, scenes::outcomeLabel(state));
        while (operationHistory_.size() > 200) operationHistory_.removeFirst();
    });
    // MangoHud overlay line: kept in a file the overlay's `exec=cat` reads (see mangohud.h).
    connect(scenes_, &SceneEngine::finished, this, &MainWindow::updateOverlay);
    connect(scenes_, &SceneEngine::activeSceneChanged, this, &MainWindow::updateOverlay);
    connect(&pp::Watcher::instance(), &pp::Watcher::changed, this, &MainWindow::updateOverlay);
    connect(qApp, &QCoreApplication::aboutToQuit, this, [] { if (const QString f = mangohud::overlayFile(); !f.isEmpty()) QFile::remove(f); });
    updateOverlay();
    // A scene (ours or a game's) that sets a curve ends a running trial: otherwise the trial's time-out
    // would reset the scene's curve to stock.
    auto endTrials = [this](const QString &name) {
        const auto s = scenes::load(name);
        if (!s) return;
        auto cancelIn = [](QWidget *tab) { if (tab) for (TrialBar *t : tab->findChildren<TrialBar *>()) t->cancel(); };
        if (s->gpu.kind != scenes::Choice::Unchanged) cancelIn(nvidia_);
        if (s->cpu.kind != scenes::Choice::Unchanged) { cancelIn(ryzen_); cancelIn(intel_); }
    };
    connect(scenes_, &SceneEngine::started, this, endTrials);
    connect(scenes_, &SceneEngine::activeSceneChanged, this, [endTrials](const QString &n, bool) { endTrials(n); });

    // Heroic / Bottles game detection (opt-in; no timer while off).
    launchers_ = new LauncherWatch(this);
    connect(optimize_, &OptimizeTab::detectLaunchersChanged, launchers_, &LauncherWatch::setEnabled);

    // Drift detector: settings Centurion applied that something else changed since.
    drift_ = new DriftWatch(this);
    connect(drift_, &DriftWatch::drift, this, [this](const QStringList &lines, const QString &culprits) {
        optimize_->setDrift(lines, culprits, [this] { drift_->reapply(); });
    });

    // Daily SMART sample (admins only) and its alerts.
    smart_ = new SmartWatch(this);
    // Lid shut for long while awake (warm in a bag): the tray warns once per closing.
    lid_ = new LidWatch(this);

    assignPageIds();
    if (theme::experimentalEnabled()) {
        arrangeTabs();
        installExperimentalShell();
    }
    // Keep startup history independent across the two shells. Migrate the old
    // Experimental preference without changing Classic's previous Home default.
    const QString workspace = theme::experimentalEnabled() ? QStringLiteral("ui/experimental/") : QStringLiteral("ui/classic/");
    QSettings startupSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("centurion"), QStringLiteral("gui"));
    const QString defaultMode = theme::experimentalEnabled() && startupSettings.value(QStringLiteral("ui/restore_last_page"), true).toBool()
        ? QStringLiteral("last") : QStringLiteral("Home");
    const QString mode = startupSettings.value(workspace + QStringLiteral("startup"), defaultMode).toString();
    const QString last = mode == QLatin1String("last")
        ? startupSettings.value(workspace + QStringLiteral("last_page"), theme::experimentalEnabled() ? startupSettings.value(QStringLiteral("ui/last_page"), QStringLiteral("Home")) : QVariant(QStringLiteral("Home"))).toString() : mode;
    tabs_->setCurrentIndex(0);
    for (int i = 0; i < tabs_->count(); ++i)
        if (tabs_->isTabEnabled(i) && tabs_->tabBar()->tabData(i).toString() == last) { tabs_->setCurrentIndex(i); break; }
    syncExperimentalNavigation();
    connect(tabs_, &QTabWidget::currentChanged, this, [this, workspace](int i) {
        if (i < 0 || i >= tabs_->count()) return;
        syncExperimentalNavigation();
        QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("centurion"), QStringLiteral("gui"));
        settings.setValue(workspace + QStringLiteral("last_page"), tabs_->tabBar()->tabData(i).toString());
    });
    auto *navigationPreferences = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")), this);
    navigationPreferences->setContext(Qt::WidgetWithChildrenShortcut);
    connect(navigationPreferences, &QShortcut::activated, this, &MainWindow::showNavigationPreferences);
    tabs_->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tabs_->tabBar(), &QWidget::customContextMenuRequested, this, [this] { showNavigationPreferences(); });
    QSettings windowSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("centurion"), QStringLiteral("gui"));
    restoreGeometry(windowSettings.value(QStringLiteral("ui/window_geometry")).toByteArray());
    if (expRail_) setExperimentalCollapsed(expUserToggled_ ? expCollapsed_ : width() < 1010);
    auto *operationObserver = new QTimer(this);
    connect(operationObserver, &QTimer::timeout, this, &MainWindow::sampleOperations);
    operationObserver->start(500);
    theme::setRestartGuard([window = QPointer<MainWindow>(this)] {
        if (!window) return true;
        const QStringList operations = window->activeOperations();
        if (!operations.isEmpty()) {
            QMessageBox::information(window.data(), window->tr("Restart deferred"),
                window->tr("Finish the active operations before restarting Centurion. Your preference is saved for the next start.")
                + QStringLiteral("\n\n") + operations.join(QLatin1Char('\n')));
            return false;
        }
        if (QMessageBox::question(window.data(), window->tr("Restart Centurion"),
            window->tr("Restarting discards unsaved editor changes. Continue?"), QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) return false;
        if (!window || !window->activeOperations().isEmpty()) {
            if (window) QMessageBox::information(window.data(), window->tr("Restart deferred"), window->tr("An operation started while the confirmation was open. Restart later."));
            return false;
        }
        return true;
    });
    auto *operationsShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+O")), this);
    connect(operationsShortcut, &QShortcut::activated, this, &MainWindow::showOperations);
    pageHistory_.append(tabs_->currentWidget());
    pageHistoryPos_ = 0;
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        QWidget *page = tabs_->widget(index);
        if (!page || replayingPageHistory_ || pageHistory_.value(pageHistoryPos_) == page) return;
        pageHistory_ = pageHistory_.mid(0, pageHistoryPos_ + 1);
        pageHistory_.append(page);
        if (pageHistory_.size() > 50) pageHistory_.removeFirst();
        pageHistoryPos_ = int(pageHistory_.size()) - 1;
    });
    for (const int delta : {-1, 1}) {
        auto *shortcut = new QShortcut(QKeySequence(delta < 0 ? QStringLiteral("Alt+Left") : QStringLiteral("Alt+Right")), this);
        connect(shortcut, &QShortcut::activated, this, [this, delta] { stepPageHistory(delta); });
    }
    if (!theme::experimentalEnabled()) {
        auto *quick = new QShortcut(QKeySequence(QStringLiteral("Ctrl+K")), this);
        connect(quick, &QShortcut::activated, this, &MainWindow::showQuickNavigator);
    }
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        if (isVisible() && !isMinimized())
            QSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("centurion"), QStringLiteral("gui"))
                .setValue(QStringLiteral("ui/window_geometry"), saveGeometry());
    });
}

NvidiaTab *MainWindow::ensureNvidiaTab() {
    if (nvidia_ || !NvidiaTab::present()) return nvidia_;
    nvidia_ = new NvidiaTab;
    // Same place it has when present at start-up: before AMD GPU / Optimizations.
    const int at = tabs_->indexOf(amdgpu_ ? static_cast<QWidget *>(amdgpu_) : static_cast<QWidget *>(optimize_));
    tabs_->insertTab(at, nvidia_, tr("NVIDIA Curve"));
    tabW_ = -1;
    assignPageIds();
    if (theme::experimentalEnabled()) { arrangeTabs(); rebuildExperimentalNavigation(); }
    fitTabs();
    Q_EMIT nvidiaTabAdded();
    return nvidia_;
}

// Canonical page IDs are separate from their translated visible captions.
// We rearrange existing QTabWidget pages, never reparent or recreate them:
// Home, Health and all vendor-specific operations keep their connections,
// timers, system helpers and security guard rails.
void MainWindow::assignPageIds() {
    // The Scenes page is not SceneEngine; it is the remaining QTabWidget page.
    // Preserve every original tab, including fallback zone-lighting devices.
    const QStringList originalNames{QStringLiteral("Home"), QStringLiteral("Health"),
        QStringLiteral("Ryzen Curve"), QStringLiteral("Intel Undervolt"),
        QStringLiteral("NVIDIA Curve"), QStringLiteral("AMD GPU"), QStringLiteral("Firmware"),
        QStringLiteral("Optimizations"), QStringLiteral("Scenes"),
        QStringLiteral("Lighting")};
    // Initial order at construction may differ; record their canonical IDs
    // once, before moving anything (Qt moves tabData along with the page).
    for (int i = 0; i < tabs_->count(); ++i) {
        if (!tabs_->tabBar()->tabData(i).isValid()) {
            QString id;
            for (const QString &n : originalNames)
                if (tabs_->tabText(i) == tr(n.toUtf8().constData())) { id = n; break; }
            if (!id.isEmpty()) tabs_->tabBar()->setTabData(i, id);
        }
    }
}

void MainWindow::arrangeTabs() {
    assignPageIds();
    const QStringList originalNames{QStringLiteral("Home"), QStringLiteral("Health"),
        QStringLiteral("Ryzen Curve"), QStringLiteral("Intel Undervolt"),
        QStringLiteral("NVIDIA Curve"), QStringLiteral("AMD GPU"), QStringLiteral("Firmware"),
        QStringLiteral("Optimizations"), QStringLiteral("Scenes"), QStringLiteral("Lighting")};
    int target = 0;
    for (const QString &id : originalNames) {
        int from = -1;
        for (int i = 0; i < tabs_->count(); ++i)
            if (tabs_->tabBar()->tabData(i).toString() == id) { from = i; break; }
        if (from < 0) continue;
        if (from != target) tabs_->tabBar()->moveTab(from, target);
        // Shorten labels only where it helps; the full name is still in the
        // tooltip and the canonical ID used by HealthTab::openTab.
        const QString display = id == QLatin1String("Health") ? tr("Health Center")
                              : id == QLatin1String("Optimizations") ? tr("Optimize")
                              : tr(id.toUtf8().constData());
        tabs_->setTabText(target, display);
        tabs_->setTabToolTip(target, tr(id.toUtf8().constData()));
        ++target;
    }
    tabW_ = -1;
}

// A dedicated experimental shell, not a re-skinned tab strip. No duplicate
// widget instances: QTabWidget stays as the page stack (with its bar hidden),
// and the rail only selects pages that were actually created by hardware gates.
void MainWindow::installExperimentalShell() {
    auto *shell = new QWidget(this);
    shell->setObjectName(QStringLiteral("expShell"));
    auto *shellLayout = new QHBoxLayout(shell);
    shellLayout->setContentsMargins(0, 0, 0, 0);
    shellLayout->setSpacing(0);

    expSplitter_ = new QSplitter(Qt::Horizontal, shell);
    expSplitter_->setChildrenCollapsible(false);
    expSplitter_->setHandleWidth(6);
    shellLayout->addWidget(expSplitter_);
    expRail_ = new QFrame(expSplitter_);
    expRail_->setObjectName(QStringLiteral("expRail"));
    auto *rail = new QVBoxLayout(expRail_);
    rail->setContentsMargins(6, 8, 6, 6);
    rail->setSpacing(4);
    auto *brand = new QWidget;
    auto *brandRow = new QHBoxLayout(brand);
    brandRow->setContentsMargins(5, 4, 0, 7);
    brandRow->setSpacing(9);
    auto *brandIcon = new QLabel;
    brandIcon->setObjectName(QStringLiteral("expBrandIcon"));
    brandIcon->setPixmap(appIcon(64).pixmap(26, 26));
    brandRow->addWidget(brandIcon);
    // Wordmark over the version: the rail's header reads as a product, not a label.
    auto *brandText = new QVBoxLayout;
    brandText->setContentsMargins(0, 0, 0, 0);
    brandText->setSpacing(0);
    expBrandLabel_ = new QLabel(QStringLiteral("CENTURION"));
    expBrandLabel_->setObjectName(QStringLiteral("expBrandLabel"));
    brandText->addWidget(expBrandLabel_);
    auto *brandVersion = new QLabel(QStringLiteral("v" CENTURION_VERSION));
    brandVersion->setObjectName(QStringLiteral("expBrandVersion"));
    brandText->addWidget(brandVersion);
    brandRow->addLayout(brandText);
    brandRow->addStretch();
    rail->addWidget(brand);

    auto *line = new QFrame;
    line->setObjectName(QStringLiteral("expRailDivider"));
    line->setFixedHeight(1);
    rail->addWidget(line);
    auto *navScroll = new QScrollArea;
    navScroll->setFrameShape(QFrame::NoFrame);
    navScroll->setWidgetResizable(true);
    navScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    navScroll->setObjectName(QStringLiteral("expNavScroll"));
    auto *nav = new QWidget;
    nav->setObjectName(QStringLiteral("expNavHost"));
    expNavLayout_ = new QVBoxLayout(nav);
    expNavLayout_->setContentsMargins(0, 4, 0, 0);
    expNavLayout_->setSpacing(3);
    navScroll->setWidget(nav);
    rail->addWidget(navScroll, 1);
    expToggle_ = new QPushButton(tr("Collapse navigation"));
    expToggle_->setObjectName(QStringLiteral("expRailToggle"));
    expToggle_->setToolTip(tr("Expand or collapse navigation (Ctrl+B)"));
    connect(expToggle_, &QPushButton::clicked, this, [this] {
        expUserToggled_ = true;
        setExperimentalCollapsed(!expCollapsed_);
    });
    auto *operationsButton = new QPushButton;
    operationsButton->setObjectName(QStringLiteral("btnMini"));
    operationsButton->setText(QStringLiteral("…"));
    operationsButton->setAccessibleName(tr("Active operations"));
    operationsButton->setToolTip(tr("Active operations") + QStringLiteral(" · Ctrl+Shift+O"));
    connect(operationsButton, &QPushButton::clicked, this, &MainWindow::showOperations);
    rail->addWidget(operationsButton);
    rail->addWidget(expToggle_);
    auto *collapseShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+B")), this);
    collapseShortcut->setContext(Qt::ApplicationShortcut);
    connect(collapseShortcut, &QShortcut::activated, expToggle_, &QPushButton::click);
    auto *favoritesButton = new QPushButton(QStringLiteral("☆"));
    favoritesButton->setAccessibleName(tr("Navigation favorites and startup page"));
    favoritesButton->setToolTip(tr("Navigation favorites and startup page") + QStringLiteral(" · Ctrl+Shift+F"));
    connect(favoritesButton, &QPushButton::clicked, this, &MainWindow::showNavigationPreferences);
    rail->insertWidget(rail->count() - 1, favoritesButton);
    expSplitter_->addWidget(expRail_);

    auto *content = new QWidget;
    content->setObjectName(QStringLiteral("expContent"));
    auto *column = new QVBoxLayout(content);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    auto *top = new QFrame;
    top->setObjectName(QStringLiteral("expTopbar"));
    auto *topRow = new QHBoxLayout(top);
    topRow->setContentsMargins(18, 7, 14, 7);
    topRow->setSpacing(8);
    auto *titles = new QVBoxLayout;
    titles->setSpacing(0);
    expPath_ = new QLabel;
    expPath_->setObjectName(QStringLiteral("expTopPath"));
    expTitle_ = new QLabel;
    expTitle_->setObjectName(QStringLiteral("expTopTitle"));
    titles->addWidget(expPath_);
    // The title, then one muted line on what the page is for (clips first on a narrow window).
    auto *titleRow = new QHBoxLayout;
    titleRow->setSpacing(12);
    titleRow->addWidget(expTitle_);
    expHint_ = new QLabel;
    expHint_->setObjectName(QStringLiteral("expTopHint"));
    expHint_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    titleRow->addWidget(expHint_, 1, Qt::AlignBottom);
    titles->addLayout(titleRow);
    topRow->addLayout(titles, 1);
    // What the machine is doing right now, on every page: the power profile and the
    // active scene. Read-only (same sources as the MangoHud line); a click opens the page.
    auto chip = [this, topRow](const char *page) {
        auto *c = new QPushButton;
        c->setObjectName(QStringLiteral("expChip"));
        c->setCursor(Qt::PointingHandCursor);
        c->setFocusPolicy(Qt::NoFocus);
        c->hide();
        const QString id = QString::fromLatin1(page);
        connect(c, &QPushButton::clicked, this, [this, id] { showTab(id); });
        topRow->addWidget(c);
        return c;
    };
    expProfileChip_ = chip("Home");
    expSceneChip_ = chip("Scenes");
    expSceneChip_->setIcon(experimentalui::navigationIcon(QStringLiteral("Scenes")));
    expSceneChip_->setIconSize(QSize(14, 14));
    connect(scenes_, &SceneEngine::finished, this, &MainWindow::syncStatusChips);
    connect(scenes_, &SceneEngine::activeSceneChanged, this, &MainWindow::syncStatusChips);
    connect(&pp::Watcher::instance(), &pp::Watcher::changed, this, &MainWindow::syncStatusChips);
    syncStatusChips();
    topRow->addSpacing(4);
    // A search field in looks, a button in behaviour: it opens the navigator.
    auto *find = new QPushButton;
    find->setObjectName(QStringLiteral("expSearch"));
    find->setToolTip(tr("Jump to a page or a control"));
    find->setAccessibleName(tr("Jump to a page or a control"));
    find->setCursor(Qt::PointingHandCursor);
    find->setMinimumWidth(150);  // narrow window: the field gives way before the title does
    find->setMaximumWidth(292);
    find->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *findRow = new QHBoxLayout(find);
    findRow->setContentsMargins(10, 0, 7, 0);
    findRow->setSpacing(8);
    auto *findIcon = new QLabel;
    findIcon->setPixmap(experimentalui::searchIcon(theme::MUTED).pixmap(14, 14));
    auto *findText = new QLabel(tr("Jump to a page or a control"));
    findText->setObjectName(QStringLiteral("expSearchText"));
    findText->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);  // clips instead of forcing the width
    auto *findKey = new QLabel(QStringLiteral("Ctrl K"));
    findKey->setObjectName(QStringLiteral("expKeycap"));
    for (QLabel *l : {findIcon, findText, findKey}) l->setAttribute(Qt::WA_TransparentForMouseEvents);
    findRow->addWidget(findIcon);
    findRow->addWidget(findText, 1);
    findRow->addWidget(findKey, 0, Qt::AlignVCenter);
    connect(find, &QPushButton::clicked, this, &MainWindow::showQuickNavigator);
    topRow->addWidget(find, 2);
    column->addWidget(top);
    // A thin line in the power profile's colour (the Legion power-button LED): the mode reads at a glance.
    expSignal_ = new QFrame;
    expSignal_->setObjectName(QStringLiteral("expSignal"));
    expSignal_->setFixedHeight(2);
    column->addWidget(expSignal_);
    syncStatusChips();
    tabs_->tabBar()->hide();
    tabs_->setObjectName(QStringLiteral("expPageStack"));
    // A gutter around every page, so content never touches the rail or the window edge.
    auto *pageHost = new QWidget;
    auto *pageLayout = new QVBoxLayout(pageHost);
    pageLayout->setContentsMargins(8, 6, 8, 4);
    pageLayout->addWidget(tabs_);
    column->addWidget(pageHost, 1);
    expSplitter_->addWidget(content);
    expSplitter_->setStretchFactor(0, 0);
    expSplitter_->setStretchFactor(1, 1);
    connect(expSplitter_, &QSplitter::splitterMoved, this, [this](int, int) {
        if (expCollapsed_) return;
        expExpandedWidth_ = expRail_->width();
        QSettings st(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("centurion"), QStringLiteral("gui"));
        st.setValue(QStringLiteral("ui/rail_width"), expExpandedWidth_);
    });
    setCentralWidget(shell);

    auto *quick = new QShortcut(QKeySequence(QStringLiteral("Ctrl+K")), this);
    quick->setContext(Qt::ApplicationShortcut);
    connect(quick, &QShortcut::activated, this, &MainWindow::showQuickNavigator);
    // Ctrl+1…9: the Nth page in the rail (the rail lists only the pages this machine has).
    for (int n = 1; n <= 9; ++n) {
        auto *page = new QShortcut(QKeySequence(QStringLiteral("Ctrl+%1").arg(n)), this);
        connect(page, &QShortcut::activated, this, [this, n] {
            if (n <= expNavButtons_.size() && expNavButtons_[n - 1]->isEnabled()) expNavButtons_[n - 1]->click();
        });
    }
    QSettings widthSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("centurion"), QStringLiteral("gui"));
    expExpandedWidth_ = qBound(0, widthSettings.value(QStringLiteral("ui/rail_width"), 0).toInt(), 480);
    rebuildExperimentalNavigation();
    // Compact automatically on small screens; manual choice persists per user.
    QSettings st(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("centurion"), QStringLiteral("gui"));
    expUserToggled_ = st.contains(QStringLiteral("ui/rail_collapsed"));
    setExperimentalCollapsed(expUserToggled_ ? st.value(QStringLiteral("ui/rail_collapsed")).toBool() : width() < 1010);
}

void MainWindow::rebuildExperimentalNavigation() {
    if (!expNavLayout_) return;
    while (QLayoutItem *item = expNavLayout_->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            // A deferred old auto-exclusive button must not compete with the new rail.
            if (auto *button = qobject_cast<QPushButton *>(widget)) button->setAutoExclusive(false);
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    expNavButtons_.clear();
    QString previousGroup;
    const auto groupFor = [](const QString &id) -> QString {
        if (id == QLatin1String("Home") || id == QLatin1String("Health")) return QStringLiteral("MONITOR");
        if (id == QLatin1String("Ryzen Curve") || id == QLatin1String("Intel Undervolt") ||
            id == QLatin1String("NVIDIA Curve") || id == QLatin1String("AMD GPU") ||
            id == QLatin1String("Firmware")) return QStringLiteral("TUNING");
        return QStringLiteral("WORKSPACE");
    };
    for (int i = 0; i < tabs_->count(); ++i) {
        const QString id = tabs_->tabBar()->tabData(i).toString();
        const QString group = groupFor(id);
        if (group != previousGroup) {
            auto *heading = new QLabel(tr(group.toUtf8().constData()));  // same catalogue entries as the top bar's path
            heading->setObjectName(QStringLiteral("expNavHeading"));
            heading->setVisible(!expCollapsed_);
            expNavLayout_->addWidget(heading);
            previousGroup = group;
        }
        QString label = tabs_->tabText(i);
        if (id == QLatin1String("Home")) label = tr("Overview");
        if (id == QLatin1String("Health")) label = tr("Health");
        if (id == QLatin1String("Optimizations")) label = tr("Optimize");
        const QString code = id == QLatin1String("Home") ? QStringLiteral("HM") :
            id == QLatin1String("Health") ? QStringLiteral("HL") :
            id == QLatin1String("Ryzen Curve") ? QStringLiteral("CPU") :
            id == QLatin1String("Intel Undervolt") ? QStringLiteral("CPU") :
            id == QLatin1String("NVIDIA Curve") ? QStringLiteral("NV") :
            id == QLatin1String("AMD GPU") ? QStringLiteral("GPU") :
            id == QLatin1String("Optimizations") ? QStringLiteral("OPT") :
            id == QLatin1String("Scenes") ? QStringLiteral("SC") :
            id == QLatin1String("Lighting") ? QStringLiteral("RGB") : QStringLiteral("FW");
        auto *button = new QPushButton;
        button->setObjectName(QStringLiteral("expNavButton"));
        button->setCheckable(true);
        button->setAutoExclusive(true);
        button->setProperty("navLabel", label);
        button->setProperty("navCode", code);
        button->setProperty("pageId", id);
        button->setIcon(experimentalui::navigationIcon(id));
        button->setIconSize(QSize(18, 18));
        button->setText(expCollapsed_ ? QString() : label);
        const int position = expNavButtons_.size() + 1;
        button->setToolTip(position <= 9 ? QStringLiteral("%1  ·  Ctrl+%2").arg(label).arg(position) : label);
        button->setEnabled(tabs_->isTabEnabled(i));
        button->setCursor(Qt::PointingHandCursor);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setMinimumHeight(std::max(32, button->fontMetrics().height() + 12));
        button->setAccessibleName(label);
        QWidget *page = tabs_->widget(i);
        connect(button, &QPushButton::clicked, this, [this, page] {
            const int index = tabs_->indexOf(page);
            if (index >= 0) tabs_->setCurrentIndex(index);
        });
        expNavLayout_->addWidget(button);
        expNavButtons_.append(button);
    }
    expNavLayout_->addStretch(1);
    setExperimentalCollapsed(expCollapsed_);
    syncExperimentalNavigation();
}

void MainWindow::setExperimentalCollapsed(bool collapsed) {
    if (!expRail_) return;
    expCollapsed_ = collapsed;
    int expandedWidth = 154;
    for (const auto *button : std::as_const(expNavButtons_))
        expandedWidth = std::max(expandedWidth, button->fontMetrics().horizontalAdvance(button->property("navLabel").toString()) + 54);
    if (collapsed) {
        expRail_->setFixedWidth(48);
    } else {
        const int maximum = std::max(480, expandedWidth);
        expRail_->setMaximumWidth(maximum);
        expRail_->setMinimumWidth(expandedWidth);
        const int desired = qBound(expandedWidth, expExpandedWidth_, maximum);
        expSplitter_->setSizes({desired, std::max(1, width() - desired)});
    }
    expSplitter_->handle(1)->setEnabled(!collapsed);
    expBrandLabel_->setVisible(!collapsed);
    expToggle_->setText(collapsed ? QStringLiteral("»") : tr("«  Hide labels"));
    for (auto *button : std::as_const(expNavButtons_)) {
        button->setText(collapsed ? QString() : button->property("navLabel").toString());
        button->setProperty("collapsed", collapsed);
        button->style()->unpolish(button);
        button->style()->polish(button);
    }
    for (QLabel *heading : expRail_->findChildren<QLabel *>(QStringLiteral("expNavHeading")))
        heading->setVisible(!collapsed);
    for (QLabel *version : expRail_->findChildren<QLabel *>(QStringLiteral("expBrandVersion")))
        version->setVisible(!collapsed);
    if (expUserToggled_) {
        QSettings st(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("centurion"), QStringLiteral("gui"));
        st.setValue(QStringLiteral("ui/rail_collapsed"), collapsed);
    }
}

void MainWindow::showNavigationPreferences() {
    struct Target { QString id, label, healthTarget; };
    QList<Target> targets;
    for (int i = 0; i < tabs_->count(); ++i) {
        const QString id = tabs_->tabBar()->tabData(i).toString();
        if (!id.isEmpty() && tabs_->isTabEnabled(i)) targets.append(Target{QStringLiteral("page/") + id, tabs_->tabText(i), {}});
        if (tabs_->widget(i) == health_ && tabs_->isTabEnabled(i)) {
            for (const auto &entry : health_->navigationEntries())
                targets.append(Target{QStringLiteral("health/") + entry.target, tr("Health") + QStringLiteral(" / ") + entry.label, entry.target});
        }
    }
    QSettings st(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("centurion"), QStringLiteral("gui"));
    const QString key = QStringLiteral("ui/navigation_favorites_v1");
    QStringList favorites = st.value(key).toStringList();
    QSet<QString> valid;
    for (const auto &target : targets) valid.insert(target.id);
    for (qsizetype i = favorites.size(); i > 0; --i) if (!valid.contains(favorites.at(i - 1))) favorites.removeAt(i - 1);
    favorites.removeDuplicates();
    st.setValue(key, favorites);
    QMenu menu(this);
    if (favorites.isEmpty()) menu.addAction(tr("No navigation favorites"))->setEnabled(false);
    for (const auto &target : targets) {
        if (!favorites.contains(target.id)) continue;
        connect(menu.addAction(target.label), &QAction::triggered, this, [this, target] {
            if (!target.healthTarget.isEmpty()) {
                const int index = tabs_->indexOf(health_);
                if (index >= 0 && tabs_->isTabEnabled(index)) { tabs_->setCurrentIndex(index); health_->openNavigationTarget(target.healthTarget); }
            } else {
                for (int i = 0; i < tabs_->count(); ++i)
                    if (tabs_->isTabEnabled(i) && tabs_->tabBar()->tabData(i).toString() == target.id.mid(5)) { tabs_->setCurrentIndex(i); break; }
            }
        });
    }
    auto *edit = menu.addMenu(tr("Manage navigation favorites"));
    for (const auto &target : targets) {
        auto *action = edit->addAction(target.label);
        action->setCheckable(true); action->setChecked(favorites.contains(target.id));
        connect(action, &QAction::toggled, &menu, [&, id = target.id](bool on) {
            if (on && !favorites.contains(id)) favorites.append(id);
            if (!on) favorites.removeAll(id);
            st.setValue(key, favorites);
        });
    }
    auto *startup = menu.addMenu(tr("Startup page"));
    const QString prefix = theme::experimentalEnabled() ? QStringLiteral("ui/experimental/") : QStringLiteral("ui/classic/");
    const QString defaultMode = theme::experimentalEnabled() && st.value(QStringLiteral("ui/restore_last_page"), true).toBool() ? QStringLiteral("last") : QStringLiteral("Home");
    const QString selected = st.value(prefix + QStringLiteral("startup"), defaultMode).toString();
    auto addStartup = [&](const QString &id, const QString &label) {
        auto *action = startup->addAction(label); action->setCheckable(true); action->setChecked(id == selected);
        connect(action, &QAction::triggered, &menu, [&, id] { st.setValue(prefix + QStringLiteral("startup"), id); });
    };
    addStartup(QStringLiteral("last"), tr("Restore last page"));
    for (const auto &target : targets) if (target.healthTarget.isEmpty()) addStartup(target.id.mid(5), target.label);
    menu.exec(QCursor::pos());
}

void MainWindow::syncExperimentalNavigation() {
    if (!expTitle_) return;
    const int i = tabs_->currentIndex();
    if (i < 0 || i >= tabs_->count()) return;
    const QString id = tabs_->tabBar()->tabData(i).toString();
    QString label = tabs_->tabText(i);
    if (id == QLatin1String("Home")) label = tr("Overview");
    if (id == QLatin1String("Health")) label = tr("Health Center");
    expTitle_->setText(label);
    const QString group = id == QLatin1String("Home") || id == QLatin1String("Health") ? tr("MONITOR") :
        id == QLatin1String("Ryzen Curve") || id == QLatin1String("Intel Undervolt") ||
        id == QLatin1String("NVIDIA Curve") || id == QLatin1String("AMD GPU") ||
        id == QLatin1String("Firmware") ? tr("TUNING") : tr("WORKSPACE");
    expPath_->setText(QStringLiteral("CENTURION  /  ") + group);
    static const QHash<QString, const char *> hints{
        {QStringLiteral("Home"), QT_TR_NOOP("Power profile, device switches, fans and live sensors")},
        {QStringLiteral("Health"), QT_TR_NOOP("Diagnostics, logs, sleep, network protection and backups")},
        {QStringLiteral("Ryzen Curve"), QT_TR_NOOP("Per-core Curve Optimizer for the CPU")},
        {QStringLiteral("Intel Undervolt"), QT_TR_NOOP("CPU voltage offsets, power and current limits")},
        {QStringLiteral("NVIDIA Curve"), QT_TR_NOOP("Voltage/frequency curve, clock offsets and memory lock")},
        {QStringLiteral("AMD GPU"), QT_TR_NOOP("Overdrive clocks, voltage, power limit and fan curve")},
        {QStringLiteral("Firmware"), QT_TR_NOOP("CPU and GPU power limits kept by the firmware")},
        {QStringLiteral("Optimizations"), QT_TR_NOOP("System tuning knobs, presets and Autotune")},
        {QStringLiteral("Scenes"), QT_TR_NOOP("One name for the whole machine, switched in one click")},
        {QStringLiteral("Lighting"), QT_TR_NOOP("Keyboard RGB effects, colours and brightness")}};
    const char *hint = hints.value(id);
    expHint_->setText(hint ? tr(hint) : QString());
    for (auto *button : std::as_const(expNavButtons_))
        button->setChecked(button->property("pageId").toString() == id);
}

void MainWindow::syncStatusChips() {
    if (!expProfileChip_) return;
    const QString profile = pp::Watcher::instance().current().value_or(QString());
    expProfileChip_->setVisible(!profile.isEmpty());
    if (!profile.isEmpty()) {
        expProfileChip_->setText(HomeTab::profileLabel(profile));
        expProfileChip_->setIcon(experimentalui::dotIcon(QColor(theme::profileAccent(profile))));
        expProfileChip_->setIconSize(QSize(10, 10));
    }
    if (!expSignal_) return;
    const QString signal = profile.isEmpty() ? QString::fromLatin1(theme::ACCENT) : theme::profileAccent(profile);
    const QByteArray hex = signal.toLatin1();
    theme::setSheet(expSignal_, QStringLiteral("QFrame#expSignal { border: 0; background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
                                               "stop:0 %1, stop:0.4 %2, stop:1 %3); }")
                                    .arg(signal, theme::rgba(hex.constData(), 0.30), QString::fromLatin1(theme::BORDER_SOFT)));
    const QString scene = scenes::activeScene();
    expSceneChip_->setVisible(!scene.isEmpty());
    expSceneChip_->setText(QString(scene).replace(QLatin1Char('&'), QLatin1String("&&")));  // no mnemonic
}

QStringList MainWindow::activeOperations() const {
    QStringList out;
    if (safeModeRunning_) out << tr("Safe mode");
    if (scenes_->operationActive()) out << tr("Scene apply");
    if (home_->operationActive()) out << tr("Device controls");
    if (optimize_->busy()) out << tr("Optimization operation");
    if (ryzen_ && ryzen_->operationActive()) out << tr("Ryzen Curve");
    if (intel_ && intel_->operationActive()) out << tr("Intel Undervolt");
    if (nvidia_ && nvidia_->busy()) out << tr("NVIDIA Curve");
    if (amdgpu_ && amdgpu_->operationActive()) out << tr("AMD GPU");
    if (fwattr_ && fwattr_->operationActive()) out << tr("Firmware");
    if (lighting_ && lighting_->operationActive()) out << tr("Lighting");
    if (health_->backup() && health_->backup()->operationActive()) out << tr("Backup / restore");
    if (health_->cooling() && health_->cooling()->operationActive()) out << tr("Cooling test");
    for (auto *trial : findChildren<TrialBar *>()) if (trial->running()) out << tr("Curve trial");
    for (auto *box : findChildren<CpuStressBox *>()) if (box->operationActive()) out << tr("CPU stress test");
    for (auto *box : findChildren<GpuStressBox *>()) if (box->operationActive()) out << tr("GPU stress test");
    for (auto *box : findChildren<MemTestBox *>()) if (box->operationActive()) out << tr("Memory test");
    for (auto *box : findChildren<CoSearchDialog *>()) if (box->operationActive()) out << tr("CO Search");
    out.removeDuplicates();
    return out;
}

bool MainWindow::canQuit() {
    const auto active = activeOperations();
    if (!active.isEmpty()) {
        QMessageBox::information(this, tr("Exit deferred"), tr("Finish the active operations before exiting Centurion.") + QStringLiteral("\n\n") + active.join(QLatin1Char('\n')));
        return false;
    }
    QStringList unsaved;
    for (const auto *page : findChildren<ScenesTab *>()) if (page->hasUnappliedChanges()) unsaved << tr("Scenes");
    if (lighting_ && lighting_->hasUnappliedChanges()) unsaved << tr("Lighting");
    if (unsaved.isEmpty()) return true;
    const bool discard = QMessageBox::question(this, tr("Unsaved changes"),
        tr("Leaving Centurion will discard unsaved changes on these pages:") + QStringLiteral("\n\n") + unsaved.join(QLatin1Char('\n')),
        QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Discard;
    return discard && activeOperations().isEmpty();
}

void MainWindow::sampleOperations() {
    const auto now = activeOperations();
    const auto time = QDateTime::currentDateTime().toString(Qt::ISODate);
    for (const auto &name : now) if (!observedOperations_.contains(name))
        operationHistory_ << tr("%1 · Observed active: %2").arg(time, name);
    for (const auto &name : observedOperations_) if (!now.contains(name))
        operationHistory_ << tr("%1 · No longer observed: %2 (outcome unknown)").arg(time, name);
    observedOperations_ = now;
    while (operationHistory_.size() > 200) operationHistory_.removeFirst();
}

void MainWindow::showOperations() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Active operations"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *note = new QLabel(tr("Reported by existing GUI operation states; background or external jobs may not appear here."));
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *status = new QLabel;
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    layout->addWidget(status, 1);
    auto *history = new QPlainTextEdit; history->setReadOnly(true);
    layout->addWidget(new QLabel(tr("Observed transitions and scene transaction events (not independent verification)")));
    layout->addWidget(history, 2);
    auto refresh = [this, status, history] {
        sampleOperations();
        const auto text = operationHistory_.join(QLatin1Char('\n'));
        if (history->toPlainText() != text) history->setPlainText(text);
        const QStringList active = activeOperations();
        status->setText(active.isEmpty() ? tr("No tracked operation is active.") : active.join(QLatin1Char('\n')));
    };
    QTimer timer;
    connect(&timer, &QTimer::timeout, &dialog, refresh);
    timer.start(500);
    refresh();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(400, 250);
    dialog.exec();
}

void MainWindow::stepPageHistory(int delta) {
    const int next = pageHistoryPos_ + delta;
    if (next < 0 || next >= pageHistory_.size()) return;
    const int index = tabs_->indexOf(pageHistory_.at(next));
    if (index < 0 || !tabs_->isTabEnabled(index)) return;
    replayingPageHistory_ = true;
    pageHistoryPos_ = next;
    tabs_->setCurrentIndex(index);
    replayingPageHistory_ = false;
}

void MainWindow::showQuickNavigator() { uinavigator::open(tabs_, this); }

void MainWindow::placeHeaderControls() {
    if (!sectionsButton_) return;
    sectionsButton_->adjustSize();
    sectionsButton_->move(std::max(0, tabs_->width() - sectionsButton_->width() - 13),
                           std::max(0, (tabs_->tabBar()->height() - sectionsButton_->height()) / 2));
    sectionsButton_->raise();
}

// The selector rail hugs its keys: document mode would stretch the tab bar (and
// so the rail painted behind it) across the whole window. Text tabs size
// themselves. The style sheet shifts the bar right by the brand inset without
// shortening it, so a narrow window is capped here too: the bar then ends at
// the window edge and shows its scroll buttons instead of running off-screen.
void MainWindow::fitTabs() {
    if (!tabs_ || theme::experimentalEnabled()) return;
    QTabBar *bar = tabs_->tabBar();
    const int room = tabs_->width() - theme::brandInset() - (sectionsButton_ ? 148 : 8);
    const int w = std::max(120, std::min(bar->sizeHint().width(), room));
    if (w != tabW_) { tabW_ = w; bar->setMaximumWidth(w); }
}

void MainWindow::resizeEvent(QResizeEvent *e) {
    QMainWindow::resizeEvent(e);
    fitTabs();
    placeHeaderControls();
    if (expRail_ && !expUserToggled_ && expCollapsed_ != (width() < 1010)) setExperimentalCollapsed(width() < 1010);
}

void MainWindow::showEvent(QShowEvent *e) {
    QMainWindow::showEvent(e);
    fitTabs();
    // Centred on the tab band (its height is known once the style has polished it).
    if (mark_) {
        mark_->adjustSize();
        mark_->move(theme::BRAND_X, std::max(0, (tabs_->tabBar()->height() - mark_->height()) / 2));
        mark_->raise();
    }
    placeHeaderControls();
    // First time the window opens (not the tray-only start): the setup wizard, once per user.
    if (!wizardChecked_) { wizardChecked_ = true; QTimer::singleShot(0, this, [this] { FirstRunWizard::maybeShow(this); }); }
}

void MainWindow::runSafeMode() {
    if (safeModeRunning_) return;
    safeModeRunning_ = true;
    home_->setAllFansAuto();
    const QString tool = QStandardPaths::findExecutable(QStringLiteral("centurion-safe-mode"));
    auto *p = new QProcess(this);
    p->setProcessChannelMode(QProcess::MergedChannels);
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus st) {
        safeModeRunning_ = false;
        p->deleteLater();
        Q_EMIT safeModeFinished(st == QProcess::NormalExit && code == 0, QString::fromUtf8(p->readAll()).trimmed());
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        safeModeRunning_ = false;
        p->deleteLater();
        Q_EMIT safeModeFinished(false, QStringLiteral("centurion-safe-mode not found"));
    });
    p->start(tool.isEmpty() ? QStringLiteral("/usr/bin/centurion-safe-mode") : tool, {});
}

void MainWindow::updateOverlay() {
    mangohud::updateOverlayFile(mangohud::overlayLine(scenes::activeScene(),
                                                      HomeTab::profileLabel(pp::Watcher::instance().current().value_or(QString()), false)));
}

void MainWindow::showTab(const QString &label) {
    for (int i = 0; i < tabs_->count(); ++i) {
        // HealthPage sends canonical English names, not the new short caption.
        if (tabs_->tabBar()->tabData(i).toString() == label ||
            tabs_->tabText(i) == tr(label.toUtf8().constData())) {
            tabs_->setCurrentIndex(i);
            break;
        }
    }
    showNormal();
    raise();
    activateWindow();
}

void MainWindow::hideEvent(QHideEvent *e) {
    if (!isMinimized()) QSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("centurion"), QStringLiteral("gui")).setValue(QStringLiteral("ui/window_geometry"), saveGeometry());
    QMainWindow::hideEvent(e);
    QTimer::singleShot(2000, this, [this] { if (!isVisible()) trimHeap(); });
}

void MainWindow::closeEvent(QCloseEvent *e) {
    if (!isMinimized()) QSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("centurion"), QStringLiteral("gui")).setValue(QStringLiteral("ui/window_geometry"), saveGeometry());
    if (hideOnClose_ && !forceQuit_) { e->ignore(); hide(); return; }
    if (!canQuit()) { e->ignore(); return; }
    QMainWindow::closeEvent(e);
}
