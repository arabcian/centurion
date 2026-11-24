#include "costrategy.h"

#include <QJsonArray>
#include <QMap>
#include <algorithm>

namespace cosearch {

static constexpr int KERNELS = 3;  // centurion-cpustress --kernel 0..2: fft-sse, fft-avx, ntt
static constexpr int QUICK_SECONDS = 24;  // 12 s steady + 10 x 1.2 s boost/idle transitions
static constexpr int FULL_SECONDS = 60;

LevelSpec spec(Level l) {
    switch (l) {
    case Level::Fast: return {1, 2, 1, 1};
    case Level::Medium: return {2, 4, 2, 2};
    case Level::Long: return {3, 4, 3, 5};
    }
    return {1, 2, 1, 1};
}

QString name(Level l) {
    switch (l) {
    case Level::Fast: return QStringLiteral("fast");
    case Level::Medium: return QStringLiteral("medium");
    case Level::Long: return QStringLiteral("long");
    }
    return {};
}

std::optional<Level> levelFrom(const QString &s) {
    for (Level l : {Level::Fast, Level::Medium, Level::Long})
        if (name(l) == s) return l;
    return std::nullopt;
}

int Trial::seconds() const {
    int total = 0;
    for (const Test &t : tests) total += t.seconds;
    return total;
}

int Search::Walk::candidate(int start) const {
    if (done) return 1;
    int c = seeded ? pass - (coarse ? 5 : 1) : std::max(start, floor + 1);
    if (c <= floor) c = pass - 1;  // all-core: don't overshoot a known failing offset
    return c > floor ? c : 1;
}

int Search::Walk::coreCandidate(int limit, Tuning tuning) const {
    // floor is the least negative value known to fail (a failure in this search or a recorded
    // crash), or limit - 1 when there is none: nothing at or below it is ever probed.
    if (done) return 1;
    // The start failed: climb from the failure towards stock; the first pass is the result.
    if (up) {
        const int c = floor + tuning.step;
        return c < pass ? c : 1;
    }
    const int deepest = std::max(limit, floor + 1);
    if (pass <= deepest) return 1;
    // The first probe is the core's start (0: one step below stock); afterwards one step below the last pass.
    const int c = seeded || start >= 0 ? pass - tuning.step : start;
    return std::clamp(c, deepest, pass - 1);
}

Search::Search(Mode mode, Level level, int limit, QList<Core> cores, Tuning tuning)
    : mode_(mode), level_(level), limit_(limit), tuning_(tuning.valid() ? tuning : Tuning{}) {
    // Alternate between CCDs (slot order within each): consecutive turns heat different dies.
    QMap<int, QList<Core>> byCcd;
    for (const Core &c : cores) byCcd[c.ccd].append(c);
    for (auto &l : byCcd) std::stable_sort(l.begin(), l.end(), [](const Core &a, const Core &b) { return a.slot < b.slot; });
    for (int i = 0; cores_.size() < cores.size(); ++i)
        for (const auto &l : byCcd)
            if (i < l.size()) cores_.append(l[i]);
    all_.floor = limit_ - 1;
    if (mode_ == Mode::PerCore) {
        stage_ = Stage::PerCore;
        for (qsizetype i = 0; i < cores_.size(); ++i)
            per_.append(Walk{0, limit_ - 1, true, false, false});
    }
}

bool Search::perCoreSearching() const { return stage_ == Stage::PerCore; }

bool Search::startVerification(const QList<int> &values) {
    if (trials_ > 0 || values.size() != cores_.size()) return false;
    for (int v : values) if (v > 0 || v < -50) return false;
    if (mode_ == Mode::AllCore) {
        all_.pass = values.first();
        all_.coarse = false;
        all_.done = true;
    } else {
        for (int i = 0; i < cores_.size(); ++i) {
            per_[i].pass = values[i];
            per_[i].seeded = true;
            per_[i].done = true;
        }
    }
    stage_ = Stage::Confirm;
    return true;
}

bool Search::setTuning(Tuning tuning) {
    if (!tuning.valid()) return false;
    tuning_ = tuning;
    return true;
}

QStringList Search::cpustress(bool perCore, const QList<int> &cpus, int cycles, int kernel, bool quick) const {
    QStringList a{perCore ? QStringLiteral("percore") : QStringLiteral("allcore"), QStringLiteral("--load"), QStringLiteral("co"),
                  QStringLiteral("--cycles"), QString::number(cycles), QStringLiteral("--kernel"), QString::number(kernel % KERNELS)};
    if (quick) a << QStringLiteral("--seconds") << QString::number(QUICK_SECONDS);
    if (perCore) {
        QStringList l;
        for (int c : cpus) l << QString::number(c);
        a << QStringLiteral("--cpus") << l.join(',');
    }
    return a;
}

QList<int> Search::topCores() const {
    // Highest CPPC rank first on each CCD, then taken from the CCDs in turn.
    QMap<int, QList<Core>> byCcd;
    for (const Core &c : cores_) byCcd[c.ccd].append(c);
    for (auto &l : byCcd) std::stable_sort(l.begin(), l.end(), [](const Core &a, const Core &b) { return a.rank > b.rank; });
    QList<int> out;
    const int want = std::min<int>(spec(level_).topCores, cores_.size());
    for (int i = 0; out.size() < want; ++i)
        for (const auto &l : byCcd)
            if (i < l.size() && out.size() < want) out << l[i].cpus.first();
    return out;
}

Trial Search::allCoreTrial(int value) const {
    const LevelSpec s = spec(level_);
    const QList<int> top = topCores();
    Trial t;
    t.allCore = value;
    t.what = QStringLiteral("all-core %1").arg(value);
    t.tests << Test{cpustress(true, top, 1, trials_ + 1), int(top.size()) * FULL_SECONDS}
            << Test{cpustress(false, {}, s.trialCycles, trials_), s.trialCycles * FULL_SECONDS};
    return t;
}

Trial Search::perCoreTrial(int core, int value) const {
    const LevelSpec s = spec(level_);
    Trial t;
    // Test ONE changing core at a time. Previously found offsets remain recorded in values(),
    // but are NOT applied during isolated probes: this avoids false blame from an unstable peer.
    t.perCore = QList<int>(cores_.size(), 0);
    t.perCore[core] = value;
    const Core &c = cores_[core];
    t.what = QStringLiteral("CCD%1 core %2 (CPU %3) at %4").arg(c.ccd).arg(c.slot).arg(c.cpus.first()).arg(value);
    t.tests << Test{cpustress(true, {c.cpus.first()}, s.trialCycles, trials_, true),
                    s.trialCycles * QUICK_SECONDS};
    return t;
}

Trial Search::confirmTrial() const {
    const LevelSpec s = spec(level_);
    QList<int> firsts;
    for (const Core &c : cores_) firsts << c.cpus.first();
    Trial t;
    t.allCore = all_.pass;
    t.what = QStringLiteral("confirmation of all-core %1").arg(all_.pass);
    t.tests << Test{cpustress(true, firsts, s.confirmPerCore, trials_), s.confirmPerCore * int(firsts.size()) * FULL_SECONDS}
            << Test{cpustress(false, {}, s.confirmAllCore, trials_ + 1), s.confirmAllCore * FULL_SECONDS};
    return t;
}

Trial Search::verifyTrial() const {
    // The whole found curve stays applied for every part; each part stops at the first wrong result. Light
    // and heavy parts alternate so single cores are also tested on a hot package:
    // 1. Each undervolted core alone, lightest kernel (fft-sse) + all ten boost/idle patterns: with one core
    //    busy the shared voltage rail follows that core's request alone, at its highest boost - where a CO
    //    offset fails first. 2. `power` on every thread: dense FMA work up to the package power limit, the
    //    highest current, voltage droop and heat. 3. Every thread for two co cycles, fft-avx then ntt, right
    //    after it: current steps and a lone core boosting in turn on a hot CPU. 4. More single-core cycles
    //    with the other kernels (ntt, then fft-avx, ...). 5. The classic SSE load (different code) with random
    //    1-200 ms idle pauses, if there is room. 6. `power` again, for the rest of the 20 minutes.
    static constexpr int POWER_FIRST = 180, CO_ALL = 2 * FULL_SECONDS, POWER_LAST = 60;
    const QList<int> v = values();
    QList<int> cpus;
    for (int i = 0; i < cores_.size(); ++i) if (v[i] < 0) cpus << cores_[i].cpus.first();
    const int n = std::max<int>(1, cpus.size());
    QStringList list;
    for (int c : cpus) list << QString::number(c);
    auto power = [](int seconds) {
        return Test{{QStringLiteral("allcore"), QStringLiteral("--load"), QStringLiteral("power"), QStringLiteral("--seconds"),
                     QString::number(seconds)}, seconds};
    };
    Trial t;
    t.perCore = v;
    t.what = QStringLiteral("20-minute verification of the per-core set");
    const int pass = n * QUICK_SECONDS;  // one single-core co cycle on every undervolted core
    int single = VERIFY_SECONDS - POWER_FIRST - CO_ALL - POWER_LAST - pass;  // what is left for parts 4 and 5
    t.tests << Test{cpustress(true, cpus, 1, 0, true), pass} << power(POWER_FIRST) << Test{cpustress(false, {}, 2, 1), CO_ALL};
    if (const int cycles = std::min(100, std::max(0, single) / pass); cycles > 0) {
        t.tests << Test{cpustress(true, cpus, cycles, 2, true), cycles * pass};
        single -= cycles * pass;
    }
    if (const int each = std::max(0, single) / n; each >= 5) {
        t.tests << Test{{QStringLiteral("percore"), QStringLiteral("--load"), QStringLiteral("sse"), QStringLiteral("--seconds"),
                         QString::number(each), QStringLiteral("--transitions"), QStringLiteral("--cpus"), list.join(',')}, each * n};
        single -= each * n;
    }
    t.tests << power(POWER_LAST + std::max(0, single));
    return t;
}

QList<int> Search::values() const {
    QList<int> v;
    for (int i = 0; i < cores_.size(); ++i) v << (per_.isEmpty() ? all_.pass : per_[i].pass);
    return v;
}

void Search::finishAllCore() {
    // Per-core runs start directly in Stage::PerCore, not through this path.
    stage_ = all_.pass < 0 ? Stage::Confirm : Stage::Done;
}

std::optional<Trial> Search::next() {
    if (stage_ == Stage::AllCore) {
        const int c = all_.candidate(tuning_.allCoreStart);
        if (c <= 0) {
            cand_ = c; ++trials_; auto trial = allCoreTrial(c);
            trial.reason = !all_.seeded ? DecisionReason::InitialProbe : all_.coarse ? DecisionReason::CoarseDescent : DecisionReason::FineDescent;
            if (all_.pass < 0) trial.previousPass = all_.pass;
            if (all_.floor >= limit_) trial.failureBoundary = all_.floor;
            return trial;
        }
        finishAllCore();
    }
    if (stage_ == Stage::PerCore) {
        for (int k = 0; k < cores_.size(); ++k) {
            const int i = (turn_ + k) % cores_.size();
            if (per_[i].done) continue;
            const int c = per_[i].coreCandidate(limit_, tuning_);
            if (c > 0) { per_[i].done = true; continue; }
            tested_ = i;
            cand_ = c;
            turn_ = (i + 1) % cores_.size();
            ++trials_;
            auto trial = perCoreTrial(i, c);
            const auto &walk = per_[i];
            trial.reason = walk.up ? DecisionReason::FailedStartRecovery : !walk.seeded ? DecisionReason::InitialProbe : DecisionReason::StepDescent;
            if (walk.pass < 0) trial.previousPass = walk.pass;
            if (walk.floor >= limit_) trial.failureBoundary = walk.floor;
            return trial;
        }
        stage_ = Stage::Confirm;
    }
    if (stage_ == Stage::Confirm) {
        const QList<int> v = values();
        if (std::all_of(v.begin(), v.end(), [](int x) { return x == 0; })) { stage_ = Stage::Done; return std::nullopt; }
        ++trials_;
        auto trial = mode_ == Mode::PerCore ? verifyTrial() : confirmTrial();
        trial.reason = DecisionReason::FinalVerification;
        return trial;
    }
    return std::nullopt;
}

std::optional<Trial> Search::previewNext() const {
    Search copy = *this;
    return copy.next();
}

std::optional<Trial> Search::inFlight() const {
    if (stage_ == Stage::AllCore && cand_ < 0) return allCoreTrial(cand_);
    if (stage_ == Stage::PerCore && tested_ >= 0 && tested_ < cores_.size() && cand_ < 0)
        return perCoreTrial(tested_, cand_);
    if (stage_ == Stage::Confirm) return mode_ == Mode::PerCore ? verifyTrial() : confirmTrial();
    return std::nullopt;
}

bool Search::guardAgainstCrash(int ccd, int slot, const QList<int> &cpus, int failed) {
    if (mode_ != Mode::PerCore || cpus.isEmpty() || failed >= 0 || failed < limit_) return false;
    for (int i = 0; i < cores_.size(); ++i) {
        // Physical CCD/slot is the stable identity; Linux logical CPU IDs can shift when
        // SMT/parking configuration changes. Keeping the guard is safer than losing it.
        if (cores_[i].ccd != ccd || cores_[i].slot != slot) continue;
        Walk &w = per_[i];
        // Example: -25 triggered a reboot => -25 and below are forbidden; -24 is the deepest
        // candidate permitted, not automatically accepted as stable.
        w.floor = std::max(w.floor, failed);
        if (w.pass <= w.floor) w.pass = std::min(0, w.floor + 1);
        return true;
    }
    return false;
}

bool Search::guardAllCore(int failed) {
    if (mode_ != Mode::AllCore || failed >= 0 || failed < limit_) return false;
    all_.floor = std::max(all_.floor, failed);
    if (all_.pass <= all_.floor) all_.pass = std::min(0, all_.floor + 1);
    return true;
}

bool Search::exclude(int ccd, int slot) {
    if (mode_ != Mode::PerCore) return false;
    for (int i = 0; i < cores_.size(); ++i) {
        if (cores_[i].ccd != ccd || cores_[i].slot != slot) continue;
        per_[i].excluded = true;
        per_[i].done = true;
        per_[i].pass = 0;
        return true;
    }
    return false;
}

bool Search::setStart(int ccd, int slot, int value) {
    if (mode_ != Mode::PerCore) return false;
    for (int i = 0; i < cores_.size(); ++i) {
        if (cores_[i].ccd != ccd || cores_[i].slot != slot) continue;
        per_[i].start = std::clamp(value, limit_, 0);
        return true;
    }
    return false;
}

Search::CoreState Search::coreState(int i) const {
    if (i < 0 || i >= per_.size()) return {};
    const Walk &w = per_[i];
    return {w.start, w.pass, w.floor >= limit_ ? std::optional<int>(w.floor) : std::nullopt, w.done, w.excluded};
}

void Search::report(const Outcome &o) {
    switch (stage_) {
    case Stage::AllCore:
        all_.seeded = true;
        if (o.pass) all_.pass = cand_;
        else {
            all_.floor = std::max(all_.floor, std::min(-1, cand_ + (o.interrupted ? 1 : 0)));
            if (all_.pass <= all_.floor) all_.pass = std::min(0, all_.floor + 1);
            // A failed start keeps the steps of 5 from stock; after a pass, steps of 1.
            if (all_.pass < 0) all_.coarse = false;
        }
        break;
    case Stage::PerCore: {
        if (tested_ < 0) return;
        Walk &w = per_[tested_];
        w.seeded = true;
        if (o.pass) {
            w.pass = cand_;
            if (w.up) w.done = true;  // first pass above a failed start
        } else {
            w.floor = std::max(w.floor, cand_);
            // The result is the last pass; nothing more negative is tried. A failed start (nothing
            // passed yet) climbs from the failure towards stock instead.
            if (w.pass < 0) w.done = true;
            else w.up = true;
        }
        tested_ = -1;
        break;
    }
    case Stage::Confirm: {
        if (o.pass) { stage_ = Stage::Done; break; }
        if (mode_ == Mode::AllCore) {
            all_.pass = std::min(0, all_.pass + (o.interrupted ? 2 : 1));
            break;
        }
        // +1 for the cores the failure was traced to; when it names none, or names a core already at
        // stock (it cannot be the cause), every undervolted core.
        QList<int> hit;
        bool atStock = false;
        for (int i = 0; i < cores_.size(); ++i)
            for (int cpu : o.cpus)
                if (cores_[i].cpus.contains(cpu) && !hit.contains(i)) { hit << i; atStock |= per_[i].pass >= 0; }
        // A crash blamed on named cores by the system logs (coevidence) is believed once per search: if the
        // set crashes again, the logs missed the real culprit, and every undervolted core backs off.
        const bool distrust = o.interrupted && !hit.isEmpty() && !atStock && blamed_++ > 0;
        if (hit.isEmpty() || atStock || distrust) {
            hit.clear();
            for (int i = 0; i < cores_.size(); ++i) if (per_[i].pass < 0) hit << i;
        }
        for (int i : hit)
            per_[i].pass = std::min(0, per_[i].pass + (o.interrupted ? 2 : 1));
        break;
    }
    case Stage::Done: break;
    }
}

QString Search::stageText() const {
    switch (stage_) {
    case Stage::AllCore: return QStringLiteral("All-core descent (last pass %1)").arg(all_.pass);
    case Stage::PerCore: {
        const int left = int(std::count_if(per_.begin(), per_.end(), [](const Walk &w) { return !w.done; }));
        return QStringLiteral("Per-core search (%1/%2 cores pending; step %3)").arg(left).arg(per_.size()).arg(tuning_.step);
    }
    case Stage::Confirm: return mode_ == Mode::PerCore ? QStringLiteral("20-minute verification") : QStringLiteral("Confirmation");
    case Stage::Done: return QStringLiteral("Finished");
    }
    return {};
}

int Search::estimateMinutes(Mode mode, Level level, int cores, int limit, Tuning tuning, const QList<int> &starts) {
    const LevelSpec s = spec(level);
    if (mode == Mode::PerCore) {
        if (!tuning.valid()) tuning = {};
        // Worst case: every step from each core's start down to the limit passes.
        int probes = 0;
        for (int i = 0; i < cores; ++i) {
            const int first = std::clamp(starts.value(i, 0), limit, 0);
            probes += 1 + (first - limit + tuning.step - 1) / tuning.step;
        }
        const int seconds = probes * s.trialCycles * QUICK_SECONDS
                          + std::max(VERIFY_SECONDS, cores * QUICK_SECONDS + 360);
        return (seconds + 59) / 60;
    }
    const int allTrials = std::max(1, -limit * 2 / 3 / 5) + 3;
    return allTrials * (std::min(s.topCores, cores) + s.trialCycles)
           + cores * s.confirmPerCore + s.confirmAllCore;
}

static QJsonObject tuningJson(Tuning tuning) {
    return {{"all_core_start", tuning.allCoreStart}, {"step", tuning.step}};
}

/// Versions 3 / 4 had one per-core "initial" (it becomes every core's start, see load()) and a fixed
/// all-core start of -5; version 3 also had coarse / refine steps instead of one step (now: default).
static std::optional<Tuning> parseTuning(const QJsonObject &o, int version) {
    Tuning t;
    if (version >= 5) {
        if (!o.value("all_core_start").isDouble() || !o.value("step").isDouble()) return std::nullopt;
        t.allCoreStart = o.value("all_core_start").toInt(1);
        t.step = o.value("step").toInt(-1);
    } else {
        if (!o.value("initial").isDouble() || (version == 4 && !o.value("step").isDouble())) return std::nullopt;
        const int initial = o.value("initial").toInt(1);
        if (initial < -50 || initial > -1) return std::nullopt;
        if (version == 4) t.step = o.value("step").toInt(-1);
    }
    if (!t.valid()) return std::nullopt;
    return t;
}

static QJsonObject walkJson(int pass, int floor, bool coarse, bool done, bool seeded, bool excluded, int start, bool up) {
    return {{"pass", pass}, {"floor", floor}, {"coarse", coarse}, {"done", done}, {"seeded", seeded}, {"excluded", excluded},
            {"start", start}, {"up", up}};
}

QJsonObject Search::save() const {
    QJsonArray cores, per;
    for (const Core &c : cores_) {
        QJsonArray cpus;
        for (int x : c.cpus) cpus << x;
        cores << QJsonObject{{"ccd", c.ccd}, {"slot", c.slot}, {"cpus", cpus}, {"rank", c.rank}};
    }
    for (const Walk &w : per_) per << walkJson(w.pass, w.floor, w.coarse, w.done, w.seeded, w.excluded, w.start, w.up);
    static const char *stages[] = {"all", "per", "confirm", "done"};
    return {{"version", 5}, {"mode", mode_ == Mode::AllCore ? "all" : "per"}, {"level", name(level_)}, {"limit", limit_},
            {"tuning", tuningJson(tuning_)},
            {"cores", cores}, {"stage", stages[int(stage_)]}, {"all", walkJson(all_.pass, all_.floor, all_.coarse, all_.done, all_.seeded, false, 0, false)},
            {"per", per}, {"turn", turn_}, {"tested", tested_}, {"trials", trials_}, {"cand", cand_}, {"blamed", blamed_}};
}

std::optional<Search> Search::load(const QJsonObject &o) {
    const auto level = levelFrom(o.value("level").toString());
    const QString mode = o.value("mode").toString();
    const int version = o.value("version").toInt();
    // Versions 3 (coarse/refine bracket search) and 4 resume under the version 5 rules; older ones cannot.
    if (version < 3 || version > 5 || !level || (mode != "all" && mode != "per")) return std::nullopt;
    const auto tuning = parseTuning(o.value("tuning").toObject(), version);
    if (!tuning) return std::nullopt;
    QList<Core> cores;
    for (const auto &v : o.value("cores").toArray()) {
        const QJsonObject c = v.toObject();
        Core k{c.value("ccd").toInt(-1), c.value("slot").toInt(-1), {}, c.value("rank").toInt(-1)};
        for (const auto &x : c.value("cpus").toArray()) k.cpus << x.toInt(-1);
        if (k.ccd < 0 || k.slot < 0 || k.cpus.isEmpty() || k.cpus.contains(-1)) return std::nullopt;
        cores << k;
    }
    const int limit = o.value("limit").toInt(1);
    if (cores.isEmpty() || limit > -1 || limit < -50 || cores.size() > 512) return std::nullopt;
    Search s(mode == "all" ? Mode::AllCore : Mode::PerCore, *level, limit, {}, *tuning);
    s.cores_ = cores;  // already in test order
    auto walk = [](const QJsonObject &w) { return Walk{w.value("pass").toInt(), w.value("floor").toInt(), w.value("coarse").toBool(),
                                                      w.value("done").toBool(), w.value("seeded").toBool(), w.value("excluded").toBool(),
                                                      w.value("start").toInt(1), w.value("up").toBool()}; };
    s.all_ = walk(o.value("all").toObject());
    // Before version 4 the all-core walk had no "seeded": it had started once it passed or failed anything.
    if (version < 5) s.all_.seeded = s.all_.pass < 0 || s.all_.floor >= limit;
    // The all-core walk is applied as it is (its pass in the confirmation, pass - 5 / floor + 1 as candidates):
    // the same bounds as a per-core walk, or a damaged journal could go past the limit or above stock.
    if (s.all_.pass > 0 || s.all_.pass < limit || s.all_.floor < limit - 1 || s.all_.floor >= s.all_.pass) return std::nullopt;
    for (const auto &v : o.value("per").toArray()) {
        Walk w = walk(v.toObject());
        if (version < 5) {
            w.start = std::clamp(o.value("tuning").toObject().value("initial").toInt(), limit, 0);
            w.up = false;
            // Version 3 kept refining after a failure. A core with a recorded failure and a pass
            // keeps that pass as its result: nothing more negative is tried under the new rules.
            if (version == 3 && w.floor >= limit && w.pass < 0) w.done = true;
            // A failed start (version 4: then descending from stock) climbs from the failure now.
            if (w.seeded && !w.done && w.pass == 0 && w.floor >= limit) w.up = true;
        }
        s.per_ << w;
    }
    const QString st = o.value("stage").toString();
    if (st != "all" && st != "per" && st != "confirm" && st != "done") return std::nullopt;
    if ((mode == "per" && st == "all") || (mode == "all" && st == "per")) return std::nullopt;
    s.stage_ = st == "all" ? Stage::AllCore : st == "per" ? Stage::PerCore : st == "confirm" ? Stage::Confirm : Stage::Done;
    if (!s.per_.isEmpty() && s.per_.size() != cores.size()) return std::nullopt;
    if (s.mode_ == Mode::PerCore && s.per_.size() != cores.size()) return std::nullopt;
    for (const Walk &w : s.per_) {
        if (w.pass > 0 || w.pass < limit || w.floor < limit - 1 || w.floor >= w.pass) return std::nullopt;
        if (w.excluded && (w.pass != 0 || !w.done)) return std::nullopt;
        if (w.start < limit || w.start > 0 || (w.up && !w.done && w.pass != 0)) return std::nullopt;
    }
    if ((s.stage_ == Stage::PerCore || (s.stage_ == Stage::Confirm && s.mode_ == Mode::PerCore)) && s.per_.isEmpty()) return std::nullopt;
    s.turn_ = std::clamp(o.value("turn").toInt(), 0, int(cores.size()) - 1);
    s.tested_ = o.value("tested").toInt(-1);
    if (s.tested_ >= cores.size() || s.tested_ < -1) return std::nullopt;
    s.trials_ = o.value("trials").toInt();
    if (s.trials_ < 0 || s.trials_ > 100000) return std::nullopt;
    s.cand_ = o.value("cand").toInt();
    s.blamed_ = std::clamp(o.value("blamed").toInt(), 0, 1000);
    if (s.cand_ < limit || s.cand_ > 0) return std::nullopt;
    return s;
}

} // namespace cosearch
