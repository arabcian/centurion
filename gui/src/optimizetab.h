#pragma once
// Optimizations tab — the GUI for tune-helper / centurion-gamemode.
//
// Every row comes from `tune-helper {"op":"describe"}` (run unprivileged: the
// helper only reads), so the tab never hard-codes a sysfs path. A row is
// [include] [name] [current] [editor] [↺]; "include" decides what goes into
// Apply and into a saved preset. Presets live in
// ~/.config/centurion/tune-presets/<name>.json, the same files
// centurion-gamemode reads for Lutris/Steam, so "Use for games" is the whole
// integration. Built-in presets are templates in code; they are copied to a
// user file the moment they are chosen for games or for boot.
#include <QJsonArray>
#include <QHash>
#include <QSet>
#include <QJsonObject>
#include <QWidget>
#include <functional>

class QEvent;
class QCheckBox;
class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class SceneEngine;
class Int64SpinBox;
class QTabWidget;
class QTimer;
class QStringListModel;

class OptimizeTab : public QWidget {
    Q_OBJECT
public:
    explicit OptimizeTab(QWidget *parent = nullptr);

    /// Built-in and user presets, as shown in the combo (tray menu).
    QStringList presetNames() const;
    /// Read-only focus request, also supported before first describe.
    void navigateSetting(const QString &key);
    /// Saved or built-in preset by name (file I/O only, no UI); {} if unknown.
    QJsonObject presetObject(const QString &name) const;
    /// A scene was deleted: drop it as the game scene (tune.json), so centurion-gamemode is not sent to a missing scene.
    void forgetGameScene(const QString &name);
    /// Load + apply as a manual change (tray). false if busy or unknown.
    bool applyNamedPreset(const QString &name);
    /// tune-helper, run as the user for "describe" (DriftWatch).
    static QString helperPathForDescribe();
    /// A built-in preset's values for this CPU's vendor ("CO validation" for the CPU test); {} if there is none.
    static QJsonObject builtinValues(const QString &name);
    /// Drift banner (DriftWatch): settings changed by something else since Centurion applied them.
    void setDrift(const QStringList &lines, const QString &culprits, std::function<void()> reapply);
    void restoreAll(bool confirm = true);
    /// Page with the rows of one System tools home (network / storage / kernel): the rows neither
    /// calibration nor Autotune touch. Same rows, presets and restore as the tabs here; created
    /// once, refilled whenever the rows are rebuilt.
    QWidget *homePage(const QString &home);
    /// Pauses Scenes, then writes every knob to the value centurion-boot-guard captured at boot (calibration baseline).
    void returnToBootDefaults();
    void setSceneEngine(SceneEngine *eng) { scenes_ = eng; }
    bool busy() const { return busy_; }
    bool tuningActive() const { return active_; }
    QString gamePreset() const;
    /// The root-owned approved copy of `name` exists and still holds `values`.
    static bool storeFresh(const QString &name, const QJsonObject &values);
    /// Records what was just approved for `name` (after a successful preset_save).
    static void noteApproved(const QString &name, const QJsonObject &values);

    static QString presetsDir();
    static QString configFile();

Q_SIGNALS:
    /// Optimizations → Game launch "Detect games started from Heroic and Bottles" toggled.
    void detectLaunchersChanged(bool on);

protected:
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Option { QString value, label; };
    struct Row {
        QString key, group, label, help, kind, current;
        QString home;      // System tools page that shows this row instead ("network", "storage", "kernel"; "" = this tab)
        QString conflict;  // another daemon fights this row (irqbalance vs irq.affinity)
        QList<Option> options;
        qint64 min = 0, max = 0;
        bool available = false, debugfs = false, caution = false, hotplug = false;
        bool userSwitch = false;  // radio on/off (rf.*): never filled in from the boot snapshot
        bool touched = false;  // user edited the editor since the last sync
        QString shadowedBy;    // label of the checked global row that covers this one (tune::SHADOWS)
        QCheckBox *include = nullptr;
        QLabel *name = nullptr, *cur = nullptr, *bootValue = nullptr, *baseline = nullptr, *viewState = nullptr;
        bool cached = false;
        QComboBox *filter = nullptr;
        QGridLayout *grid = nullptr;
        QWidget *stripe = nullptr;
        int gridLine = -1;
        QComboBox *combo = nullptr;
        Int64SpinBox *spin = nullptr;  // 64-bit: dirty limits exceed INT_MAX
        QPushButton *revert = nullptr;
    };

    void buildUi();
    QWidget *buildLaunchPage();
    void refresh(bool fromPoll = false);
    void onDescribe(const QJsonObject &d, bool light = false);
    void buildRows(const QJsonArray &rows);
    void jumpToSetting(const QString &query);
    bool focusSettingKey(const QString &key);
    QString pendingNavigationKey_;
    void updateRow(Row &r, const QJsonObject &o);
    void updateStateBanner();
    class QFrame *driftBar_ = nullptr;
    QLabel *driftText_ = nullptr;
    std::function<void()> driftReapply_;
    void updateLaunchPreview();
    void saveUndervolt();

    static bool validFor(const Row &r, const QString &v);
    QString editorValue(const Row &r) const;
    QString canonicalCcd(const QString &v) const;
    bool setEditorValue(Row &r, const QString &v);
    bool differs(const Row &r) const;
    void markRow(Row &r);
    void updateRowPresentation(Row &r);
    void setReadbackStale(bool stale);
    QString changePreview(const QJsonObject &values, bool forBoot) const;
    /// Greys out (and unchecks) every row a checked global row covers (describe "shadows").
    void applyShadows();
    /// Checking `key` at `value` would shadow one of `keys`.
    bool wouldShadow(const QString &key, const QString &value, const QSet<QString> &keys) const;
    Row *row(const QString &key);

    QJsonObject collectValues() const;
    QJsonObject collectRun() const;
    /// How a preset's rows were filled by loadPresetObject.
    struct Fill { int preset = 0, boot = 0, live = 0, unchecked = 0, radios = 0; };
    /// Loads a preset into the rows. With `fillDefaults` every row the preset leaves out is checked
    /// too, at its boot-guard default (bootValueFor): a preset then describes the whole state, not
    /// just the rows it changes. Returns the number of rows the preset itself set.
    int loadPresetObject(const QJsonObject &p, QStringList *skipped, bool fillDefaults = true, Fill *fill = nullptr);
    /// `values` plus every other available row at its boot-guard default (bootValueFor).
    QJsonObject withBootDefaults(const QJsonObject &values) const;
    /// Value for a row a preset leaves out: the one centurion-boot-guard captured early in this boot
    /// (/var/lib/centurion/defaults.json, `defs`); for a row the snapshot does not hold, the live
    /// value while Centurion has not changed it (*live = true). Empty: nothing trustworthy.
    QString bootValueFor(const Row &r, const QJsonObject &defs, bool *live = nullptr) const;
    /// The "values" object of the boot-guard snapshot ({} when it was never captured), without the
    /// radio switches (snapshots from older versions hold rf.wlan = 0, captured before the Wi-Fi was up).
    static QJsonObject bootDefaults();
    /// One-time repair of presets saved while the fill still took the radio switches from the boot
    /// snapshot (it turned Wi-Fi off at every scene change). Returns the presets it rewrote.
    static QStringList repairRadioFill();
    /// Offers (once) to re-approve the root-owned copies of the repaired presets and the boot preset.
    void offerRadioRepairApproval();
    QStringList radioRepaired_;
    bool radioRepairOffered_ = false;
    QString fillSummary(const Fill &f) const;
    bool writeUserPreset(const QString &name, const QJsonObject &p, QString *err = nullptr);

    void reloadPresets(const QString &select = {}, bool poll = false);
    void loadSelected();
    void saveAs();
    void deleteSelected();
    void useForGames();
    void setBoot(const QString &home = {});  // home: add only that page's checked rows to the stored boot preset
    void clearBoot();
    /// The boot preset is persistent; row checkboxes are transient selection state.
    void updateHomeBootStatuses();
    void loadHomeBootValues(const QString &home);

    // Autotune: tune-helper {"op":"autotune"} profiles the hardware and
    // returns a preset for the chosen base (see centurion_helpers::autotune).
    void runAutotune(bool exploreOnly = false);
    /// Per-goal objective weights (QSettings autotune/weights/<goal>); empty = goal defaults.
    QJsonObject autotuneWeights(const QString &goal) const;
    void editAutotuneWeights();
    void showAutotuneWeights();
    bool weightsPending_ = false;
    void showAutotuneReport(const QJsonObject &d, const QStringList &notLoaded, bool exploreOnly = false);
    /// "Calibrate at next boot…": schedules centurion-calibrate sessions for the next boot (tune-helper calibrate_on_boot).
    void scheduleBootCalibration();
    /// Status line of a scheduled / running / finished boot calibration (world-readable files, no helper call).
    void refreshCalibrationStatus();
    void fillAutotuneContexts();
    void importFieldData();

    void applySelected(const QString &home = {});  // home: only that page's checked rows
    void applyValues(const QJsonObject &values, const QString &preset);
    void revertRow(const QString &key);
    void runOp(const QJsonObject &req, const QString &what, std::function<void(const QJsonObject &)> then = {});
    // Stores the preset's values root-owned (tune-helper preset_save) so it can be applied by name; then() runs on success.
    void approvePreset(const QString &name, const QJsonObject &values, std::function<void()> then);
    void setBusy(bool b);
    void showStatus(const QString &msg, const char *color = nullptr, int ms = 6000);

    QList<Row> rows_;
    QJsonObject topology_, state_, boot_, isolation_, baselineValues_;
    bool readbackStale_ = true;
    QJsonArray discovered_;  // scheduler entries the helper's table does not know (read-only)
    QJsonArray shadows_;     // tune::SHADOWS: {"global", "when", "keys"}
    bool active_ = false, busy_ = false, describing_ = false, helperMissing_ = false;
    bool needFull_ = true;                 // next describe must read the slow rows too
    int opGen_ = 0;                        // bumped when a privileged op starts: a describe begun before it is stale
    QHash<QString, QJsonObject> slowKeep_; // slow rows as of the last full describe

    QLabel *banner_ = nullptr, *bannerDetail_ = nullptr, *status_ = nullptr, *bootLabel_ = nullptr;
    QLabel *conflictBanner_ = nullptr;  // hardware policy and swap tier diagnostics (read-only)
    SceneEngine *scenes_ = nullptr;
    QPushButton *defaultsBtn_ = nullptr;
    QPushButton *restoreBtn_ = nullptr, *applyBtn_ = nullptr, *gameBtn_ = nullptr, *bootBtn_ = nullptr, *bootClear_ = nullptr;
    QComboBox *presetCombo_ = nullptr, *affinity_ = nullptr, *autoGoal_ = nullptr, *autoMode_ = nullptr;
    QPushButton *autoBtn_ = nullptr, *autoExploreBtn_ = nullptr;
    // The request of the last Fast result: Explore… asks the same question (goal, weights, profile).
    QJsonObject exploreReq_;
    bool autoRunning_ = false;
    QSpinBox *nice_ = nullptr;
    QComboBox *sliceUs_ = nullptr, *gameShare_ = nullptr, *uclampMin_ = nullptr, *uclampMax_ = nullptr, *memoryLow_ = nullptr;
    QCheckBox *zramRecompress_ = nullptr;
    QCheckBox *autogroup_ = nullptr, *preciseTimers_ = nullptr, *uvCpu_ = nullptr, *uvGpu_ = nullptr, *wirelessLock_ = nullptr;
    QLabel *uvInfo_ = nullptr;
    QComboBox *gameScene_ = nullptr;   // scene the first game switches to (tune.json "game_scene")
    QComboBox *autoCtx_ = nullptr;     // power profile autotune decides for (a scene's, or the live one)
    QLabel *calLabel_ = nullptr;       // boot calibration status
    QCheckBox *detectLaunchers_ = nullptr;  // tune.json "detect_launchers": Heroic / Bottles game detection
    QCheckBox *fieldAb_ = nullptr;     // tune.json "field_ab": A/B comparison of the game preset in real games
    void fillGameScenes();
    QLineEdit *lutrisPre_ = nullptr, *lutrisPost_ = nullptr, *lutrisPrefix_ = nullptr, *steam_ = nullptr;
    QLabel *topoLabel_ = nullptr;
    QTabWidget *groups_ = nullptr;
    QHash<QString, QWidget *> homeHolders_;
    QHash<QString, QLabel *> homeBootStatus_;
    QHash<QString, QPushButton *> homeBootLoad_;
    QLineEdit *settingSearch_ = nullptr;
    QStringListModel *settingSearchModel_ = nullptr;
    QTimer *poll_ = nullptr, *statusTimer_ = nullptr;
    QString loadedPreset_;
};
