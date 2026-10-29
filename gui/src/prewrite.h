#pragma once
// Safety copy of the Centurion configuration right before a write that persists
// in firmware: BIOS memory timings, BIOS CPU OC, GPU MUX mode and the forced
// iGPU-only override (all through legion-firmware-helper, which asks for the
// password every time).
//
// The copy is the same `config_backup` as Health → Backup, run as the user (no
// password), tagged "pre-<what>" so it is kept apart from the manual and
// automatic archives: backup-helper keeps only the newest few of these.
// Nothing here blocks the write: if the copy fails, the user gets a
// notification and the write goes ahead.
#include <QObject>
#include <QString>
#include <functional>

namespace prewrite {

/// `what` is the tag suffix: "memtiming", "biosoc", "gpumux" or "igpu".
/// `then` runs on the GUI thread once the copy has finished, whatever its result.
/// It is not called if `context` is destroyed meanwhile (the write is then dropped).
void backupThen(const QString &what, QObject *context, std::function<void()> then);

} // namespace prewrite
