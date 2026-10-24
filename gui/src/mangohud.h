#pragma once
// MangoHud in-game overlay line: the active scene and power profile, shown through MangoHud's `exec=`.
// MangoHud re-runs `exec` at every refresh, so the GUI keeps the line in a small file and the block
// runs `cat` on it; `centurion-gamemode OVERLAY` (prints the same line) is the fallback when there is
// no usable runtime directory, and what blocks written by older versions still run. With the GUI not
// running the file is gone and the line stays empty.
//
// The switch adds (or removes) one marked block at the end of the user's
// ~/.config/MangoHud/MangoHud.conf. MangoHud only runs `exec` with
// legacy_layout=0, and in that layout it draws only the elements the file
// lists — so when the file does not already use it, the block also lists the
// four elements MangoHud shows by default (gpu_stats, cpu_stats, fps,
// frame_timing) unless the file names them itself. Removing the block gives
// back the file exactly as it was.
#include <QString>

namespace mangohud {

QString configPath();
/// MangoHud is installed (the `mangohud` launcher on PATH, or a config directory).
bool available();

/// The file the GUI keeps current with the overlay line ($XDG_RUNTIME_DIR/centurion-overlay; "" without one).
QString overlayFile();
/// What MangoHud's `exec=` runs: `cat <file>` (no process of ours per refresh), or `centurion-gamemode OVERLAY`
/// when there is no file path that needs no quoting.
QString execCommand(const QString &file);
/// "Scene · Power profile label", the same line `centurion-gamemode OVERLAY` prints.
QString overlayLine(const QString &scene, const QString &profileLabel);
/// Writes the line to overlayFile() when the overlay is on (only on change), removes the file when it is off.
void updateOverlayFile(const QString &line);

bool hasOverlay(const QString &conf);
QString withOverlay(const QString &conf, const QString &exec = execCommand(overlayFile()));
QString withoutOverlay(const QString &conf);

/// Reads, edits and writes the config file. `err` on failure.
bool enabled();
bool setEnabled(bool on, QString *err = nullptr);

} // namespace mangohud
