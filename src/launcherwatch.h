#pragma once
// Game detection for Heroic and Bottles — the two launchers that have no per-game hook Centurion can
// rely on. Opt-in (Optimizations → Game launch, tune.json "detect_launchers"); off = no timer at all.
//
// How a game is recognised (only the user's own processes are read):
//   Heroic   every process Heroic starts for a game carries HEROIC_APP_NAME in its environment
//            (Heroic's setupWrapperEnvVars); the game "runs" while any such process exists.
//   Bottles  Bottles sets WINEPREFIX to the bottle; a game is a Windows program (*.exe) running in a
//            prefix under a bottles directory, minus Wine's own service programs and installers.
// Games already wrapped by centurion-gamemode (CENTURION_WRAPPED in their environment) are skipped.
//
// Cost: a /proc sweep of process names every 15 s to see whether Heroic or Bottles runs at all; only
// while one does (or a detected game is still running) the environment of the user's processes is read
// every 5 s. A game counts as started after two sweeps in a row and as ended after two sweeps without it.
//
// Start / end run `centurion-gamemode PRE` / `POST` (game scene, ★ game preset, session log), exactly
// like a Lutris hook. The GUI is the session owner, so a session it leaves behind (crash) is pruned by
// tune-helper as soon as the GUI process is gone.
#include <QHash>
#include <QObject>
#include <QString>

class QTimer;

class LauncherWatch : public QObject {
    Q_OBJECT
public:
    explicit LauncherWatch(QObject *parent = nullptr);
    ~LauncherWatch() override;
    /// Reads tune.json "detect_launchers".
    static bool configured();
    void setEnabled(bool on);
    bool enabled() const { return enabled_; }

Q_SIGNALS:
    /// A detected game started / ended (`source` = "Heroic" or "Bottles").
    void gameStarted(const QString &name, const QString &source);
    void gameEnded(const QString &name, const QString &source);

private:
    struct Game { QString name, source; int seen = 0, missing = 0; bool active = false; };
    void slowTick();
    void fastTick();
    void retune();
    void gamemode(const QString &verb, const QString &game);
    bool launcherRunning() const;

    bool enabled_ = false;
    bool launcher_ = false;
    QTimer *slow_ = nullptr, *fast_ = nullptr;
    QHash<QString, Game> games_;  // key: "heroic:<app>" / "bottles:<prefix>|<exe>"
};
