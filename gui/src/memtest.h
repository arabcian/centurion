#pragma once
// Health → Curve stability → Memory test: the logic behind MemTestBox, kept free of widgets so it can be
// tested (tests/memtest_test.cpp).
//
// The test runs as the user, no root: stressapptest when installed (time-bounded, needs no mlock), else
// memtester (runs until stopped; it must lock its memory, so the size is held to RLIMIT_MEMLOCK and the
// user is told when that is the limit). Results go to $XDG_STATE_HOME/centurion/memtest.json, newest
// last, at most KEEP entries, each with the BIOS memory timing snapshot taken before the run
// (tune-profile-helper {"op":"mem_snapshot"}).
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

namespace memtest {

enum class Tool { None, Stressapptest, Memtester };
enum class Verdict { Pass, Fail, Incomplete };

constexpr qint64 MIN_MIB = 64;  // less than this tests too little to mean anything
constexpr int KEEP = 50;

QString toolName(Tool t);
/// MemAvailable of /proc/meminfo in KiB, or -1.
qint64 availableKib(const QByteArray &meminfo);

struct Plan {
    QStringList args;
    qint64 mib = 0;
    bool clamped = false;  // memtester: smaller than asked because of RLIMIT_MEMLOCK
    QString error;         // not startable; args empty
};
/// `percent` of the available memory for `seconds`. `memlockBytes` < 0 means unlimited.
Plan plan(Tool t, qint64 availKib, int percent, int seconds, qint64 memlockBytes);

/// What the output said so far, fed line by line.
struct Run {
    int errors = 0;        // memtester FAILURE lines; stressapptest "Hardware Error" lines or its final count
    bool unlocked = false; // memtester could not lock its memory and tested it unlocked
    bool statusPass = false, statusFail = false;  // stressapptest's closing "Status:" line
};
void feed(Run &r, Tool t, const QString &line);

/// `stopped`: Centurion ended a memtester run at its deadline (the normal end); `cancelled`: the user stopped it.
/// `crashed`: the process did not exit normally. `mce`: machine checks the Monitor saw during the run.
Verdict verdict(const Run &r, Tool t, int exitCode, bool crashed, bool stopped, bool cancelled, int mce);
QString verdictName(Verdict v);

QString stateFile();
QJsonArray load();
/// Appends one result and keeps the newest KEEP. False when the file could not be written.
bool append(const QJsonObject &result);

} // namespace memtest
