#pragma once
// Scenes: one named state for the whole machine, built from the profiles the
// tabs already save — platform profile, firmware limits, CPU curve (Ryzen CO
// or Intel undervolt), NVIDIA curve, Optimizations preset, and an optional
// user command, plus the keyboard lighting profile/brightness. A scene only *references* those profiles by name, so there is
// no second copy of any setting to drift out of sync.
//
// SceneEngine applies a scene step by step through the same root helpers the
// tabs use, strictly in dependency order (platform profile first: firmware
// limits only accept writes in Custom). It never opens dialogs — it runs from
// the tray and on power-source changes with the window hidden — and reports
// through finished(). Automatic switching watches the power source.
//
// Files (per user):
//   ~/.config/centurion/scenes/<name>.json
//   ~/.config/centurion/scenes.json      {"auto", "on_ac", "on_battery"}
#include <QElapsedTimer>
#include <QJsonObject>
#include <QMap>
#include <QVector>
#include <QObject>
#include <QStringList>
#include <functional>
#include <optional>

class MainWindow;
class QSocketNotifier;
class QTimer;
class QFileSystemWatcher;

namespace scenes {

/// One component of a scene.
struct Choice {
    enum Kind { Unchanged, Reset, Profile } kind = Unchanged;
    QString name;  // Profile only
    bool operator==(const Choice &) const = default;
};

struct Scene {
    QString name;
    QString platformProfile;       // empty = unchanged
    QMap<QString, int> firmware;   // firmware-attribute name → value; empty = unchanged
    Choice cpu, gpu, tuning;       // tuning Reset = restore originals
    int lightProfile = -1;         // keyboard lighting profile 1-6; -1 = unchanged
    int lightBrightness = -1;      // keyboard brightness 0-9; -1 = unchanged
    QVector<int> fanTable;         // Custom-mode EC fan table (10 levels, 1..10); empty = unchanged
    int fanFullSpeed = -1;         // EC fan boost (Full Speed): 1 on, 0 auto; -1 = unchanged
    QString command;               // optional; run as the user, no shell
    bool operator==(const Scene &) const = default;
};

struct Auto {
    bool enabled = false;
    QString onAc, onBattery;       // scene names; empty = leave as is
    bool paused = false;           // no automatic scene changes (power source, login, game); manual Apply still works
    bool onResume = false;         // after waking from suspend, reapply the scene for the current power source (needs `enabled`)
};

QString dir();
bool validName(const QString &name);
QStringList names();
std::optional<Scene> load(const QString &name);
bool save(const Scene &s, QString *err = nullptr);
bool remove(const QString &name);
Auto loadAuto();
bool saveAuto(const Auto &a, QString *err = nullptr);

/// true = on mains (barrel or USB-C PD), false = on battery,
/// nullopt = cannot tell: the machine reports no power supplies, or a sysfs read
/// failed (the EC is busy answering _PSR/_BST during a profile or limit change).
/// A failed read is NOT "offline" -- treating it as such made the machine flip to
/// the battery scene for a moment while a scene was being applied.
/// `lenient` = read errors count as "offline" again (the engine falls back to
/// this only after the source has been unreadable for a long time).
std::optional<bool> onAc(bool lenient = false);

/// Shared with centurion-gamemode ($XDG_RUNTIME_DIR/centurion/scene.json):
/// the active scene, whoever applied it, and the game-launch bookkeeping.
QString activeScene();
void setActiveScene(const QString &name);
/// Game sessions currently holding game mode (tune-helper's state).
int gameSessions();

/// centurion-boot-guard state (/var/lib/centurion/boot-guard.json):
/// non-empty reason = boot presets paused after a crashed boot.
QString bootGuardReason();
/// Login guard: the automatic scene at login is paused because the last
/// login's scene apply was followed by a crash. Empty = not paused.
QString loginGuardReason();
void resumeLoginGuard();

} // namespace scenes

class SceneEngine : public QObject {
    Q_OBJECT
public:
    explicit SceneEngine(MainWindow *win);

    /// Applies a saved scene. While one is running, the latest request is
    /// queued and runs right after (an AC flip mid-apply is never lost).
    void apply(const QString &name);
    bool busy() const { return busy_; }
    /// Active scene (applied by the GUI or by centurion-gamemode at game start/exit).
    QString active() const { return scenes::activeScene(); }

    scenes::Auto autoConfig() const { return auto_; }
    /// Persists the setting; enabling it applies the matching scene at once.
    bool setAuto(const scenes::Auto &a, QString *err = nullptr);
    bool paused() const { return auto_.paused; }
    bool setPaused(bool on, QString *err = nullptr);
    std::optional<bool> powerSource() const { return ac_; }
    /// Applies a saved CPU profile from its file (no tab editor, no dialogs); `done(ok, message)`.
    void applyCpuProfile(const QString &name, bool fromScene, std::function<void(bool ok, const QString &msg)> done);

Q_SIGNALS:
    void started(const QString &name);
    /// `log` holds one line per component, failures prefixed with "✗".
    void finished(const QString &name, bool ok, const QStringList &log);
    void powerSourceChanged(bool onAc);
    void pausedChanged(bool paused);
    /// The active scene was changed from outside the GUI (centurion-gamemode PRE/POST/SCENE): `game` = it is a game scene.
    void activeSceneChanged(const QString &name, bool game);

private:
    using Done = std::function<void(bool ok, const QString &msg)>;
    using Step = std::function<void(Done)>;

    void start(const scenes::Scene &s);
    void addStep(const QString &what, Step step);
    void next();
    void finish();
    void pollPower(bool timerTick = false);
    void commitPowerChange(bool onAc);
    void retunePoll();
    void watchUevents();
    void watchSceneState();
    void externalStateChanged();
    void checkGameEnd();
    void armVerify();
    void verifyPass();
    void retuneResume();
    void resumeTick();
    bool armResumeFd();
    void resumePass();
    void applyForSource(bool onAc);
    void startupApply();
    void helper(const QString &name, const QJsonObject &req, Done done,
                std::function<QString(const QJsonObject &)> describe = {});

    MainWindow *win_;
    QList<QPair<QString, Step>> steps_;
    QStringList log_;
    bool ok_ = true, busy_ = false;
    QString current_, pending_;
    bool startupPending_ = false;  // login scene waiting for the first readable power source
    bool deferred_ = false;  // a power-source switch waiting for the game to end
    bool calDeferred_ = false;  // a switch waiting for centurion-calibrate to release its hold
    bool nextAuto_ = false, pendingAuto_ = false, autoApply_ = false;  // the apply being started / queued / running came from automatic switching
    QString expectedProfile_;   // platform profile of the scene that was just applied ("" = scene does not set one)
    int verifyTriesLeft_ = 0;
    QTimer *verify_ = nullptr;  // re-checks the profile a few seconds after an automatic switch (the EC can override it late)

    scenes::Auto auto_;
    QTimer *powerTimer_;
    std::optional<bool> ac_, candidate_;
    // Power-source debounce is by TIME, not by number of reads: uevents call pollPower() in bursts
    // (ACAD + BAT0 + UCSI within a few ms), which used to satisfy a "2 reads" debounce instantly.
    QElapsedTimer candidateClock_, unknownClock_, clock_;
    QTimer *debounce_ = nullptr, *uevTimer_ = nullptr, *flapRelease_ = nullptr;
    QVector<qint64> flips_;   // clock_ times of recent committed power-source changes
    bool flapHold_ = false;   // source keeps flipping: automatic switching is held until it settles
    int gameGoneReads_ = 0;
    bool sawGameScene_ = false;
    bool gameTookOver_ = false;  // an automatic switch gave way to a starting game
    QFileSystemWatcher *stateWatch_ = nullptr;  // $XDG_RUNTIME_DIR/centurion: scene.json is replaced by rename
    QTimer *stateTimer_ = nullptr;
    QString seenActive_;                        // last active scene this engine knows about (own applies and external ones)
    int uevFd_ = -1;  // uevent socket; -1 → poll at the fast cadence as before
    QTimer *resumeTick_ = nullptr, *resumePass_ = nullptr;  // suspend detector / reapply passes after a wake
    // Wall-clock-set notification (timerfd): the kernel raises it on every resume, so the detector
    // sleeps instead of ticking. -1 / nullptr (no timerfd): the 2 s tick as before.
    int resumeFd_ = -1;
    QSocketNotifier *resumeNotifier_ = nullptr;
    long long bootOffsetNs_ = -1;  // CLOCK_BOOTTIME - CLOCK_MONOTONIC at the last tick; it grows by the time spent suspended
    int resumePassesLeft_ = 0;
};
