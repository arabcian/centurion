#pragma once
// Unlock PIN (crates/centurion-helpers/src/unlock.rs). The helpers that write new values to the hardware or
// the kernel refuse unless this window was unlocked with the user's 4-digit PIN. The PIN is checked by root
// (tune-profile-helper "unlock"); this object only keeps what the window shows and asks for it.
//
//   locked   -> the lock button (or the first refused write) asks for the PIN; no PIN yet: set one first
//               (tune-admin-helper "pin_set", the administrator password).
//   unlocked -> closing / hiding the window locks the UI again; scene steps keep the grant (they run from the tray).
#include <QObject>
#include <QString>
#include <functional>

class QWidget;

namespace unlock {

class Manager : public QObject {
    Q_OBJECT
public:
    static Manager &instance();
    bool unlocked() const { return unlocked_; }
    /// Asks for the PIN (or for a new one when none is set) and unlocks; `done(true)` once unlocked.
    void requestUnlock(QWidget *parent, std::function<void(bool)> done = {});
    /// Locks the UI. `keepScenes`: scene steps keep the grant (window closed, app still in the tray).
    void lock(bool keepScenes);
    /// Sets a new PIN (administrator password), then unlocks with it.
    void changePin(QWidget *parent, std::function<void(bool)> done = {});

Q_SIGNALS:
    void changed(bool unlocked);

private:
    void askPin(QWidget *parent, const QString &note, std::function<void(bool)> done);
    void setUnlocked(bool on);
    bool unlocked_ = false;
    bool asking_ = false;
};

}  // namespace unlock
