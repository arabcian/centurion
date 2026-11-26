#pragma once
// Run a root helper through pkexec, asynchronously — port of privileged.py.
//
// Unlike the Python version (blocking subprocess.run on the GUI thread, which
// froze the window while the polkit dialog was up), this is non-blocking:
// the callback fires on the GUI thread when the helper finishes.
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <cstdint>

namespace privileged {

inline constexpr const char *INSTALL_PREFIX = CENTURION_HELPER_DIR;

/// Installed helper path (the one the polkit actions pin).
QString helperPath(const QString &relative);

/// legion-firmware-helper: settings that persist in firmware (BIOS memory
/// timings, BIOS CPU OC, GPU MUX mode). polkit asks for the administrator
/// password on every call, so callers give the user time to type it.
inline constexpr const char *FIRMWARE_HELPER = "legion-firmware-helper";
inline constexpr int FIRMWARE_TIMEOUT_MS = 180000;

struct Result {
    bool reached = false;   // false: pkexec/auth failure; `error` explains
    QJsonObject json;       // helper's own reply (ok/error/...) when reached
    QString error;
    bool ok() const { return reached && json.value(QStringLiteral("ok")).toBool(); }
    QString message() const;  // error text for either failure kind
};

// Real process lifetime; a timed-out root process may still be running.
QStringList activeOperations();
QStringList operationHistory();
bool resourceActive(const QString &helperName);

using Callback = std::function<void(const Result &)>;
// Session lease covers the gaps between helper processes. GUI-process only.
// Ordinary helpers cannot write a leased CPU curve. Emergency recovery stays available.
std::uint64_t acquireCpuCurveLease(QObject *context);
bool releaseCpuCurveLease(std::uint64_t id);
// Timeout is reported immediately via onTimeout, but the result callback is
// deferred until the actual process exit. A timed-out reply is never success.
void runUntilExit(const QString &helper, const QJsonObject &payload, QObject *context,
                  Callback cb, int timeoutMs = 60000, std::function<void()> onTimeout = {},
                  std::uint64_t lease = 0);
void runUntilExit(const QString &helper, const QByteArray &payload, QObject *context,
                  Callback cb, int timeoutMs = 60000, std::function<void()> onTimeout = {},
                  std::uint64_t lease = 0);

/// Unlock PIN (crates/centurion-helpers/src/unlock.rs): a helper answers {"locked": true} when a new value
/// needs the unlocked window. The handler (set once by MainWindow) asks for the PIN and calls `done(true)`
/// once unlocked; the request is then sent again. Requests marked "scene": true are never retried (the
/// scene engine reports the refusal). Without a handler the refusal is returned as it is.
using LockedHandler = std::function<void(std::function<void(bool unlocked)> done)>;
void setLockedHandler(LockedHandler handler);

/// Starts `pkexec helper`, writes `payload` to stdin. `context` bounds the
/// callback's lifetime (not invoked if context is destroyed first).
void run(const QString &helper, const QJsonObject &payload, QObject *context, Callback cb,
         int timeoutMs = 60000);
void run(const QString &helper, const QByteArray &payload, QObject *context, Callback cb,
         int timeoutMs = 60000);

} // namespace privileged
