#pragma once
// Curve Optimizer search strategy (no widgets, no I/O): decides which offsets to try and how to test them;
// CoSearchDialog applies them, runs centurion-cpustress and reports back.
//
// All-core: from a configurable start (-5), down in steps of 5 until a trial fails or the limit is reached,
// then in steps of 1 between the last pass and that failure, then a confirmation at the result (if it fails:
// +1 and confirm again). A failed start keeps the steps of 5 from stock down to that failure.
// A trial tests the best-ranked cores alone first (single-core load is where the boost and the offset's
// effect are highest) and then every core at once.
//
// Per-core: each core is probed alone (every other core at stock), round-robin over the cores. Each core
// has its own start value (typed in, or taken from a saved Ryzen Curve profile) and descends by a
// configurable step (1). The FIRST failure after a pass ends that core: its result is the last value that
// passed, and nothing as negative as the failed value (or more) is ever tried on it again. If the start
// itself fails, the core climbs back towards stock by the step from the failure; the first value that
// passes is its result. Excluded cores stay at stock.
// The short probes include steady load and all ten boost/idle transitions. When every core is done, the
// whole set is verified for 20 minutes (verifyTrial), including all-core loads up to the power limit. Correlated verification failures make the implicated
// core(s) less negative; untraceable failures back off all undervolted cores; then it runs again.
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

/// Per-core tuning; editable before starting or while a run is in progress. The new values
/// take effect on the NEXT selected candidate; in-flight trials and durable crash guards stay intact.
struct Tuning {
    int allCoreStart = -5;  // all-core: first offset tried (per-core cores have their own start)
    int step = 1;           // per-core: descent after each pass / climb after a failed start, in CO points
    bool valid() const { return allCoreStart >= -50 && allCoreStart <= -1 && step >= 1 && step <= 10; }
    bool operator==(const Tuning &) const = default;
};

/// How much testing each level buys. One cycle = 60 s of centurion-cpustress --load co.
struct LevelSpec {
    int trialCycles;     // per trial: cycles of the all-core test, or of the core under test
    int topCores;        // all-core trials: how many best-ranked cores are tested alone (one cycle each)
    int confirmPerCore;  // confirmation: cycles for every core alone
    int confirmAllCore;  // confirmation: cycles with every core loaded
};
LevelSpec spec(Level l);
/// Per-core: length of the final verification of the whole set (more with over 35 cores, see verifyTrial).
inline constexpr int VERIFY_SECONDS = 1200;
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
    int seconds = 0;  // expected test length, without startup/reference overhead
};

enum class DecisionReason { Unknown, InitialProbe, CoarseDescent, FineDescent, StepDescent, FailedStartRecovery, FinalVerification, Replay };
/// What to try next: the offsets to apply, then the tests to run in order (stop at the first failure).
struct Trial {
    std::optional<int> allCore;  // set: one offset on every core
    QList<int> perCore;          // otherwise: one offset per core, in cores() order
    QList<Test> tests;
    QString what;                // for the log and the crash note
    int seconds() const;
    DecisionReason reason = DecisionReason::Unknown;
    std::optional<int> previousPass, failureBoundary; // evidence/guard bound, not proof of stability
};

struct Outcome {
    bool pass = false;
    QList<int> cpus;  // logical CPUs a failure was traced to; empty = unknown (kernel event, crash)
    bool interrupted = false; // hard reboot / interrupted trial: apply additional safety margin
};

class Search {
public:
    Search(Mode mode, Level level, int limit, QList<Core> cores, Tuning tuning = {});

    /// Change only FUTURE candidates; the active test and its replay signature never change.
    bool setTuning(Tuning tuning);
    Tuning tuning() const { return tuning_; }
    bool perCoreSearching() const;

    /// The next trial, or nullopt when the search is over (see result()).
    std::optional<Trial> next();
    /// Exact next dispatch on a copy; never advances the live search.
    std::optional<Trial> previewNext() const;
    struct WorkEstimate { qint64 seconds = 0; int trials = 0, tests = 0; };
    // Conditional scenario, not a statistical interval or worst-case bound.
    // Simulates only on a copy; pending means this is an after-next() state.
    // The current trial is excluded from the returned work.
    std::optional<WorkEstimate> remainingIfPass(bool pending, bool failPending = false) const;
    /// The result of the trial last returned by next().
    void report(const Outcome &o);
    /// Reconstruct the CURRENT in-flight candidate from an after-next() snapshot, without advancing.
    std::optional<Trial> inFlight() const;
    /// Never probe `failed` or anything more negative on this physical core (a recorded crash).
    bool guardAgainstCrash(int ccd, int slot, const QList<int> &cpus, int failed);
    /// All-core: never try `failed` or anything more negative on every core at once (a recorded crash).
    bool guardAllCore(int failed);
    /// Per-core: leave this physical core at stock for the whole search. Call before the first next().
    bool exclude(int ccd, int slot);
    /// Per-core: this core's first probe (clamped to limit..0; 0 = step down from stock). Before next().
    bool setStart(int ccd, int slot, int value);
    /// Verification only (a saved profile): skip the walk; `values` (cores() order, -50..0) go straight to
    /// the final verification / confirmation, which backs failing cores off as usual. Before next().
    /// All-core mode takes values[0] for every core. False on a bad list or after next().
    bool startVerification(const QList<int> &values);

    /// Per-core progress of one core (cores() order), for display.
    struct CoreState {
        int start = 0;              // configured first probe
        int pass = 0;               // last value that passed (0 = stock)
        std::optional<int> failed;  // least negative value known bad (failure or crash guard)
        bool done = false, excluded = false;
    };
    CoreState coreState(int i) const;

    bool finished() const { return stage_ == Stage::Done; }
    /// In the final verification / confirmation (every searched value applied at once).
    bool confirming() const { return stage_ == Stage::Confirm; }
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
    /// Per-core: `starts` = start of each tested core (missing: 0).
    static int estimateMinutes(Mode mode, Level level, int cores, int limit, Tuning tuning = {}, const QList<int> &starts = {});

private:
    enum class Stage { AllCore, PerCore, Confirm, Done };
    struct Walk {
        int pass = 0;          // last observed passing value; 0 is assumed stock
        int floor = 0;         // nearest failing value, or limit - 1 sentinel
        bool coarse = true;    // all-core only: steps of 5, then 1
        bool done = false;
        bool seeded = false;   // per-core: whether the configured initial probe has been made
        bool excluded = false; // per-core: left at stock by the user
        int start = 0;         // per-core: first probe; 0 = start one step below stock
        bool up = false;       // per-core: the start failed, climbing towards stock
        int candidate(int start) const;  // all-core descent

        int coreCandidate(int limit, Tuning tuning) const;
    };
    Trial allCoreTrial(int value) const;
    Trial perCoreTrial(int core, int value) const;
    Trial confirmTrial() const;  // all-core
    Trial verifyTrial() const;   // per-core
    QStringList cpustress(bool perCore, const QList<int> &cpus, int cycles, int kernel, bool quick = false) const;
    QList<int> topCores() const;
    void finishAllCore();

    Mode mode_;
    Level level_;
    int limit_;
    Tuning tuning_;
    QList<Core> cores_;
    Stage stage_ = Stage::AllCore;
    Walk all_;
    QList<Walk> per_;
    int turn_ = 0;           // per-core: index of the core whose turn is next
    int tested_ = -1;        // per-core: core of the trial in flight
    int trials_ = 0;         // kernel rotation
    int cand_ = 0;           // value of the trial in flight
    int blamed_ = 0;         // final-stage crashes the logs blamed on named cores (only the first is believed)
};

} // namespace cosearch
