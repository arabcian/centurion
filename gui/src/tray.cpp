#include "tray.h"
#include "coolingpage.h"
#include "launcherwatch.h"
#include "driftwatch.h"
#include "lidwatch.h"
#include "smartwatch.h"
#include "healthtab.h"
#include "hometab.h"
#include "inteltab.h"
#include "lightingtab.h"
#include "mainwindow.h"
#include "notifyprefs.h"
#include "nvidiatab.h"
#include "optimizetab.h"
#include "ryzentab.h"
#include "scenes.h"
#include "theme.h"
#include <QApplication>
#include <algorithm>
#include <QDateTime>
#include <QFile>
#include <QMenu>
#include <QSettings>
#include <QPainter>
#include <QPixmap>
#include <QRectF>
#include <QSvgRenderer>
#include <QTimer>

static constexpr int APPLY_COOLDOWN_MS = 2500;
static constexpr int COOLING_CHECK_MS = 3 * 60 * 1000;  // after login settles

QIcon appIcon(int size) {
    // The installed centurion.svg (compiled in), recoloured to theme::ACCENT / BG1 so window,
    // tray and menu entry show the same glyph. Every common size is rendered from the vector
    // instead of scaling one bitmap down (the tray asks for 22-24 px).
    static const QByteArray src = [] {
        QFile f(QStringLiteral(":/centurion.svg"));
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }();
    QByteArray svg = src;
    svg.replace("#eba55b", theme::ACCENT).replace("#191a1e", theme::BG1);
    QSvgRenderer r(svg);
    QIcon icon;
    if (!r.isValid()) return icon;
    for (int s : {16, 22, 24, 32, 48, 64, 128, 256, size}) {
        QPixmap pm(s, s);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        r.render(&p, QRectF(0, 0, s, s));
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

Tray::Tray(MainWindow *win) : QSystemTrayIcon(appIcon(), win), win_(win), menu_(new QMenu), notifier_(new Notifier(this)) {
    setToolTip("Centurion");
    // Rebuilt on every open: profiles can change behind our back.
    connect(menu_, &QMenu::aboutToShow, this, &Tray::rebuild);
    setContextMenu(menu_);
    connect(this, &QSystemTrayIcon::activated, this, [this](ActivationReason r) {
        if (r == Trigger || r == DoubleClick) toggleWindow();
    });
    rebuild();

    // Hardware/driver faults found by the Health tab's background scan.
    connect(win_->health(), &HealthTab::alert, this, [this](const QString &category, const QString &severity, const QString &t, const QString &m) {
        gatedNotify(category, severity, t, m);
    });
    // The dGPU stayed awake on battery (DgpuUsage): one alert per stretch, with a button to its page.
    connect(win_->health(), &HealthTab::dgpuAwake, this, [this](qint64 minutes, const QString &holders) {
        const QString who = holders.isEmpty() ? tr("Nothing identified as holding it.") : tr("Held by: %1").arg(holders);
        notify(tr("dGPU awake for %1 min on battery").arg(minutes), who + tr("\nHealth → dGPU power for details."),
               {{tr("Show"), [this] { win_->health()->openDgpuPage(); win_->showTab(QStringLiteral("Health")); }}});
    });
    // Fan guard dropped fixed fan speeds after a temperature spike.
    connect(win_->home(), &HomeTab::alert, this, [this](const QString &t, const QString &m) { notify(t, m); });
    // Curve guard reset a curve after an error: offer to put the profile back (same path as the tray menu).
    connect(win_->health(), &HealthTab::curveReset, this, [this](const QString &group, const QString &profile, const QString &m) {
        const bool gpu = group == QLatin1String("gpu");
        const QString title = gpu ? tr("GPU curve guard") : tr("CPU curve guard");
        if (profile.isEmpty()) { gatedNotify(notifyprefs::CURVE, QString(), title, m); return; }
        gatedNotify(notifyprefs::CURVE, QString(), title, m + tr("\nWas: '%1'").arg(profile), {{tr("Undo"), [this, gpu, title, profile] {
            if (!claimCooldown()) return;
            if (gpu) {
                if (NvidiaTab *nv = win_->nvidia()) { nv->applyNamedProfile(profile); notify(title, tr("Applying profile '%1'…").arg(profile)); }
                return;
            }
            win_->scenes()->applyCpuProfile(profile, false, [this, title, profile](bool ok, const QString &r) {
                notify(title, ok ? tr("Profile '%1' applied again.").arg(profile) : tr("Profile '%1': %2").arg(profile, r));
            });
        }}});
    });

    // Heroic / Bottles games noticed by the launcher watch.
    if (LauncherWatch *lw = win_->launchers()) {
        connect(lw, &LauncherWatch::gameStarted, this, [this](const QString &g, const QString &src) {
            notify(tr("%1 game detected").arg(src), tr("%1 — game mode started.").arg(g));
        });
        connect(lw, &LauncherWatch::gameEnded, this, [this](const QString &g, const QString &src) {
            notify(tr("%1 game closed").arg(src), tr("%1 — game mode ended.").arg(g));
            flushDeferred();
        });
    }

    if (DriftWatch *dw = win_->drift()) {
        connect(dw, &DriftWatch::drift, this, [this, dw](const QStringList &lines, const QString &culprits) {
            if (lines.isEmpty()) return;
            notify(tr("Settings changed outside Centurion"),
                   lines.mid(0, 3).join('\n') + (lines.size() > 3 ? QStringLiteral("\n…") : QString())
                       + (culprits.isEmpty() ? QString() : tr("\nRunning: %1").arg(culprits))
                       + tr("\nOptimizations tab → Re-apply."),
                   {{tr("Re-apply"), [dw] { dw->reapply(); }}});
        });
    }

    if (SmartWatch *sw = win_->smart()) {
        connect(sw, &SmartWatch::alert, this, [this](const QStringList &lines) {
            gatedNotify(notifyprefs::SMART, QString(), tr("Disk health"), lines.join(QLatin1Char('\n')) + tr("\nHealth → SMART for details."));
        });
    }

    // Lid shut for longer than the Sleep page's limit while the machine stays awake.
    if (LidWatch *lw = win_->lid()) {
        connect(lw, &LidWatch::warm, this, [this](const QString &m) { gatedNotify(notifyprefs::HEAT, QString(), tr("Laptop awake with the lid closed"), m); });
    }

    // Scene results: a notification when nobody is looking at the Scenes tab
    // (tray pick, charger plugged/pulled); the tab shows its own status line.
    SceneEngine *eng = win_->scenes();
    connect(eng, &SceneEngine::finished, this, [this](const QString &n, bool ok, const QStringList &log) {
        if (win_->isVisible() && win_->isActiveWindow()) return;
        // Informational messages stay silent, except the ones that say automatic switching is NOT doing its job
        // (power source flapping, login apply skipped): nobody is looking at the tab when those happen.
        const QList<NotifyAction> details{{tr("Details"), [this] { win_->showTab(QStringLiteral("Scenes")); }}};
        if (n.isEmpty()) { if (!ok && !log.isEmpty()) notify(tr("Scenes"), log.join('\n'), details); return; }
        QStringList bad;
        for (const QString &l : log) if (l.startsWith(QStringLiteral("✗"))) bad << l.mid(2);
        notify(tr("Scene: %1").arg(n), ok ? tr("Applied.") : bad.join('\n'), ok ? QList<NotifyAction>() : details);
    });
    connect(eng, &SceneEngine::activeSceneChanged, this, [this](const QString &n, bool game) {
        setToolTip(tr("Centurion — scene: %1").arg(n));
        if (!game) flushDeferred();
        if (win_->isVisible() && win_->isActiveWindow()) return;
        notify(game ? tr("Game scene: %1").arg(n) : tr("Scene: %1").arg(n),
               game ? tr("Game started — scene switched.") : tr("Scene switched."));
    });
    connect(eng, &SceneEngine::started, this, [this](const QString &n) { setToolTip(tr("Centurion — applying scene '%1'…").arg(n)); });
    connect(eng, &SceneEngine::finished, this, [this](const QString &n) { if (!n.isEmpty()) setToolTip(tr("Centurion — scene: %1").arg(n)); });

    // An overdue cooling test: checked once, a few minutes after start-up (as the backup reminder's first check); nothing repeats.
    QTimer::singleShot(COOLING_CHECK_MS, this, [this] { coolingReminder(); });
}

void Tray::coolingReminder() {
    const QSettings st(coolingreminder::settingsPath(), QSettings::IniFormat);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QFile store(coolingreminder::storePath());
    const qint64 lastClean = store.open(QIODevice::ReadOnly) ? coolingreminder::lastCleanRun(store.readAll()) : 0;
    const int days = st.value(QStringLiteral("cooling/remind_days"), 120).toInt();
    if (!coolingreminder::reminderDue(lastClean, st.value(QStringLiteral("cooling/last_reminder"), 0).toLongLong(), now, days)) return;
    QSettings(coolingreminder::settingsPath(), QSettings::IniFormat).setValue(QStringLiteral("cooling/last_reminder"), now);
    gatedNotify(notifyprefs::COOLING, QString(), tr("Cooling test overdue"),
                tr("The last clean cooling test was %1 days ago. Health → Cooling → Run cooling test.").arg((now - lastClean) / 86400));
}

void Tray::toggleWindow() {
    if (win_->isVisible() && !win_->isMinimized()) { win_->hide(); return; }
    win_->showNormal();
    win_->raise();
    win_->activateWindow();
}

bool Tray::claimCooldown() {
    if (cooldown_) { notify(QStringLiteral("Centurion"), tr("Still applying the previous change — try again in a moment.")); return false; }
    cooldown_ = true;
    QTimer::singleShot(APPLY_COOLDOWN_MS, this, [this] { cooldown_ = false; });
    return true;
}

void Tray::notify(const QString &t, const QString &m, const QList<NotifyAction> &actions) {
    // The title is the topic: a newer message with the same title replaces the older one.
    notifier_->notify(t, t, m, actions, 2500, [this, t, m] { if (supportsMessages()) showMessage(t, m, icon(), 2500); });
}

void Tray::gatedNotify(const QString &category, const QString &severity, const QString &t, const QString &m, const QList<NotifyAction> &actions) {
    switch (notifyprefs::decide(notifyprefs::load(), category, severity, scenes::gameSessions() > 0)) {
    case notifyprefs::Verdict::Show: notify(t, m, actions); break;
    case notifyprefs::Verdict::Hide: break;
    case notifyprefs::Verdict::Defer: deferred_ << t + QStringLiteral(" — ") + QString(m).replace(QLatin1Char('\n'), QLatin1Char(' ')); break;
    }
}

void Tray::flushDeferred() {
    if (deferred_.isEmpty() || scenes::gameSessions() > 0) return;
    constexpr int SHOWN = 5;
    QStringList lines = deferred_.mid(0, SHOWN);
    if (deferred_.size() > SHOWN) lines << QStringLiteral("…");
    notify(tr("While gaming: %1 alert(s)").arg(deferred_.size()), lines.join(QLatin1Char('\n')) + tr("\nHealth → Notifications… to change this."));
    deferred_.clear();
}

static void disabledEntry(QMenu *m, const QString &t) { m->addAction(t)->setEnabled(false); }

void Tray::rebuild() {
    menu_->clear();

    // Scenes: the whole machine in one click; the component menus below stay
    // for one-off changes.
    SceneEngine *eng = win_->scenes();
    const QStringList sceneNames = scenes::names();
    if (!sceneNames.isEmpty()) {
        QMenu *sm = menu_->addMenu(tr("Scene"));
        for (const QString &n : sceneNames) {
            QAction *a = sm->addAction(n);
            a->setCheckable(true);
            a->setChecked(n == eng->active());
            connect(a, &QAction::triggered, this, [this, eng, n] {
                if (!claimCooldown()) return;
                eng->apply(n);
                notify(tr("Scene"), tr("Applying '%1'…").arg(n));
            });
        }
        sm->addSeparator();
        QAction *au = sm->addAction(tr("Switch with power source"));
        au->setCheckable(true);
        const scenes::Auto cfg = eng->autoConfig();
        au->setChecked(cfg.enabled);
        au->setEnabled(!cfg.onAc.isEmpty() || !cfg.onBattery.isEmpty());
        if (!au->isEnabled()) au->setToolTip(tr("Choose the AC / battery scenes in the Scenes tab first"));
        connect(au, &QAction::toggled, this, [eng](bool on) {
            scenes::Auto c = eng->autoConfig();
            c.enabled = on;
            eng->setAuto(c);
        });
        QAction *pa = sm->addAction(tr("Pause scenes"));
        pa->setCheckable(true);
        pa->setChecked(eng->paused());
        pa->setToolTip(tr("No automatic scene changes (power source, login, game) until unchecked"));
        connect(pa, &QAction::toggled, this, [eng](bool on) { eng->setPaused(on); });
        sm->setEnabled(!eng->busy());
        menu_->addSeparator();
    }

    // Power profile
    QMenu *pm = menu_->addMenu(tr("Power Profile"));
    HomeTab *home = win_->home();
    const QStringList profiles = home->offeredProfiles();
    if (profiles.isEmpty()) disabledEntry(pm, tr("(no platform-profile driver)"));
    const auto current = home->currentProfile();
    for (const QString &name : profiles) {
        QAction *a = pm->addAction(HomeTab::profileLabel(name));
        a->setCheckable(true);
        a->setChecked(current == name);
        connect(a, &QAction::triggered, this, [this, home, name] {
            if (!claimCooldown()) return;
            home->applyProfile(name);  // async; Home emits profileChanged → Firmware tab relocks
        });
    }

    // Ryzen (AMD only — the window has no Ryzen tab on Intel)
    if (RyzenTab *ryzen = win_->ryzen()) {
    QMenu *rm = menu_->addMenu(tr("CPU Curve (Ryzen)"));
    const QStringList rnames = ryzen->savedProfileNames();
    if (rnames.isEmpty()) disabledEntry(rm, tr("(no saved profiles)"));
    for (const QString &n : rnames)
        connect(rm->addAction(n), &QAction::triggered, this, [this, n] {
            // Straight from the profile file, like a scene: loading it into the tab first replaced
            // unsaved values in the editor and could open a modal box behind the hidden window.
            if (!claimCooldown()) return;
            win_->scenes()->applyCpuProfile(n, false, [this, n](bool ok, const QString &m) {
                notify(tr("CPU curve"), ok ? tr("Profile '%1' applied.").arg(n) : tr("Profile '%1': %2").arg(n, m));
            });
        });
    rm->addSeparator();
    connect(rm->addAction(tr("Reset curve")), &QAction::triggered, this, [this, ryzen] {
        if (!claimCooldown()) return;
        ryzen->applyReset();
        notify(tr("CPU curve"), tr("Resetting Curve Optimizer (coall=0)…"));
    });
    }

    // Intel undervolt (Intel only)
    if (IntelTab *intel = win_->intel()) {
        QMenu *im = menu_->addMenu(tr("CPU Undervolt (Intel)"));
        const QStringList inames = intel->savedProfileNames();
        if (inames.isEmpty()) disabledEntry(im, tr("(no saved profiles)"));
        for (const QString &n : inames)
            connect(im->addAction(n), &QAction::triggered, this, [this, n] {
                if (!claimCooldown()) return;
                win_->scenes()->applyCpuProfile(n, false, [this, n](bool ok, const QString &m) {
                    notify(tr("CPU undervolt"), ok ? tr("Profile '%1' applied.").arg(n) : tr("Profile '%1': %2").arg(n, m));
                });
            });
        im->addSeparator();
        connect(im->addAction(tr("Reset voltages")), &QAction::triggered, this, [this, intel] {
            if (!claimCooldown()) return;
            intel->applyReset();
            notify(tr("CPU undervolt"), tr("Resetting voltage offsets to 0 mV…"));
        });
    }

    // NVIDIA
    if (NvidiaTab *nv = win_->nvidia()) {  // no NVIDIA GPU → no menu
    QMenu *nm = menu_->addMenu(tr("GPU Curve (NVIDIA)"));
    const QStringList nnames = nv->profileNames();
    const QString def = nv->defaultProfileName();
    if (nnames.isEmpty()) disabledEntry(nm, tr("(no saved profiles)"));
    for (const QString &n : nnames)
        connect(nm->addAction(n == def ? n + "  ★" : n), &QAction::triggered, this, [this, nv, n] {
            if (!claimCooldown()) return;
            nv->applyNamedProfile(n);
            notify(tr("GPU curve"), tr("Applying profile '%1'…").arg(n));
        });
    nm->addSeparator();
    connect(nm->addAction(tr("Reset curve")), &QAction::triggered, this, [this, nv] {
        if (claimCooldown()) nv->resetCurve();
    });
    nm->setEnabled(!nv->busy());  // greyed while an NVIDIA helper call is in flight
    }

    // Optimizations
    QMenu *om = menu_->addMenu(tr("Optimizations"));
    OptimizeTab *opt = win_->optimize();
    const QString game = opt->gamePreset();
    for (const QString &n : opt->presetNames())
        connect(om->addAction(n == game ? n + "  ★" : n), &QAction::triggered, this, [this, opt, n] {
            if (!claimCooldown()) return;
            if (opt->applyNamedPreset(n)) notify(tr("Optimizations"), tr("Applying preset '%1'…").arg(n));
            else notify(tr("Optimizations"), tr("Preset '%1' has nothing applicable here.").arg(n));
        });
    om->addSeparator();
    QAction *restore = om->addAction(tr("Restore originals"));
    restore->setEnabled(opt->tuningActive());
    connect(restore, &QAction::triggered, this, [this, opt] {
        if (!claimCooldown()) return;
        opt->restoreAll(false);
        notify(tr("Optimizations"), tr("Restoring original values…"));
    });
    om->setEnabled(!opt->busy());

    // Keyboard lighting (Spectrum controller) — immediate, no cooldown needed.
    if (LightingTab *lt = win_->lighting()) {
        QMenu *lm = menu_->addMenu(tr("Keyboard Lighting"));
        if (!lt->ready()) {
            disabledEntry(lm, tr("(keyboard not read — see the Lighting tab)"));
        } else {
            for (int p = lighting::MIN_PROFILE; p <= lighting::MAX_PROFILE; ++p) {
                QAction *a = lm->addAction(tr("Profile %1").arg(p));
                a->setCheckable(true);
                a->setChecked(p == lt->activeProfile());
                connect(a, &QAction::triggered, this, [this, lt, p] {
                    if (lt->hasUnappliedChanges()) { notify(tr("Keyboard lighting"), tr("The Lighting tab has changes not applied yet — apply or revert them first.")); return; }
                    lt->activateProfile(p);
                });
            }
            lm->addSeparator();
            QAction *off = lm->addAction(tr("Lights off"));
            off->setCheckable(true);
            off->setChecked(lt->brightness() == 0);
            connect(off, &QAction::triggered, this, [lt](bool checked) { lt->setLightsOn(!checked); });
            QMenu *bm = lm->addMenu(tr("Brightness"));
            for (int b = 1; b <= lighting::MAX_BRIGHTNESS; ++b) {
                QAction *a = bm->addAction(QString::number(b));
                a->setCheckable(true);
                a->setChecked(b == lt->brightness());
                connect(a, &QAction::triggered, this, [lt, b] { lt->setBrightness(b); });
            }
        }
        lm->setEnabled(!lt->busy());
    }

    // Fans: all to max / Auto, or one fan at a fixed RPM (multiples of 100).
    const QList<HomeTab::FanInfo> fans = home->fanInfo();
    if (!fans.isEmpty()) {
        QMenu *fm = menu_->addMenu(tr("Fans"));
        const bool maxMode = home->fansMaxMode();
        QAction *mx = fm->addAction(tr("Max all fans"));
        mx->setCheckable(true);
        mx->setChecked(maxMode);
        connect(mx, &QAction::triggered, this, [this, home, maxMode] {
            if (!claimCooldown()) return;
            if (maxMode) { home->setAllFansAuto(); notify(tr("Fans"), tr("All fans back to Auto…")); }
            else { home->setAllFansMax(); notify(tr("Fans"), tr("All fans to maximum…")); }
        });
        connect(fm->addAction(tr("All fans to Auto")), &QAction::triggered, this, [this, home] {
            if (!claimCooldown()) return;
            home->setAllFansAuto();
            notify(tr("Fans"), tr("All fans back to Auto…"));
        });
        fm->addSeparator();
        for (const HomeTab::FanInfo &f : fans) {
            const QString cur = f.target > 0 ? tr("%1 RPM").arg(f.target) : tr("Auto");
            QMenu *sub = fm->addMenu(f.name + "  (" + cur + ")");
            auto add = [this, home, sub, f](const QString &text, int rpm) {
                QAction *a = sub->addAction(text);
                a->setCheckable(true);
                a->setChecked(f.target == rpm);
                const QString key = f.key, name = f.name;
                connect(a, &QAction::triggered, this, [this, home, key, name, rpm] {
                    if (!claimCooldown()) return;
                    home->setFanTarget(key, rpm);
                    notify(tr("Fans"), rpm > 0 ? tr("%1 → %2 RPM").arg(name).arg(rpm) : tr("%1 → Auto").arg(name));
                });
            };
            add(tr("Auto"), 0);
            sub->addSeparator();
            const int top = f.max > 0 && f.max < 9999 ? f.max : 6000;
            const int start = std::max(100, (f.min + 99) / 100 * 100);
            for (int r = start; r <= top; r += 100) add(tr("%1 RPM").arg(r), r);
            if (top % 100) add(tr("%1 RPM (max)").arg(top), top);
        }
        fm->setEnabled(!home->fanBusy());
    }

    // Health: the worst Overview level, one click away. Shown only once the Overview has a reading.
    if (const int worst = win_->health()->worstLevel(); worst >= 0) {
        const char *color = worst >= 2 ? theme::DANGER : worst == 1 ? theme::WARN : theme::OK;
        const QString text = worst >= 2 ? tr("Health: problem found") : worst == 1 ? tr("Health: needs a look") : tr("Health: all clear");
        QPixmap dot(16, 16);
        dot.fill(Qt::transparent);
        QPainter p(&dot);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(QLatin1String(color)));
        p.drawEllipse(QRectF(3, 3, 10, 10));
        p.end();
        connect(menu_->addAction(QIcon(dot), text), &QAction::triggered, this, [this] {
            win_->showNormal();
            win_->raise();
            win_->activateWindow();
            win_->showTab(QStringLiteral("Health"));
        });
    }

    menu_->addSeparator();
    connect(menu_->addAction(win_->isVisible() ? tr("Hide window") : tr("Show window")), &QAction::triggered, this, &Tray::toggleWindow);
    connect(menu_->addAction(tr("Quit")), &QAction::triggered, this, [this] {
        win_->setForceQuit(true);
        hide();
        qApp->quit();
    });
}
