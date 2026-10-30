#pragma once
// Health → Curve stability → CPU test: the logic behind CpuStressBox, kept free of widgets so it can be
// tested (tests/cpustress_test.cpp).
//
// `centurion-cpustress` (crates/centurion-cpustress) runs as the user: `percore` moves one thread from
// physical core to physical core (the Curve Optimizer / undervolt offset of one core is what fails, and a
// lone thread runs at the highest boost), `allcore` loads every logical CPU. Every chunk of work is compared
// with a reference computed on two other cores. It prints one JSON object per line.
//
// Results go to $XDG_STATE_HOME/centurion/cpustress.json, newest last, at most KEEP entries, each with the
// CPU curve in effect when the run started (CurvePage::currentState("cpu"): Curve Optimizer or Intel
// undervolt). A run is stored as "running" when it starts and updated whenever the per-core test moves on,
// so a machine that goes down under the test leaves a record naming the core that was loaded ("crashed").
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QStringList>

namespace cpustress {

enum class Mode { PerCore, AllCore };
enum class Verdict { Pass, Fail, Incomplete };

constexpr int KEEP = 50;
constexpr int MAX_CYCLES = 10;

QString modeName(Mode m);  // "percore" / "allcore": the tool's argument and the record's value

struct Info {
    QList<QList<int>> cores;  // logical CPUs of each physical core, the first one is what the per-core test loads
    QStringList loads;        // of "sse", "avx2", "avx512": the ones this CPU can run
};
Info parseInfo(const QByteArray &infoJson);

struct Plan {
    QStringList args;
    QString error;  // "load" or "seconds": not startable; args empty
};
/// `seconds`: per core (PerCore) or for the whole run (AllCore). `cycles` is PerCore only.
Plan plan(Mode m, const QString &load, int seconds, bool transitions, int cycles = 1);

/// What the output said so far, fed line by line.
struct Run {
    qint64 chunks = 0;
    qint64 errors = 0;      // the full count from progress / end lines, at least the error lines seen
    qint64 errorLines = 0;
    int cpu = -1;           // per-core: the CPU the thread is on now
    QList<int> cpus;        // start: the CPUs under test
    QList<int> failed;      // CPUs that returned a wrong result
    QList<int> done;        // per-core: CPUs whose turn ended (core_end), in order
    QString status, msg;    // end
    bool ended = false;
};
/// Returns true when the line moved the per-core test to another CPU (the running record is worth updating).
bool feed(Run &r, const QString &line);

/// A kernel event of this session that counts against a running CPU test: a machine check or a lockup.
bool isCpuFault(const QString &kind);
/// The CPU an MCE or lockup line names ("CPU 3: Machine Check …", "soft lockup - CPU#5 stuck …"), or -1.
int eventCpu(const QString &text);

/// `cancelled`: the user stopped it. `crashed`: the process did not exit normally. `cpuFaults`: isCpuFault events during the run.
Verdict verdict(const Run &r, int exitCode, bool crashed, bool cancelled, int cpuFaults);
QString verdictName(Verdict v);

/// The first 16 hex digits of the SHA-1 of a Curve Optimizer profile file: a result belongs to the profile as it was tested.
QString profileHash(const QByteArray &profileFile);
/// The newest finished test (pass / fail / crashed) of this profile with this content, or {} when none.
QJsonObject verification(const QJsonArray &all, const QString &profile, const QString &hash);

QString stateFile();
QJsonArray load();
/// Stores a record: replaces the newest one when it is the same run ("id") and still "running", else appends.
/// Keeps the newest KEEP. False when the file could not be written.
bool store(const QJsonObject &result);
/// Runs left "running" by an earlier session: "crashed" when they started in another boot (the machine went
/// down under the test), "incomplete" when in this one (Centurion itself ended). Saved; returns the crashed ones.
QJsonArray settle(const QString &bootId);

} // namespace cpustress
