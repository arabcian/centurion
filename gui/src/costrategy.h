#pragma once
// Curve Optimizer search strategy (no widgets, no I/O): decides which offsets to try and how to test them;
// CoSearchDialog applies them, runs centurion-cpustress and reports back.
//
// All-core: down from 0 in steps of 5 until a trial fails or the limit is reached, then in steps of 1 between
// the last pass and that failure, then a confirmation at the result (if it fails: +1 and confirm again).
// A trial tests the best-ranked cores alone first (single-core load is where the boost and the offset's
// effect are highest) and then every core at once.
//
// Per-core: the all-core search (without its confirmation) gives a floor every core is known to pass. Then
// each core goes down from there on its own, in turn alternating between CCDs: the core under test gets the
// candidate, every other core keeps its last passed value, and only that core is loaded, so it alone sets
// the shared voltage rail. A failure always counts against the core under test (the only thing that
// changed). A final confirmation runs every core alone and then all together with the whole set; a core
// that fails there gets +1 (every undervolted core when a failure names no CPU) until it passes; a search
// that climbs back to 0 everywhere ends with stock.
//
// The state is a plain JSON snapshot (save / load), so a search interrupted by a crash can resume: the trial
// that was running counts as failed.
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <optional>

namespace cosearch {

enum class Mode { AllCore, PerCore };
enum class Level { Fast, Medium, Long };

/// How much testing each level buys. One cycle = 60 s of centurion-cpustress --load co.
struct LevelSpec {
    int trialCycles;     // per trial: cycles of the all-core test, or of the core under test
    int topCores;        // all-core trials: how many best-ranked cores are tested alone (one cycle each)
    int confirmPerCore;  // confirmation: cycles for every core alone
    int confirmAllCore;  // confirmation: cycles with every core loaded
};
LevelSpec spec(Level l);
QString name(Level l);
std::optional<Level> levelFrom(const QString &s);

/// A physical core: SMU slot (CCD, core index on it), its logical CPUs, CPPC rank (-1 = unknown).
struct Core {
    int ccd = 0, slot = 0;
    QList<int> cpus;
    int rank = -1;
    bool operator==(const Core &) const = default;
};

/// One centurion-cpustress run.
struct Test {
    QStringList args;
    int minutes = 0;  // expected length
};

/// What to try next: the offsets to apply, then the tests to run in order (stop at the first failure).
struct Trial {
    std::optional<int> allCore;  // set: one offset on every core
    QList<int> perCore;          // otherwise: one offset per core, in cores() order
    QList<Test> tests;
    QString what;                // for the log and the crash note
    int minutes() const;
};

struct Outcome {
    bool pass = false;
    QList<int> cpus;  // logical CPUs a failure was traced to; empty = unknown (kernel event, crash)
};

class Search {
public:
    Search(Mode mode, Level level, int limit, QList<Core> cores);

    /// The next trial, or nullopt when the search is over (see result()).
    std::optional<Trial> next();
    /// The result of the trial last returned by next().
    void report(const Outcome &o);

    bool finished() const { return stage_ == Stage::Done; }
    Mode mode() const { return mode_; }
    Level level() const { return level_; }
    int limit() const { return limit_; }
    /// Cores in test order (CCDs alternating).
    const QList<Core> &cores() const { return cores_; }
    /// Current offsets: last passed (during the search) / confirmed (when finished), one per core.
    QList<int> values() const;
    QString stageText() const;

    QJsonObject save() const;
    static std::optional<Search> load(const QJsonObject &o);

    /// Rough length of a whole search, for the dialog.
    static int estimateMinutes(Mode mode, Level level, int cores, int limit);

private:
    enum class Stage { AllCore, PerCore, Confirm, Done };
    struct Walk {             // one descent: all-core, or one core's
        int pass = 0;         // last value that passed (0 = stock, never tested)
        int floor = 0;        // most negative allowed minus one, or the highest value that failed
        bool coarse = true;   // steps of 5 until the first failure, then 1
        bool done = false;
        int candidate() const;  // next value to try, or 1 when there is none
    };
    Trial allCoreTrial(int value);
    Trial perCoreTrial(int core, int value);
    Trial confirmTrial();
    QStringList cpustress(bool perCore, const QList<int> &cpus, int cycles, int kernel) const;
    QList<int> topCores() const;
    void finishAllCore();

    Mode mode_;
    Level level_;
    int limit_;
    QList<Core> cores_;
    Stage stage_ = Stage::AllCore;
    Walk all_;
    QList<Walk> per_;
    int turn_ = 0;           // per-core: index of the core whose turn is next
    int tested_ = -1;        // per-core: core of the trial in flight
    int trials_ = 0;         // kernel rotation
    int cand_ = 0;           // value of the trial in flight
};

} // namespace cosearch
