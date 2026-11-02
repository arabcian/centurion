#include "mainwindow.h"
#include <QFile>
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
#include "uinavigator.h"
#include <QApplication>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QAction>
#include <QKeySequence>
#include <QMenu>
#include <QSettings>
#include <QShortcut>
#include <QStandardPaths>
#include <QToolButton>
#include <QLabel>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <malloc.h>
#include <algorithm>

// glibc keeps heap freed by the window's tabs, dialogs and previews mapped;
// once it is back in the tray, hand that memory to the kernel.
static void trimHeap() { ::malloc_trim(0); }

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("Centurion");
    setWindowIcon(appIcon(128));
    // Room for the 2×2 Home grid and the curve tabs' control rows without clipping;
    // still well inside a 1366×768 (or 1920×1080 at 150 %) desktop.
    resize(theme::experimentalEnabled() ? 1320 : 1080, theme::experimentalEnabled() ? 760 : 700);
    setMinimumSize(620, 420);

    tabs_ = new QTabWidget;
    tabs_->setObjectName("mainTabs");  // header-band styling in theme.cpp
    tabs_->setAttribute(Qt::WA_StyledBackground, true);  // paint the band behind the tab bar
    tabs_->tabBar()->setAttribute(Qt::WA_StyledBackground, true);  // ...and the selector rail behind the tabs
    tabs_->setDocumentMode(true);
    tabs_->setUsesScrollButtons(true);
    setCentralWidget(tabs_);
    // Brand mark at the start of the header bezel; the version lives in its tooltip
    // (it used to take a whole status-bar row for four seconds). Floated over
    // the band rather than set as a corner widget: in document mode a corner
    // widget moves the page's top rule up through the tab labels. The rail
    // starts at theme::brandInset(), measured with the wordmark's font.
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
    if (theme::experimentalEnabled()) {
        auto *tag = new QLabel(tr("Experimental"));
        tag->setObjectName(QStringLiteral("experimentalTag"));
        markRow->addWidget(tag);
    }

    // Full tab index is available even when the tab bar must scroll on a
    // smaller screen. This is navigation only: never builds missing hardware
    // pages and never calls a device control.
    if (theme::experimentalEnabled()) {
        sectionsButton_ = new QToolButton(tabs_);
        sectionsButton_->setObjectName(QStringLiteral("experimentalSectionsButton"));
        sectionsButton_->setText(tr("All sections  ▾"));
        sectionsButton_->setToolTip(tr("All available pages, including hardware-specific tools. Ctrl+K to search."));
        sectionsButton_->setPopupMode(QToolButton::InstantPopup);
        sectionsButton_->setMenu(new QMenu(sectionsButton_));
        connect(sectionsButton_->menu(), &QMenu::aboutToShow, this, &MainWindow::populateSectionMenu);
        sectionsButton_->setAutoRaise(false);
        auto *quick = new QShortcut(QKeySequence(QStringLiteral("Ctrl+K")), this);
        quick->setContext(Qt::ApplicationShortcut);
        connect(quick, &QShortcut::activated, this, &MainWindow::showQuickNavigator);
    }

    home_ = new HomeTab;
    tabs_->addTab(home_, tr("Home"));
    connect(home_, &HomeTab::openPageRequested, this, &MainWindow::showTab);
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

    if (theme::experimentalEnabled()) {
        arrangeTabs();
        populateSectionMenu();
        // The last selected main page is restored by identity rather than an
        // index (hardware-gated pages appear/disappear between launches).
        QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("centurion"), QStringLiteral("gui"));
        const QString last = settings.value(QStringLiteral("ui/last_page"), QStringLiteral("Home")).toString();
        for (int i = 0; i < tabs_->count(); ++i)
            if (tabs_->tabBar()->tabData(i).toString() == last) { tabs_->setCurrentIndex(i); break; }
        connect(tabs_, &QTabWidget::currentChanged, this, [this](int i) {
            if (i < 0 || i >= tabs_->count()) return;
            QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("centurion"), QStringLiteral("gui"));
            settings.setValue(QStringLiteral("ui/last_page"), tabs_->tabBar()->tabData(i).toString());
        });
    }
}

NvidiaTab *MainWindow::ensureNvidiaTab() {
    if (nvidia_ || !NvidiaTab::present()) return nvidia_;
    nvidia_ = new NvidiaTab;
    // Same place it has when present at start-up: before AMD GPU / Optimizations.
    const int at = tabs_->indexOf(amdgpu_ ? static_cast<QWidget *>(amdgpu_) : static_cast<QWidget *>(optimize_));
    tabs_->insertTab(at, nvidia_, tr("NVIDIA Curve"));
    tabW_ = -1;
    if (theme::experimentalEnabled()) { arrangeTabs(); populateSectionMenu(); }
    fitTabs();
    Q_EMIT nvidiaTabAdded();
    return nvidia_;
}

// Canonical page IDs are separate from their translated visible captions.
// We rearrange existing QTabWidget pages, never reparent or recreate them:
// Home, Health and all vendor-specific operations keep their connections,
// timers, system helpers and security guard rails.
void MainWindow::arrangeTabs() {
    // The Scenes page is not SceneEngine; it is the remaining QTabWidget page.
    // Preserve every original tab, including fallback zone-lighting devices.
    const QStringList originalNames{QStringLiteral("Home"), QStringLiteral("Health"),
        QStringLiteral("Ryzen Curve"), QStringLiteral("Intel Undervolt"),
        QStringLiteral("NVIDIA Curve"), QStringLiteral("AMD GPU"),
        QStringLiteral("Optimizations"), QStringLiteral("Scenes"),
        QStringLiteral("Lighting"), QStringLiteral("Firmware")};
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

void MainWindow::populateSectionMenu() {
    if (!sectionsButton_) return;
    QMenu *menu = sectionsButton_->menu();
    menu->clear();
    QAction *find = menu->addAction(tr("Find a setting…    Ctrl+K"));
    connect(find, &QAction::triggered, this, &MainWindow::showQuickNavigator);
    menu->addSeparator();
    for (int i = 0; i < tabs_->count(); ++i) {
        QWidget *page = tabs_->widget(i);
        QAction *action = menu->addAction(tabs_->tabText(i));
        action->setCheckable(true);
        action->setChecked(i == tabs_->currentIndex());
        action->setEnabled(tabs_->isTabEnabled(i));
        connect(action, &QAction::triggered, this, [this, page] {
            const int n = tabs_->indexOf(page);
            if (n >= 0) tabs_->setCurrentIndex(n);
        });
    }
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
    QTabBar *bar = tabs_->tabBar();
    const int room = tabs_->width() - theme::brandInset() - (sectionsButton_ ? 148 : 8);
    const int w = std::max(120, std::min(bar->sizeHint().width(), room));
    if (w != tabW_) { tabW_ = w; bar->setMaximumWidth(w); }
}

void MainWindow::resizeEvent(QResizeEvent *e) {
    QMainWindow::resizeEvent(e);
    fitTabs();
    placeHeaderControls();
}

void MainWindow::showEvent(QShowEvent *e) {
    QMainWindow::showEvent(e);
    fitTabs();
    // Centred on the tab band (its height is known once the style has polished it).
    mark_->adjustSize();  // the style sheet arrives after the constructor (theme.h)
    mark_->move(theme::BRAND_X, std::max(0, (tabs_->tabBar()->height() - mark_->height()) / 2));
    mark_->raise();
    placeHeaderControls();
    // First time the window opens (not the tray-only start): the setup wizard, once per user.
    if (!wizardChecked_) { wizardChecked_ = true; QTimer::singleShot(0, this, [this] { FirstRunWizard::maybeShow(this); }); }
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
    QMainWindow::hideEvent(e);
    QTimer::singleShot(2000, this, [this] { if (!isVisible()) trimHeap(); });
}

void MainWindow::closeEvent(QCloseEvent *e) {
    if (hideOnClose_ && !forceQuit_) { e->ignore(); hide(); return; }
    QMainWindow::closeEvent(e);
}
