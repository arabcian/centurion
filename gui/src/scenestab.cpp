#include "scenestab.h"
#include "privileged.h"
#include "lighting.h"
#include "mangohud.h"
#include "bundle.h"
#include "fwattrtab.h"
#include "hometab.h"
#include "inteltab.h"
#include "mainwindow.h"
#include "nvidiatab.h"
#include "optimizetab.h"
#include "ryzentab.h"
#include "shortcuts.h"
#include "theme.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QCheckBox>
#include <QColor>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <tuple>
#include <QDate>
#include <QDir>
#include <QFileDialog>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHash>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QShowEvent>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

using scenes::Choice;
using scenes::Scene;

// Combo item data: "u" unchanged, "r" reset, "p:<name>" profile.
static QString encode(const Choice &c) {
    return c.kind == Choice::Reset ? QStringLiteral("r") : c.kind == Choice::Profile ? "p:" + c.name : QStringLiteral("u");
}
static Choice decode(const QString &d) {
    if (d == QLatin1String("r")) return {Choice::Reset, {}};
    if (d.startsWith(QLatin1String("p:"))) return {Choice::Profile, d.mid(2)};
    return {};
}

/// Fills a component combo, keeping `keep` selectable even when its profile
/// has since been deleted — shown as missing instead of silently dropped.
static void fillChoice(QComboBox *c, const QStringList &names, const QString &resetLabel, const Choice &keep) {
    c->clear();
    c->addItem(QStringLiteral("Unchanged"), QStringLiteral("u"));
    if (!resetLabel.isEmpty()) c->addItem(resetLabel, QStringLiteral("r"));
    for (const QString &n : names) c->addItem(n, "p:" + n);
    if (keep.kind == Choice::Profile && !names.contains(keep.name)) c->addItem(keep.name + QStringLiteral("  (missing)"), "p:" + keep.name);
    c->setCurrentIndex(std::max(0, c->findData(encode(keep))));
}

static QLabel *muted(const QString &t) {
    auto *l = new QLabel(t);
    l->setProperty("role", "muted");
    return l;
}

ScenesTab::ScenesTab(MainWindow *win) : win_(win), eng_(win->scenes()) {
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(10);

    // ── left: scene list ──
    auto *left = new QVBoxLayout;
    left->setSpacing(6);
    auto *listBox = new QGroupBox("Scenes");
    auto *ll = new QVBoxLayout(listBox);
    list_ = new QListWidget;
    list_->setFrameShape(QFrame::NoFrame);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);  // long names elide instead
    list_->setTextElideMode(Qt::ElideRight);
    list_->setStyleSheet(QStringLiteral("QListWidget { background: transparent; border: none; padding: 0; outline: none; }"
                                        "QListWidget::item { padding: 4px 6px; border-radius: 5px; }"
                                        "QListWidget::item:selected { background: %1; }"
                                        "QListWidget::item:hover:!selected { background: %2; }")
                             .arg(theme::BG3, theme::BG2));
    ll->addWidget(list_, 1);
    auto *add = new QPushButton("New scene…");
    ll->addWidget(add);
    auto *io = new QHBoxLayout;
    io->setSpacing(4);
    auto *exp = new QPushButton("Export…");
    auto *imp = new QPushButton("Import…");
    exp->setToolTip("Save scenes, Optimizations presets, game-launch settings and every CPU / GPU curve profile to one file");
    imp->setToolTip("Load a file made with Export… (backup, reinstall, another machine of the same model)");
    for (QPushButton *b : {exp, imp}) { b->setObjectName("btnMini"); io->addWidget(b); }
    ll->addLayout(io);
    connect(exp, &QPushButton::clicked, this, [this] {
        const QString def = QDir::homePath() + "/centurion-" + QDate::currentDate().toString(Qt::ISODate) + ".centurion.json";
        const QString path = QFileDialog::getSaveFileName(this, "Export settings", def, "Centurion bundle (*.centurion.json *.json)");
        if (path.isEmpty()) return;
        QString err;
        const QString r = bundle::exportTo(path, &err);
        setStatus(r.isEmpty() ? "Export failed: " + err : r + "  →  " + path, r.isEmpty() ? theme::DANGER : theme::OK);
    });
    connect(imp, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, "Import settings", QDir::homePath(), "Centurion bundle (*.centurion.json *.json)");
        if (path.isEmpty()) return;
        bundle::importFrom(path, this, [this](const QString &r) {
            if (r.isEmpty()) return;  // cancelled
            setStatus(r, r.startsWith("Import failed") ? theme::DANGER : theme::OK);
            loaded_ = {};
            reloadList();
            reloadAuto();
            win_->shortcuts()->sync();
        });
    });
    dup_ = new QPushButton("Copy…");
    dup_->setToolTip("Duplicate this scene under a new name");
    del_ = new QPushButton("Delete");
    del_->setObjectName("btnDanger");
    left->addWidget(listBox, 1);
    auto *leftHost = new QWidget;
    leftHost->setLayout(left);
    leftHost->setFixedWidth(190);
    root->addWidget(leftHost);

    // ── right: editor + automatic switching ──
    auto *right = new QVBoxLayout;
    right->setSpacing(6);

    editor_ = new QGroupBox("Scene");
    auto *g = new QGridLayout(editor_);
    g->setHorizontalSpacing(10);
    g->setVerticalSpacing(6);
    g->setColumnStretch(1, 1);
    int row = 0;
    auto addRow = [&](const QString &label, QWidget *w, const QString &tip) {
        auto *l = muted(label);
        l->setToolTip(tip);
        w->setToolTip(tip);
        g->addWidget(l, row, 0);
        g->addWidget(w, row++, 1, 1, 3);
    };
    profile_ = new QComboBox;
    addRow("Power profile", profile_, "Platform profile set first. Firmware limits below only take effect in Custom.");

    fwLabel_ = new QLabel;
    // A rich-text table: no word wrap (QLabel under-measures a wrapped table and cut its last row off).
    fwLabel_->setWordWrap(false);
    fwLabel_->setTextFormat(Qt::RichText);
    fwCapture_ = new QPushButton("Capture current");
    fwCapture_->setObjectName("btnMini");
    fwCapture_->setToolTip("Store the firmware attributes as they are right now (set them in Firmware Attributes first).");
    fwClear_ = new QPushButton("Clear");
    fwClear_->setObjectName("btnMini");
    // One row per captured value: label and buttons stay at the top of the list.
    g->addWidget(muted("Firmware limits"), row, 0, Qt::AlignTop);
    g->addWidget(fwLabel_, row, 1, Qt::AlignTop);
    g->addWidget(fwCapture_, row, 2, Qt::AlignTop);
    g->addWidget(fwClear_, row++, 3, Qt::AlignTop);
    fan_ = new QComboBox;
    fan_->addItem("Unchanged", -1);
    fan_->addItem("Auto (EC fan curve)", 0);
    fan_->addItem("Turbo (EC full speed)", 1);
    fanTableLabel_ = new QLabel;
    fanTableLabel_->setWordWrap(true);
    fanClear_ = new QPushButton("Clear");
    fanClear_->setObjectName("btnMini");
    fanTableLabel_->setToolTip("Custom-mode EC fan curve (10 temperature steps, level 1-10). Stored by Capture current above; "
                               "applied only while the scene uses the Custom power profile.");
    g->addWidget(muted("Fan curve"), row, 0);
    g->addWidget(fanTableLabel_, row, 1, 1, 2);
    g->addWidget(fanClear_, row++, 3);
    addRow("Fan boost", fan_, "EC Full Speed flag (the Turbo fan switch). Applied after the power profile; Capture current reads it too.");

    if (win_->ryzen() || win_->intel()) {
        cpu_ = new QComboBox;
        addRow(win_->ryzen() ? "CPU curve (Ryzen)" : "CPU undervolt (Intel)", cpu_,
               "A profile saved in the CPU tab. Reset sets every offset back to 0.");
    }
    // Always laid out, shown only while there is an NVIDIA tab: the dGPU can
    // come back after start-up ("Bring NVIDIA back"), and the row with it.
    gpu_ = new QComboBox(this);
    addRow("GPU curve (NVIDIA)", gpu_, "A profile saved in the NVIDIA tab. Reset clears the curve offsets.");
    gpuLabel_ = qobject_cast<QLabel *>(g->itemAtPosition(row - 1, 0)->widget());
    const auto showGpuRow = [this] {
        const bool on = win_->nvidia() != nullptr;
        gpu_->setVisible(on);
        if (gpuLabel_) gpuLabel_->setVisible(on);
    };
    showGpuRow();
    connect(win_, &MainWindow::nvidiaTabAdded, this, [this, showGpuRow] { showGpuRow(); fillOptions(); });
    tuning_ = new QComboBox;
    addRow("Optimizations", tuning_,
           "An Optimizations preset. Switching presets restores every knob the new one does not set, "
           "so nothing from the previous scene lingers. A running game's tuning is never touched.");
    if (win_->lighting()) {
        lightProfile_ = new QComboBox;
        lightProfile_->addItem("Unchanged", -1);
        for (int p = lighting::MIN_PROFILE; p <= lighting::MAX_PROFILE; ++p) lightProfile_->addItem(QStringLiteral("Profile %1").arg(p), p);
        lightBright_ = new QComboBox;
        lightBright_->addItem("Unchanged", -1);
        lightBright_->addItem("Off", 0);
        for (int b = 1; b <= lighting::MAX_BRIGHTNESS; ++b) lightBright_->addItem(QStringLiteral("Brightness %1").arg(b), b);
        auto *lh = new QWidget;
        auto *ll = new QHBoxLayout(lh);
        ll->setContentsMargins(0, 0, 0, 0);
        ll->addWidget(lightProfile_, 1);
        ll->addWidget(lightBright_, 1);
        addRow("Keyboard lighting", lh, "A lighting profile of the keyboard controller (edit them in the Lighting tab) "
                                        "and its brightness. Off turns the backlight off.");
    }
    if (lighting::zone4Present()) {
        zone4Label_ = new QLabel;
        zone4Label_->setWordWrap(true);
        zone4Capture_ = new QPushButton("Capture");
        zone4Capture_->setObjectName("btnMini");
        zone4Capture_->setToolTip("Store the 4-zone settings the Lighting tab last applied (effect, colours, speed, brightness).");
        zone4Clear_ = new QPushButton("Clear");
        zone4Clear_->setObjectName("btnMini");
        g->addWidget(muted("4-zone lighting"), row, 0);
        g->addWidget(zone4Label_, row, 1);
        g->addWidget(zone4Capture_, row, 2);
        g->addWidget(zone4Clear_, row++, 3);
        connect(zone4Capture_, &QPushButton::clicked, this, [this] {
            const QString ini = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/centurion/zone4.ini");
            if (!QFileInfo::exists(ini)) { setStatus("Apply a 4-zone setting in the Lighting tab first.", theme::WARN); return; }
            QSettings st(ini, QSettings::IniFormat);
            QJsonArray cols;
            for (int i = 0; i < 4; ++i) cols.append(QColor(st.value(QStringLiteral("zone4/color%1").arg(i), QStringLiteral("#ffffff")).toString()).name().mid(1));
            zone4_ = QJsonObject{{"effect", st.value("zone4/effect", "static").toString()},
                                 {"brightness", st.value("zone4/brightness", 2).toInt()},
                                 {"speed", st.value("zone4/speed", 2).toInt()},
                                 {"right_to_left", st.value("zone4/right_to_left", false).toBool()},
                                 {"colors", cols}};
            updateZone4Label();
            updateDirty();
        });
        connect(zone4Clear_, &QPushButton::clicked, this, [this] { zone4_ = {}; updateZone4Label(); updateDirty(); });
    }
    if (const int max = HomeTab::kbdBacklightMax(); max > 0) {
        kbdLevel_ = new QComboBox;
        kbdLevel_->addItem("Unchanged", -1);
        kbdLevel_->addItem("Off", 0);
        for (int i = 1; i <= max; ++i) kbdLevel_->addItem(max == 1 ? QStringLiteral("On") : QStringLiteral("Level %1").arg(i), i);
        addRow("Keyboard light", kbdLevel_, "White keyboard backlight level (the same setting Fn+Space cycles).");
    }
    // Always laid out; shown when the firmware reported Over Drive (Home probes it) or the scene already sets it.
    panelOd_ = new QComboBox;
    panelOd_->addItem("Unchanged", -1);
    panelOd_->addItem("On", 1);
    panelOd_->addItem("Off", 0);
    addRow("Panel Over Drive", panelOd_, "Faster pixel response of the internal panel (less ghosting, a little overshoot). "
                                         "Offered where the firmware reports it.");
    panelOdLabel_ = g->itemAtPosition(row - 1, 0)->widget();
    refresh_ = new QComboBox;
    addRow("Refresh rate", refresh_, "Internal panel refresh rate at its current resolution, set as you through the desktop's own tool "
                                     "(KDE kscreen-doctor, wlr-randr or X11 xrandr). A lower rate saves noticeable power on battery.");
    refreshLabel_ = g->itemAtPosition(row - 1, 0)->widget();
    command_ = new QLineEdit;
    command_->setPlaceholderText("optional, e.g.  pactl set-card-profile …");
    addRow("Run command", command_, "Runs as you, without a shell, after everything else except the screen brightness — for things "
                                    "this app does not manage itself (audio profile, a script…).");
    // Screen brightness: the very last step, after the command. Shown where there is a backlight (or the scene sets one).
    screenBright_ = new QSpinBox;
    screenBright_->setRange(0, 100);
    screenBright_->setSingleStep(5);
    screenBright_->setSuffix(QStringLiteral(" %"));
    screenBright_->setSpecialValueText(QStringLiteral("Unchanged"));
    auto *sbCapture = new QPushButton("Capture");
    sbCapture->setObjectName("btnMini");
    sbCapture->setToolTip("Store the screen brightness as it is right now.");
    auto *sbClear = new QPushButton("Clear");
    sbClear->setObjectName("btnMini");
    screenBrightRow_ = new QWidget;
    auto *sbl = new QHBoxLayout(screenBrightRow_);
    sbl->setContentsMargins(0, 0, 0, 0);
    sbl->addWidget(screenBright_, 1);
    sbl->addWidget(sbCapture);
    sbl->addWidget(sbClear);
    addRow("Screen brightness", screenBrightRow_, "Internal panel backlight, set as you after the run command has been started "
                                                  "(the command may change display mode and reset it). Unchanged leaves it alone.");
    screenBright_->setToolTip(screenBrightRow_->toolTip());
    screenBrightLabel_ = g->itemAtPosition(row - 1, 0)->widget();
    connect(sbCapture, &QPushButton::clicked, this, &ScenesTab::captureScreenBrightness);
    connect(sbClear, &QPushButton::clicked, this, [this] { screenBright_->setValue(0); });

    // Global shortcut: not a step — the desktop's GlobalShortcuts portal applies the scene when it is pressed.
    shortcut_ = new QKeySequenceEdit;
    auto *scClear = new QPushButton("Clear");
    scClear->setObjectName("btnMini");
    shortcutNote_ = muted(QString());
    shortcutNote_->setWordWrap(true);
    auto *scRow = new QWidget;
    auto *scv = new QVBoxLayout(scRow);
    scv->setContentsMargins(0, 0, 0, 0);
    scv->setSpacing(2);
    // The desktop's own dialog for our shortcuts (portal v2): change keys where the desktop keeps them.
    shortcutConfigure_ = new QPushButton("Change on desktop…");
    shortcutConfigure_->setObjectName("btnMini");
    shortcutConfigure_->setToolTip("Opens the desktop's shortcut dialog for Centurion's scene shortcuts.");
    shortcutConfigure_->setVisible(false);
    connect(shortcutConfigure_, &QPushButton::clicked, win_->shortcuts(), &GlobalShortcuts::configure);
    auto *scl = new QHBoxLayout;
    scl->addWidget(shortcut_, 1);
    scl->addWidget(scClear);
    scl->addWidget(shortcutConfigure_);
    scv->addLayout(scl);
    scv->addWidget(shortcutNote_);
    addRow("Shortcut", scRow, "A key combination that applies this scene from anywhere. Click the field and press the keys, then Save. "
                              "It is registered with the desktop (global-shortcuts portal), which may ask you to confirm it and "
                              "can change it later in its own shortcut settings.");
    connect(scClear, &QPushButton::clicked, shortcut_, &QKeySequenceEdit::clear);
    // One combination only: the field would otherwise keep recording up to four.
    connect(shortcut_, &QKeySequenceEdit::editingFinished, this, [this] {
        if (const QKeySequence k = shortcut_->keySequence(); k.count() > 1) shortcut_->setKeySequence(QKeySequence(k[0]));
    });
    connect(shortcut_, &QKeySequenceEdit::keySequenceChanged, this, [this] { if (!filling_) updateDirty(); });
    connect(win_->shortcuts(), &GlobalShortcuts::changed, this, &ScenesTab::updateShortcutNote);

    auto *eb = new QHBoxLayout;
    eb->addWidget(dup_);
    eb->addWidget(del_);
    eb->addStretch(1);
    save_ = new QPushButton("Save");
    apply_ = new QPushButton("Apply");
    apply_->setObjectName("btnAccent");
    apply_->setToolTip("Saves the scene if changed, then applies it");
    eb->addWidget(save_);
    eb->addWidget(apply_);
    g->addLayout(eb, row++, 0, 1, 4);
    right->addWidget(editor_);

    empty_ = muted("No scenes yet. A scene bundles a power profile, firmware limits, CPU and GPU curves and an "
                   "Optimizations preset (and keyboard lighting) under one name — create one with New…");
    empty_->setWordWrap(true);
    right->addWidget(empty_);

    auto *autoBox = new QGroupBox("Automatic switching");
    auto *ag = new QGridLayout(autoBox);
    ag->setHorizontalSpacing(10);
    ag->setVerticalSpacing(6);
    ag->setColumnStretch(1, 1);
    ag->setColumnStretch(3, 1);
    auto_ = new QCheckBox("Switch scenes with the power source");
    auto_->setToolTip("Applies the chosen scene when the charger is plugged in or pulled (and at login). "
                      "A charger that stays connected under heavy load counts as AC even if the battery dips.");
    onAc_ = new QComboBox;
    onBattery_ = new QComboBox;
    ag->addWidget(auto_, 0, 0, 1, 3);
    ag->addWidget(muted("On AC"), 1, 0);
    ag->addWidget(onAc_, 1, 1);
    ag->addWidget(muted("On battery"), 1, 2);
    ag->addWidget(onBattery_, 1, 3);
    power_ = muted(QString());
    ag->addWidget(power_, 2, 0, 1, 4);
    resume_ = new QCheckBox("Reapply the scene after waking from suspend");
    resume_->setToolTip("Firmware and the EC can reset limits during sleep. After a wake the scene for the current power source is "
                        "applied again (5 s after waking, once more 30 s later). Needs automatic switching; honours Pause and running games.");
    ag->addWidget(resume_, 3, 0, 1, 4);
    pause_ = new QPushButton;
    pause_->setCheckable(true);
    pause_->setToolTip("Pause every automatic scene change (power source, login, game scene). Apply still works.");
    ag->addWidget(pause_, 0, 3, Qt::AlignRight);
    auto syncPause = [this](bool on) {
        const QSignalBlocker b(pause_);
        pause_->setChecked(on);
        pause_->setText(on ? QStringLiteral("▶ Resume scenes") : QStringLiteral("⏸ Pause scenes"));
    };
    syncPause(eng_->paused());
    connect(eng_, &SceneEngine::pausedChanged, this, syncPause);
    connect(pause_, &QPushButton::toggled, this, [this, syncPause](bool on) {
        QString err;
        if (!eng_->setPaused(on, &err)) { syncPause(!on); setStatus("Could not save the pause setting: " + err, theme::DANGER); }
    });
    right->addWidget(autoBox);

    if (mangohud::available()) {
        mangohud_ = new QCheckBox("Show the active scene and power profile in games (MangoHud)");
        mangohud_->setToolTip("Adds a marked block to " + mangohud::configPath() + ": a \"Centurion\" line read from a small file "
                              "Centurion keeps current (empty while Centurion is not running). MangoHud runs it only with legacy_layout=0, which draws just the listed "
                              "elements, so the block also lists MangoHud's default ones when your file does not. "
                              "Unticking removes the block and leaves the file as it was.");
        mangohud_->setChecked(mangohud::enabled());
        connect(mangohud_, &QCheckBox::toggled, this, [this](bool on) {
            QString err;
            if (!mangohud::setEnabled(on, &err)) {
                const QSignalBlocker b(mangohud_);
                mangohud_->setChecked(!on);
                setStatus("Could not change the MangoHud config: " + err, theme::DANGER);
                return;
            }
            win_->updateOverlay();
            setStatus(on ? QStringLiteral("MangoHud shows the scene from the next game start.") : QStringLiteral("MangoHud line removed."), theme::OK);
        });
        right->addWidget(mangohud_);
    }

    status_ = new QLabel;
    status_->setWordWrap(true);
    right->addWidget(status_);
    right->addStretch(1);
    root->addLayout(right, 1);

    // ── wiring ──
    connect(list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *it) {
        if (!filling_ && it) select(it->data(Qt::UserRole).toString());
        colorList();
    });
    connect(add, &QPushButton::clicked, this, [this] { newScene(false); });
    connect(dup_, &QPushButton::clicked, this, [this] { newScene(true); });
    connect(del_, &QPushButton::clicked, this, &ScenesTab::deleteScene);
    connect(save_, &QPushButton::clicked, this, [this] { if (saveCurrent()) setStatus("Saved.", theme::OK); });
    connect(apply_, &QPushButton::clicked, this, &ScenesTab::applyCurrent);
    connect(fwCapture_, &QPushButton::clicked, this, &ScenesTab::captureFirmware);
    connect(fanClear_, &QPushButton::clicked, this, [this] { fanTable_.clear(); updateFanTableLabel(); updateDirty(); });
    connect(fwClear_, &QPushButton::clicked, this, [this] { firmware_.clear(); updateFirmwareLabel(); updateDirty(); });
    for (QComboBox *c : {profile_, cpu_, gpu_, tuning_, lightProfile_, lightBright_, fan_, panelOd_, kbdLevel_, refresh_})
        if (c) connect(c, &QComboBox::currentIndexChanged, this, [this] { if (!filling_) updateDirty(); });
    connect(command_, &QLineEdit::textChanged, this, [this] { if (!filling_) updateDirty(); });
    connect(screenBright_, &QSpinBox::valueChanged, this, [this] { if (!filling_) updateDirty(); });
    connect(auto_, &QCheckBox::toggled, this, [this](bool on) { resume_->setEnabled(on); if (!filling_) storeAuto(); });
    connect(resume_, &QCheckBox::toggled, this, [this] { if (!filling_) storeAuto(); });
    for (QComboBox *c : {onAc_, onBattery_})
        connect(c, &QComboBox::currentIndexChanged, this, [this] { if (!filling_) storeAuto(); });

    connect(eng_, &SceneEngine::started, this, [this](const QString &n) {
        apply_->setEnabled(false);
        setStatus("Applying '" + n + "'…");
    });
    connect(eng_, &SceneEngine::finished, this, [this](const QString &n, bool ok, const QStringList &log) {
        if (n.isEmpty()) { setStatus(log.join(' '), theme::FG_DIM); return; }  // informational (deferred switch)
        apply_->setEnabled(list_->count() > 0);
        setStatus((ok ? "'" + n + "' applied — " : "'" + n + "' applied with problems — ") + log.join(QStringLiteral("  ·  ")),
                  ok ? theme::OK : theme::WARN);
        reloadList(loaded_.name);  // marks the active scene
        updatePowerLabel();
    });
    connect(eng_, &SceneEngine::powerSourceChanged, this, &ScenesTab::updatePowerLabel);
    connect(eng_, &SceneEngine::activeSceneChanged, this, [this](const QString &n, bool game) {
        reloadList(loaded_.name);  // marks the active scene
        updatePowerLabel();
        setStatus((game ? "Game scene '" : "Scene '") + n + "' is now active (switched by centurion-gamemode).", theme::FG_DIM);
    });

    fillOptions();
    reloadAuto();
    reloadList();
    updatePowerLabel();
}

void ScenesTab::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    // Profiles may have been saved or deleted in other tabs meanwhile.
    const Scene cur = fromEditor();
    const Scene saved = loaded_;
    // Scenes saved outside this tab (first-run wizard): the list follows, unsaved edits stay.
    QStringList shown;
    for (int i = 0; i < list_->count(); ++i) shown << list_->item(i)->data(Qt::UserRole).toString();
    const bool listChanged = shown != scenes::names();
    if (listChanged) reloadList();
    fillOptions();
    if (!listChanged || !saved.name.isEmpty()) {
        setEditor(cur);
        loaded_ = saved;
    }
    updateDirty();
    reloadAuto();
    updatePowerLabel();
    queryRates();      // once: the panel's refresh rates (as the user, through the desktop's tool)
    syncExtraRows();   // Over Drive support may have been probed by Home meanwhile
}

void ScenesTab::fillOptions() {
    filling_ = true;
    const QString keepProfile = profile_->currentData().toString();
    profile_->clear();
    profile_->addItem("Unchanged", QString());
    for (const QString &p : win_->home()->offeredProfiles()) profile_->addItem(HomeTab::profileLabel(p), p);
    profile_->setCurrentIndex(std::max(0, profile_->findData(keepProfile)));
    // Component combos are (re)filled by setEditor() with the scene's own choice kept.
    filling_ = false;
}

void ScenesTab::setEditor(const Scene &s) {
    filling_ = true;
    int pi = profile_->findData(s.platformProfile);
    if (pi < 0 && !s.platformProfile.isEmpty()) {
        profile_->addItem(HomeTab::profileLabel(s.platformProfile) + "  (not offered)", s.platformProfile);
        pi = profile_->count() - 1;
    }
    profile_->setCurrentIndex(std::max(0, pi));
    if (cpu_) {
        const QStringList names = win_->ryzen() ? win_->ryzen()->savedProfileNames() : win_->intel()->savedProfileNames();
        fillChoice(cpu_, names, win_->ryzen() ? "Reset (0 offset)" : "Reset (0 mV)", s.cpu);
    }
    fillChoice(gpu_, win_->nvidia() ? win_->nvidia()->profileNames() : QStringList(), "Reset curve", s.gpu);
    fillChoice(tuning_, win_->optimize()->presetNames(), "Restore originals", s.tuning);
    if (lightProfile_) {
        lightProfile_->setCurrentIndex(std::max(0, lightProfile_->findData(s.lightProfile)));
        lightBright_->setCurrentIndex(std::max(0, lightBright_->findData(s.lightBrightness)));
    }
    fan_->setCurrentIndex(std::max(0, fan_->findData(s.fanFullSpeed)));
    panelOd_->setCurrentIndex(std::max(0, panelOd_->findData(s.panelOd)));
    if (kbdLevel_) {
        if (kbdLevel_->findData(s.kbdLevel) < 0) kbdLevel_->addItem(QStringLiteral("Level %1  (not offered)").arg(s.kbdLevel), s.kbdLevel);
        kbdLevel_->setCurrentIndex(std::max(0, kbdLevel_->findData(s.kbdLevel)));
    }
    zone4_ = s.zone4;
    updateZone4Label();
    fillRefresh(s.refresh);
    command_->setText(s.command);
    screenBright_->setValue(s.screenBrightness > 0 ? s.screenBrightness : 0);
    shortcut_->setKeySequence(QKeySequence::fromString(s.shortcut, QKeySequence::PortableText));
    firmware_ = s.firmware;
    fanTable_ = s.fanTable;
    updateFanTableLabel();
    editor_->setTitle(s.name.isEmpty() ? QStringLiteral("Scene") : s.name);
    filling_ = false;
    loaded_ = s;
    syncExtraRows();
    updateFirmwareLabel();
    updateShortcutNote();
    updateDirty();
}

void ScenesTab::updateShortcutNote() {
    GlobalShortcuts *g = win_->shortcuts();
    QString t;
    if (!g->available())
        t = QStringLiteral("no global-shortcut portal on this desktop — bind  centurion-gamemode SCENE \"%1\"  in its shortcut settings").arg(loaded_.name);
    else if (const QString bound = g->trigger(loaded_.name); !bound.isEmpty())
        t = QStringLiteral("desktop: ") + bound;
    else if (!loaded_.shortcut.isEmpty())
        t = QStringLiteral("not registered yet");
    shortcutNote_->setText(t);
    shortcutNote_->setVisible(!t.isEmpty());
    shortcutConfigure_->setVisible(g->canConfigure());
}

Scene ScenesTab::fromEditor() const {
    Scene s;
    s.name = loaded_.name;
    s.platformProfile = profile_->currentData().toString();
    s.firmware = firmware_;
    s.fanTable = fanTable_;
    if (cpu_) s.cpu = decode(cpu_->currentData().toString());
    s.gpu = decode(gpu_->currentData().toString());
    s.tuning = decode(tuning_->currentData().toString());
    if (lightProfile_) {
        s.lightProfile = lightProfile_->currentData().toInt();
        s.lightBrightness = lightBright_->currentData().toInt();
    } else {  // no keyboard here: keep what the scene file says
        s.lightProfile = loaded_.lightProfile;
        s.lightBrightness = loaded_.lightBrightness;
    }
    s.fanFullSpeed = fan_->currentData().toInt();
    s.panelOd = panelOd_->currentData().toInt();
    s.kbdLevel = kbdLevel_ ? kbdLevel_->currentData().toInt() : loaded_.kbdLevel;
    s.zone4 = zone4Label_ ? zone4_ : loaded_.zone4;
    s.refresh = refresh_->currentData().toDouble();
    s.command = command_->text().trimmed();
    s.screenBrightness = screenBright_->value() > 0 ? screenBright_->value() : -1;
    if (const QKeySequence k = shortcut_->keySequence(); !k.isEmpty()) s.shortcut = QKeySequence(k[0]).toString(QKeySequence::PortableText);
    return s;
}

void ScenesTab::updateZone4Label() {
    if (!zone4Label_) return;
    zone4Clear_->setEnabled(!zone4_.isEmpty());
    if (zone4_.isEmpty()) { zone4Label_->setText(QStringLiteral("<span style='color:%1'>Unchanged</span>").arg(theme::MUTED)); return; }
    QString sw;
    for (const QJsonValue &c : zone4_.value("colors").toArray()) sw += QStringLiteral("<span style='color:#%1'>■</span>").arg(c.toString());
    zone4Label_->setText(zone4_.value("effect").toString() + QStringLiteral(" · brightness %1  ").arg(zone4_.value("brightness").toInt()) + sw);
}

/// Refresh combo: Unchanged + the rates the panel offers now; the scene's own value stays selectable when it is not offered.
void ScenesTab::fillRefresh(double keep) {
    const bool was = filling_;
    filling_ = true;
    refresh_->clear();
    refresh_->addItem("Unchanged", 0.0);
    for (double r : rates_) refresh_->addItem(QStringLiteral("%1 Hz").arg(r, 0, 'f', r == std::floor(r) ? 0 : 2), r);
    int i = 0;
    if (keep > 0) {
        for (int k = 1; k < refresh_->count(); ++k) if (std::abs(refresh_->itemData(k).toDouble() - keep) < 0.5) i = k;
        if (i == 0) { refresh_->addItem(QStringLiteral("%1 Hz  (not offered now)").arg(keep, 0, 'f', 0), keep); i = refresh_->count() - 1; }
    }
    refresh_->setCurrentIndex(i);
    filling_ = was;
    syncExtraRows();
}

void ScenesTab::queryRates() {
    if (ratesAsked_) return;
    ratesAsked_ = true;
    auto *p = new QProcess(this);
    connect(p, &QProcess::finished, this, [this, p](int, QProcess::ExitStatus) {
        const QJsonObject o = QJsonDocument::fromJson(p->readAllStandardOutput().trimmed()).object();
        p->deleteLater();
        if (!o.value("ok").toBool()) return;
        rates_.clear();
        for (const QJsonValue &v : o.value("rates").toArray()) rates_ << v.toDouble();
        const double keep = refresh_->currentData().toDouble();
        const bool dirty = fromEditor() != loaded_;
        fillRefresh(keep);
        if (!dirty) updateDirty();
    });
    connect(p, &QProcess::errorOccurred, p, [p](QProcess::ProcessError e) { if (e == QProcess::FailedToStart) p->deleteLater(); });
    p->start(scenes::displayTool(), {QStringLiteral("list")});
}

void ScenesTab::syncExtraRows() {
    const bool od = HomeTab::panelOdSupported().value_or(false) || loaded_.panelOd >= 0;
    panelOd_->setVisible(od);
    panelOdLabel_->setVisible(od);
    const bool rr = rates_.size() > 1 || loaded_.refresh > 0;
    refresh_->setVisible(rr);
    refreshLabel_->setVisible(rr);
    const bool sb = !QDir(QStringLiteral("/sys/class/backlight")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()
                    || loaded_.screenBrightness > 0;
    screenBrightRow_->setVisible(sb);
    screenBrightLabel_->setVisible(sb);
}

void ScenesTab::captureScreenBrightness() {
    auto *p = new QProcess(this);
    connect(p, &QProcess::finished, this, [this, p](int, QProcess::ExitStatus) {
        const QJsonObject o = QJsonDocument::fromJson(p->readAllStandardOutput().trimmed().split('\n').last()).object();
        p->deleteLater();
        if (!o.value("ok").toBool()) { setStatus("Could not read the screen brightness: " + o.value("error").toString("centurion-display failed"), theme::WARN); return; }
        screenBright_->setValue(std::clamp(o.value("percent").toInt(), 1, 100));
        setStatus(QStringLiteral("Screen brightness %1 % captured (%2).").arg(screenBright_->value()).arg(o.value("device").toString()), theme::OK);
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        p->deleteLater();
        setStatus("centurion-display not found.", theme::DANGER);
    });
    p->start(scenes::displayTool(), {QStringLiteral("brightness")});
}

// Active scene in the theme's accent (amber in Graphite, each palette's own equivalent elsewhere), the selected
// row in the normal text colour, the rest dimmed. Set per item: a colour in the ::item stylesheet rule would win.
void ScenesTab::colorList() {
    const QString active = eng_->active();
    for (int i = 0; i < list_->count(); ++i) {
        QListWidgetItem *it = list_->item(i);
        const char *c = it->data(Qt::UserRole).toString() == active && !active.isEmpty() ? theme::ACCENT
                        : it == list_->currentItem() ? theme::FG : theme::FG_DIM;
        it->setForeground(QColor(QLatin1String(c)));
    }
}

void ScenesTab::reloadList(const QString &selectName) {
    filling_ = true;
    const QString want = selectName.isEmpty() ? loaded_.name : selectName;
    list_->clear();
    const QStringList names = scenes::names();
    const auto a = eng_->autoConfig();
    for (const QString &n : names) {
        QStringList tags;
        if (a.enabled && n == a.onAc) tags << QStringLiteral("AC");
        if (a.enabled && n == a.onBattery) tags << QStringLiteral("battery");
        auto *it = new QListWidgetItem(tags.isEmpty() ? n : n + QStringLiteral("  ·  ") + tags.join('/'));
        it->setData(Qt::UserRole, n);
        if (n == eng_->active()) { QFont f = it->font(); f.setBold(true); it->setFont(f); it->setToolTip("Active"); }
        list_->addItem(it);
    }
    filling_ = false;
    const bool any = !names.isEmpty();
    editor_->setVisible(any);
    empty_->setVisible(!any);
    dup_->setEnabled(any);
    del_->setEnabled(any);
    if (!any) { loaded_ = {}; return; }
    const int i = names.indexOf(want);
    list_->setCurrentRow(i < 0 ? 0 : i);  // fires select(); a no-op for the scene already loaded
    colorList();
}

void ScenesTab::select(const QString &name) {
    if (name.isEmpty() || name == loaded_.name) return;
    if (fromEditor() != loaded_ && !loaded_.name.isEmpty()) {
        const auto r = QMessageBox::question(this, "Unsaved changes", "Save the changes to '" + loaded_.name + "' first?",
                                             QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (r == QMessageBox::Cancel) {
            // Put the selection back — deferred: we are inside the list's own signal.
            QTimer::singleShot(0, this, [this] { reloadList(loaded_.name); });
            return;
        }
        if (r == QMessageBox::Save && !saveCurrent()) return;
    }
    if (auto s = scenes::load(name)) setEditor(*s);
}

void ScenesTab::updateDirty() {
    const bool dirty = !loaded_.name.isEmpty() && fromEditor() != loaded_;
    save_->setEnabled(dirty);
    editor_->setTitle(loaded_.name + (dirty ? QStringLiteral("  •") : QString()));
    // Firmware limits need Custom: say so where the choice is made.
    if (!firmware_.isEmpty() && profile_->currentData().toString() != QLatin1String("custom"))
        setStatus("Firmware limits only apply in the Custom power profile — pick Custom above or they will be skipped.", theme::WARN);
}

// Readable name and unit of a firmware attribute for the scene summary.
struct FwShown { QString name, unit; };
static FwShown fwShown(const QString &key) {
    static const struct { const char *key, *name; } KNOWN[] = {
        {"ppt_pl1_spl", "CPU PL1 · sustained"}, {"ppt_pl2_sppt", "CPU PL2 · short boost"}, {"ppt_pl3_fppt", "CPU PL3 · fast peak"},
        {"cpu_temp", "CPU temperature target"}, {"gpu_nv_ctgp", "GPU cTGP"}, {"gpu_nv_ppab", "GPU Dynamic Boost ceiling"},
        {"gpu_nv_cpu_boost", "GPU Dynamic Boost floor"}, {"gpu_temp", "GPU temperature target"},
        {"ppt_cpu_cl", "CPU cross-load limit"}, {"gpu_nv_ac_offset", "GPU AC baseline offset"},
    };
    // The firmware's own display names (sysfs), read once: they do not change while the app runs.
    static const QHash<QString, QString> display = [] {
        QHash<QString, QString> h;
        for (const FwAttr &a : FwattrTab::discover()) {
            QString n = a.displayName;
            if (const int i = n.indexOf(QStringLiteral("  (")); i > 0) n.truncate(i);  // "(missing from sysfs — via WMI)"
            n = n.trimmed();
            // Firmware names are phrased as commands ("Set the CPU cross loading power limit"): keep the noun.
            if (n.startsWith(QLatin1String("Set the "), Qt::CaseInsensitive)) n = n.mid(8);
            else if (n.startsWith(QLatin1String("Set "), Qt::CaseInsensitive)) n = n.mid(4);
            if (!n.isEmpty()) n[0] = n[0].toUpper();
            if (!n.isEmpty() && n != a.name) h.insert(a.name, n);
        }
        return h;
    }();
    FwShown f{display.value(key, key), QString()};
    for (const auto &k : KNOWN)
        if (key == QLatin1String(k.key)) { f.name = QString::fromUtf8(k.name); break; }
    if (key.contains(QLatin1String("temp"))) f.unit = QStringLiteral("°C");
    else if (key.contains(QLatin1String("tau")) || key.endsWith(QLatin1String("_time"))) f.unit = QStringLiteral("s");
    else if (key.startsWith(QLatin1String("ppt_")) || key == QLatin1String("gpu_nv_ctgp") || key == QLatin1String("gpu_nv_ppab")
             || key == QLatin1String("gpu_nv_cpu_boost")) f.unit = QStringLiteral("W");
    return f;
}

void ScenesTab::updateFirmwareLabel() {
    fwClear_->setEnabled(!firmware_.isEmpty());
    if (firmware_.isEmpty()) {
        fwLabel_->setText(QStringLiteral("<span style='color:%1'>Unchanged</span>").arg(theme::MUTED));
        fwLabel_->setToolTip(QString());
        return;
    }
    // Every captured value, one row each: CPU limits, then GPU, then the rest (known names first, in a fixed order).
    struct Line { int group, rank; QString key; FwShown f; int value; };
    QList<Line> lines;
    static const char *const ORDER[] = {"ppt_pl1_spl", "ppt_pl2_sppt", "ppt_pl3_fppt", "cpu_temp",
                                        "gpu_nv_ctgp", "gpu_nv_ppab", "gpu_nv_cpu_boost", "gpu_temp"};
    for (auto it = firmware_.cbegin(); it != firmware_.cend(); ++it) {
        const QString &k = it.key();
        const int group = k.startsWith(QLatin1String("cpu")) || k.startsWith(QLatin1String("ppt")) ? 0
                        : k.startsWith(QLatin1String("gpu")) || k.startsWith(QLatin1String("dgpu")) ? 1 : 2;
        int rank = int(std::size(ORDER));
        for (int i = 0; i < int(std::size(ORDER)); ++i) if (k == QLatin1String(ORDER[i])) { rank = i; break; }
        lines.append({group, rank, k, fwShown(k), it.value()});
    }
    std::stable_sort(lines.begin(), lines.end(), [](const Line &a, const Line &b) {
        return std::tie(a.group, a.rank, a.key) < std::tie(b.group, b.rank, b.key);
    });
    // Two columns, filled top to bottom (CPU limits on the left, GPU on the right when both are there):
    // half the height of one row per value, so a Custom scene is not much taller than the others.
    const int n = int(lines.size()), rows = (n + 1) / 2;
    auto cells = [](const Line &l, bool gap) {
        const QString val = l.f.unit.isEmpty() ? QString::number(l.value) : QStringLiteral("%1 %2").arg(l.value).arg(l.f.unit);
        return QStringLiteral("<td style='white-space:nowrap; padding-left:%1px'>%2</td>"
                              "<td align='right' style='white-space:nowrap; padding-left:10px'><b>%3</b></td>"
                              "<td style='white-space:nowrap; padding-left:8px; color:%4; font-size:small'>%5</td>")
            .arg(gap ? 28 : 0).arg(l.f.name.toHtmlEscaped(), val.toHtmlEscaped(), QString::fromLatin1(theme::MUTED), l.key.toHtmlEscaped());
    };
    QString html = QStringLiteral("<table cellspacing='0' cellpadding='0'>");
    for (int r = 0; r < rows; ++r) {
        html += QStringLiteral("<tr>") + cells(lines[r], false);
        if (r + rows < n) html += cells(lines[r + rows], true);
        html += QStringLiteral("</tr>");
    }
    html += QStringLiteral("</table>");
    QStringList tip;
    for (const Line &l : lines) tip << l.key + " = " + QString::number(l.value);
    fwLabel_->setText(html);
    fwLabel_->setToolTip(QStringLiteral("%1 firmware value(s) stored in this scene (attribute = value):\n").arg(firmware_.size()) + tip.join('\n'));
}

void ScenesTab::updateFanTableLabel() {
    QStringList l;
    for (int x : fanTable_) l << QString::number(x);
    fanTableLabel_->setText(fanTable_.isEmpty() ? QStringLiteral("unchanged") : l.join(QStringLiteral(" · ")));
}

QMap<QString, int> ScenesTab::firmwareSnapshot(MainWindow *win, bool *wmiUnread) {
    QMap<QString, int> fw;
    bool hasWmi = false;
    for (const FwAttr &a : FwattrTab::discover()) {
        if (a.viaWmi()) { hasWmi = true; continue; }
        if (a.ranged) fw.insert(a.name, a.current);
    }
    const QMap<QString, int> wmi = win->fwattr() ? win->fwattr()->wmiValues() : QMap<QString, int>();
    for (auto it = wmi.cbegin(); it != wmi.cend(); ++it) fw.insert(it.key(), it.value());
    if (wmiUnread) *wmiUnread = hasWmi && wmi.isEmpty();
    return fw;
}

void ScenesTab::captureFirmware() {
    bool wmiUnread = false;
    const QMap<QString, int> fw = firmwareSnapshot(win_, &wmiUnread);
    if (fw.isEmpty()) {
        // No firmware attributes: the fan curve alone can still be captured.
        captureFanTable(QString());
        setStatus("This machine exposes no writable firmware attributes.", theme::WARN);
        return;
    }
    firmware_ = fw;
    QString note = QStringLiteral("Captured %1 firmware value(s).").arg(fw.size());
    if (wmiUnread) note += " The GPU cTGP/boost values were not read yet — open Firmware Attributes once and capture again to include them.";
    if (profile_->currentData().toString() != QLatin1String("custom") && profile_->findData(QStringLiteral("custom")) >= 0) {
        profile_->setCurrentIndex(profile_->findData(QStringLiteral("custom")));
        note += " Power profile set to Custom (required for these).";
    }
    updateFirmwareLabel();
    updateDirty();
    setStatus(note, theme::OK);
    // The EC fan boost state belongs to the same custom-mode snapshot.
    privileged::run(privileged::helperPath("legion-profile-helper"), QJsonObject{{"fan_fullspeed", "get"}}, this,
                    [this, note](const privileged::Result &r) {
                        if (!r.ok()) return;
                        fan_->setCurrentIndex(std::max(0, fan_->findData(r.json.value("on").toBool() ? 1 : 0)));
                        updateDirty();
                        setStatus(note + QStringLiteral(" Fan boost: %1.").arg(r.json.value("on").toBool() ? "turbo" : "auto"), theme::OK);
                    });
    captureFanTable(note);
}

// The active Custom-mode fan curve (EC table) belongs to the same snapshot; machines without
// the fan-table interface answer "unsupported" and simply keep it "unchanged".
void ScenesTab::captureFanTable(const QString &note) {
    privileged::run(privileged::helperPath("legion-profile-helper"), QJsonObject{{"fan_table", "get"}}, this,
                    [this, note](const privileged::Result &r) {
                        if (!r.ok()) return;
                        const QJsonArray a = r.json.value("levels").toArray();
                        if (a.size() != 10) return;
                        fanTable_.clear();
                        for (const QJsonValue &v : a) fanTable_ << v.toInt();
                        updateFanTableLabel();
                        updateDirty();
                        setStatus((note.isEmpty() ? QString() : note + ' ') + QStringLiteral("Fan curve captured."), theme::OK);
                    });
}

bool ScenesTab::saveCurrent() {
    const Scene s = fromEditor();
    QString err;
    if (!scenes::save(s, &err)) { setStatus("Could not save: " + err, theme::DANGER); return false; }
    loaded_ = s;
    updateDirty();
    win_->shortcuts()->sync();
    return true;
}

void ScenesTab::applyCurrent() {
    if (loaded_.name.isEmpty()) return;
    if (fromEditor() != loaded_ && !saveCurrent()) return;
    eng_->apply(loaded_.name);
}

void ScenesTab::newScene(bool duplicate) {
    bool ok = false;
    const QString title = duplicate ? "Duplicate scene" : "New scene";
    const QString name = QInputDialog::getText(this, title, "Scene name:", QLineEdit::Normal,
                                               duplicate ? loaded_.name + " copy" : QString(), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    if (!scenes::validName(name)) {
        QMessageBox::warning(this, title, "Use 1-64 characters: letters, digits, space, underscore or hyphen, starting with a letter or digit.");
        return;
    }
    if (scenes::names().contains(name)) { QMessageBox::warning(this, title, "A scene named '" + name + "' already exists."); return; }
    Scene s = duplicate ? fromEditor() : Scene{};
    s.name = name;
    s.shortcut.clear();  // a copy does not take the same key
    QString err;
    if (!scenes::save(s, &err)) { setStatus("Could not save: " + err, theme::DANGER); return; }
    loaded_ = {};  // no unsaved-changes prompt for the scene we are leaving (duplicate carries them)
    reloadList(name);
    setStatus(duplicate ? "Duplicated as '" + name + "'." : "Created '" + name + "'. Pick what it should set, then Save.", theme::OK);
}

void ScenesTab::deleteScene() {
    const QString n = loaded_.name;
    if (n.isEmpty() || QMessageBox::question(this, "Delete scene", "Delete scene '" + n + "'?\n\nThe profiles it refers to are kept.",
                                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    scenes::remove(n);
    win_->shortcuts()->sync();
    win_->optimize()->forgetGameScene(n);
    auto a = eng_->autoConfig();
    if (a.onAc == n || a.onBattery == n) {
        if (a.onAc == n) a.onAc.clear();
        if (a.onBattery == n) a.onBattery.clear();
        eng_->setAuto(a);
    }
    loaded_ = {};
    reloadList();
    reloadAuto();
    setStatus("Deleted '" + n + "'.");
}

void ScenesTab::reloadAuto() {
    filling_ = true;
    const auto a = eng_->autoConfig();
    const QStringList names = scenes::names();
    for (auto [c, sel] : {std::pair{onAc_, a.onAc}, std::pair{onBattery_, a.onBattery}}) {
        c->clear();
        c->addItem(QStringLiteral("— leave as is"), QString());
        for (const QString &n : names) c->addItem(n, n);
        c->setCurrentIndex(std::max(0, c->findData(sel)));
    }
    auto_->setChecked(a.enabled);
    resume_->setChecked(a.onResume);
    resume_->setEnabled(a.enabled);
    onAc_->setEnabled(!names.isEmpty());
    onBattery_->setEnabled(!names.isEmpty());
    filling_ = false;
}

void ScenesTab::storeAuto() {
    scenes::Auto a = eng_->autoConfig();  // keeps the pause state
    a.enabled = auto_->isChecked();
    a.onResume = resume_->isChecked();
    a.onAc = onAc_->currentData().toString();
    a.onBattery = onBattery_->currentData().toString();
    QString err;
    if (!eng_->setAuto(a, &err)) { setStatus("Could not save the automatic switching setting: " + err, theme::DANGER); return; }
    if (a.enabled && a.onAc.isEmpty() && a.onBattery.isEmpty()) setStatus("Pick a scene for AC and/or battery.", theme::WARN);
    reloadList(loaded_.name);
    updatePowerLabel();
}

void ScenesTab::updatePowerLabel() {
    const auto ac = eng_->powerSource();
    QString t = !ac ? QStringLiteral("Power source: unknown (no charger or battery reported)")
                    : *ac ? QStringLiteral("Now on AC power") : QStringLiteral("Now on battery");
    if (!eng_->active().isEmpty()) t += "  ·  active scene: " + eng_->active();
    power_->setText(t);
}

void ScenesTab::setStatus(const QString &msg, const char *color) {
    status_->setText(msg);
    status_->setStyleSheet(QStringLiteral("color:%1; background:transparent;").arg(color ? color : theme::FG_DIM));
}
