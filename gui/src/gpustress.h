#pragma once
// Health → Curve stability → GPU test: the logic behind GpuStressBox, kept free of widgets so it can be
// tested (tests/gpustress_test.cpp).
//
// `centurion-gpustress` (crates/centurion-gpustress, specification docs/GPUSTRESS.md) runs as the user:
// `vram` fills a share of the VRAM with patterns and reads them back (use it after a memory clock offset
// change), `core` runs dependent arithmetic and compares every run with a reference run of the same work
// (use it after a core curve change). It prints one JSON object per line: start, progress, error, end.
// Results go to $XDG_STATE_HOME/centurion/gpustress.json, newest last, at most KEEP entries, each with the
// GPU curve state in effect when the run started (CurvePage::currentState).
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QStringList>

namespace gpustress {

enum class Mode { Vram, Core };
enum class Verdict { Pass, Fail, Incomplete };

constexpr int KEEP = 50;
constexpr int MIN_PERCENT = 10, MAX_PERCENT = 95;
constexpr int DEFAULT_PERCENT = 80, DEFAULT_MINUTES = 10;

QString modeName(Mode m);  // "vram" / "core": the tool's argument and the record's value

struct Device {
    QString pci, name, type;  // type: discrete / integrated / other
    qint64 vramMib = 0;
};
/// The devices of `centurion-gpustress list` the user may test: CPU implementations (lavapipe / llvmpipe) are left out.
QList<Device> parseDevices(const QByteArray &listJson);
/// Index of the GPU to preselect: the first discrete one, else the first.
int preselect(const QList<Device> &devices);

struct Plan {
    QStringList args;
    QString error;  // "device", "percent" or "seconds": not startable; args empty
};
/// `percent` is only used by Mode::Vram.
Plan plan(Mode m, const QString &pci, int percent, int seconds);

/// What the output said so far, fed line by line.
struct Run {
    int errors = 0;          // wrong words: the full count from progress / end lines, at least the error lines seen
    int errorLines = 0;
    int passes = 0;
    qint64 bytes = 0;        // start: how much one run tests
    QString name;            // start: the device
    QString status;          // end: pass / fail / stopped / device_lost / init_failed
    QString msg;             // end
    bool ended = false, deviceLost = false;
};
void feed(Run &r, const QString &line);

/// A kernel event of this session that counts against a running test: an NVIDIA Xid (45 and 154 are
/// follow-ups of another Xid, as everywhere else) or an AMD GPU hang / reset.
bool isGpuFault(const QString &kind, int code);

/// `cancelled`: the user stopped it. `crashed`: the process did not exit normally (or was killed after the
/// deadline). `gpuFaults`: isGpuFault events the Monitor saw during the run.
Verdict verdict(const Run &r, int exitCode, bool crashed, bool cancelled, int gpuFaults);
QString verdictName(Verdict v);

QString stateFile();
QJsonArray load();
/// Appends one result and keeps the newest KEEP. False when the file could not be written.
bool append(const QJsonObject &result);

} // namespace gpustress
