#pragma once
// Drift detector: notices when something other than Centurion changes a setting Centurion applied
// (TLP on a charger event, power-profiles-daemon, tuned, auto-cpufreq, thermald, a script…).
//
// After an apply settles (scene finished, preset changed) it takes a reference snapshot of exactly the
// settings Centurion put in place — the Optimizations knobs it changed (tune-helper's state) and the
// firmware limits of the active scene — as they read back then. Later checks compare against that
// snapshot in the same representation, so a value that merely reads back differently from how it was
// written never counts. Checks: 30 s after a power-source change, every 10 min, and when asked.
// Unprivileged throughout (tune-helper describe as the user, sysfs reads).
#include <QHash>
#include <QObject>
#include <QStringList>

class MainWindow;
class QTimer;

class DriftWatch : public QObject {
    Q_OBJECT
public:
    explicit DriftWatch(MainWindow *win);
    /// Re-applies what drifted: the active scene if there is one, else the Optimizations preset.
    void reapply();
    void check() { run(false); }

Q_SIGNALS:
    /// `lines`: "key: now X (was Y after Centurion applied it)"; empty = nothing drifted (any more). `culprits`: tools found running.
    void drift(const QStringList &lines, const QString &culprits);

private:
    void run(bool snapshot);
    void onDescribe(const QJsonObject &d, bool snapshot);
    static QString culprits();

    MainWindow *win_;
    QTimer *periodic_, *soon_;
    QHash<QString, QString> ref_;   // key → value read back right after the apply
    QString refSig_;                // tune state signature the snapshot belongs to
    QString preset_;                // preset named in tune-helper's state
    QStringList last_;              // last reported drift (no repeat notifications)
    bool busy_ = false, wantSnapshot_ = false;
};
