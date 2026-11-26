#pragma once
// Unlock PIN (crates/centurion-helpers/src/unlock.rs). The helpers that write new values to the hardware or
// the kernel refuse unless this window was unlocked with the user's 4-digit PIN. The PIN is checked by root
// (tune-profile-helper "unlock"); this object only keeps what the window shows and asks for it.
//
//   locked   -> the lock button asks for the PIN; no PIN yet: set one first
//               (tune-admin-helper "pin_set", the administrator password).
//   unlocked -> closing / hiding the window or leaving it idle locks the UI; previously approved scenes can still run.
#include <QObject>
#include <QString>
#include <QTimer>

class QWidget;

namespace unlock {

class Manager : public QObject {
    Q_OBJECT
public:
    static Manager &instance();
    bool unlocked() const { return unlocked_; }
    /// Asks for the PIN (or for a new one when none is set) and unlocks.
    void requestUnlock(QWidget *parent);
    /// Locks the UI. `keepScenes`: scene steps keep the grant (window closed, app still in the tray).
    void lock(bool keepScenes);
    /// Sets a new PIN (administrator password), then unlocks with it.
    void changePin(QWidget *parent);

Q_SIGNALS:
    void changed(bool unlocked);

private:
    Manager();
    bool eventFilter(QObject *receiver, QEvent *event) override;
    void askPin(QWidget *parent, const QString &note);
    void setNewPin(QWidget *parent);
    void submitPin(QWidget *parent, const QString &pin, bool retryWrongPin);
    void revokeGrant(bool keepScenes);
    void setUnlocked(bool on);
    QTimer idleTimer_;
    bool unlocked_ = false;
    bool busy_ = false;
    bool revoking_ = false;
    bool hiddenDuringUnlock_ = false;
};

}  // namespace unlock
