#pragma once
// CO Search crash watcher: which core, if any, the system logs blame for an interrupted trial (no widgets, no
// I/O, so it can be tested). The dialog collects the records: the live watcher during a trial (MCA threshold
// counters and classified kernel events, written to the recovery journal at once), the machine-check records
// the kernel prints early in the next boot, and the pstore records of the crashed boot.
//
// False-positive protection (judge): only machine-check records of a core's OWN units (load/store,
// instruction fetch, L2, decode, execution, floating point) can blame a core. A record of a shared unit
// (L3, memory controller, fabric) points at RAM / fabric as much as at a core: then nobody is blamed. Records
// naming more than one core: ambiguous, nobody. A record naming a core at stock contradicts the CO as the
// cause: nobody. Lockups and oopses name the CPU that noticed a problem, not the one that caused it: shown,
// never used.
#include "costrategy.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <optional>

namespace coevidence {

struct Record {
    QString source;           // "watch-counter", "watch-kernel", "boot-kernel", "pstore"
    QString kind;             // "mce" or "lockup"
    int cpu = -1;             // logical CPU, -1 = none named
    int bank = -1;            // MCA bank, -1 = unknown
    QString unit;             // MCA unit name when the source gives one (threshold counters)
    bool coreUnit = false;    // the bank / unit belongs to one core (see the header)
    bool uncorrected = false;
    QString text;
    QJsonObject toJson() const;
    static std::optional<Record> fromJson(const QJsonObject &o);
};

/// One classified kernel-log line (health-reader text, or a pstore line): a machine check naming a CPU, a
/// lockup naming a CPU, or nullopt.
std::optional<Record> fromKernelText(const QString &text, const QString &source);
/// A threshold counter that moved: .../machinecheckN/<unit>/<block>/error_count.
std::optional<Record> fromCounterPath(const QString &path);

struct Verdict {
    enum Kind { NoEvidence, Blamed, SharedUnit, Ambiguous, AtStock } kind = NoEvidence;
    int core = -1;            // Blamed: index into the cores
    QList<int> cpus;          // Blamed: that core's first logical CPU, for cosearch::Outcome
    QString summary;          // one line for the log and the resume question
};

/// `offsets`: the interrupted trial's offset per core (cores order).
Verdict judge(const QList<Record> &records, const QList<cosearch::Core> &cores, const QList<int> &offsets);

} // namespace coevidence
