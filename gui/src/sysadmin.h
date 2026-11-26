#pragma once
// sysadmin-helper from the GUI (polkit com.centurion.sysadmin: the administrator password, then polkit's
// keep window). Every system change the Health tools offer goes through here: one confirmation that says
// what changes, the helper call, and one message with the helper's answer.
#include <QJsonObject>
#include <QString>
#include <functional>

class QLabel;
class QObject;
class QWidget;

namespace sysadmin {

using Done = std::function<void(bool ok, const QString &message)>;

/// Runs one request (no dialog). `cb` is not called if `ctx` is destroyed first.
void run(const QJsonObject &req, QObject *ctx, Done cb, int timeoutMs = 120000);
/// Same, with the helper's whole reply (e.g. "needs_confirm" / "fstab_issues" of a config_write).
using DoneJson = std::function<void(bool ok, const QString &message, const QJsonObject &reply)>;
void runJson(const QJsonObject &req, QObject *ctx, DoneJson cb, int timeoutMs = 120000);
/// The helper that serves `op`: sysadmin-config-helper (password every time) for configuration files,
/// modules and driver options (crates/centurion-helpers/src/sysadmin.rs CONFIG_OPS), sysadmin-helper otherwise.
QString helperFor(const QString &op);

/// Asks `confirm` (skipped when empty), runs `req`, shows the answer, then calls `after(ok)`.
/// The answer goes in `status` when given (inline, no dialog), else in a message box.
/// `req` {"op":"pipewire_restart"} restarts the user's sound servers without root.
void act(QWidget *parent, const QString &title, const QString &confirm, const QJsonObject &req,
         std::function<void(bool)> after = {}, QLabel *status = nullptr);

}  // namespace sysadmin
