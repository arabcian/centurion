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

int Search::Walk::candidate() const {
    if (done) return 1;
    int c = pass - (coarse ? 5 : 1);
    if (c <= floor) c = pass - 1;  // all-core: don't overshoot a known failing offset
    return c > floor ? c : 1;
}

int Search::Walk::coreCandidate(int limit) const {
    if (done || pass <= limit || pass - floor <= 1) return 1;
    // The first per-core write is -15, not -5. With a shallower user limit, respect that limit.
    if (!seeded) {
        // A persisted crash guard can make -15 unsafe too: never cross the failure boundary.
        const int initial = std::max({limit, -15, floor + 1});
        return initial < pass ? initial : 1;
    }
    if (coarse) {
        const int candidate = std::max(limit, pass - 5);
        if (candidate < pass && candidate > floor) return candidate;
        // At a persisted crash boundary, bisect instead of jumping past it.
    }
    // pass is closer to zero than floor; never retest a known pass or fail.
    // Integer midpoint yields logarithmic refinement, including when the initial -15 fails.
    return floor + (pass - floor + 1) / 2;
}

Search::Search(Mode mode, Level level, int limit, QList<Core> cores) : mode_(mode), level_(level), limit_(limit) {
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
    if (mode_ == Mode::AllCore) t.allCore = all_.pass;
    else t.perCore = values();
    t.what = mode_ == Mode::AllCore ? QStringLiteral("confirmation of all-core %1").arg(all_.pass) : QStringLiteral("confirmation of the per-core set");
    t.tests << Test{cpustress(true, firsts, s.confirmPerCore, trials_), s.confirmPerCore * int(firsts.size()) * FULL_SECONDS}
            << Test{cpustress(false, {}, s.confirmAllCore, trials_ + 1), s.confirmAllCore * FULL_SECONDS};
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
        const int c = all_.candidate();
        if (c <= 0) { cand_ = c; ++trials_; return allCoreTrial(c); }
        finishAllCore();
    }
    if (stage_ == Stage::PerCore) {
        for (int k = 0; k < cores_.size(); ++k) {
            const int i = (turn_ + k) % cores_.size();
            if (per_[i].done) continue;
            const int c = per_[i].coreCandidate(limit_);
            if (c > 0) { per_[i].done = true; continue; }
            tested_ = i;
            cand_ = c;
            turn_ = (i + 1) % cores_.size();
            ++trials_;
            return perCoreTrial(i, c);
        }
        stage_ = Stage::Confirm;
    }
    if (stage_ == Stage::Confirm) {
        const QList<int> v = values();
        if (std::all_of(v.begin(), v.end(), [](int x) { return x == 0; })) { stage_ = Stage::Done; return std::nullopt; }
        ++trials_;
        return confirmTrial();
    }
    return std::nullopt;
}

std::optional<Trial> Search::inFlight() const {
    if (stage_ == Stage::AllCore && cand_ < 0) return allCoreTrial(cand_);
    if (stage_ == Stage::PerCore && tested_ >= 0 && tested_ < cores_.size() && cand_ < 0)
        return perCoreTrial(tested_, cand_);
    if (stage_ == Stage::Confirm) return confirmTrial();
    return std::nullopt;
}

bool Search::guardAgainstCrash(int ccd, int slot, const QList<int> &cpus, int failed) {
    if (mode_ != Mode::PerCore || cpus.isEmpty() || failed >= 0 || failed < limit_) return false;
    for (int i = 0; i < cores_.size(); ++i) {
        // Physical CCD/slot is the stable identity; Linux logical CPU IDs can shift when
        // SMT/parking configuration changes. Keeping the guard is safer than losing it.
        if (cores_[i].ccd != ccd || cores_[i].slot != slot) continue;
        Walk &w = per_[i];
        // Example: -25 triggered a reboot => -25 and -24 are forbidden; -23 is the deepest
        // candidate permitted, not automatically accepted as stable.
        w.floor = std::max(w.floor, std::min(-1, failed + 1));
        if (w.pass <= w.floor) w.pass = std::min(0, w.floor + 1);
        w.done = false;
        return true;
    }
    return false;
}

void Search::report(const Outcome &o) {
    switch (stage_) {
    case Stage::AllCore:
        if (o.pass) all_.pass = cand_;
        else {
            all_.floor = std::max(all_.floor, std::min(-1, cand_ + (o.interrupted ? 1 : 0)));
            if (all_.pass <= all_.floor) all_.pass = std::min(0, all_.floor + 1);
            all_.coarse = false;
        }
        break;
    case Stage::PerCore: {
        if (tested_ < 0) return;
        Walk &w = per_[tested_];
        w.seeded = true;
        if (o.pass) w.pass = cand_;
        else {
            w.floor = std::max(w.floor, std::min(-1, cand_ + (o.interrupted ? 1 : 0)));
            if (w.pass <= w.floor) w.pass = std::min(0, w.floor + 1);
            w.coarse = false;
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
        if (hit.isEmpty() || atStock) {
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
        return QStringLiteral("Per-core quick search (%1 of %2 cores remaining; 60 s/core validation follows)")
            .arg(left).arg(per_.size());
    }
    case Stage::Confirm: return QStringLiteral("Confirmation");
    case Stage::Done: return QStringLiteral("Finished");
    }
    return {};
}

int Search::estimateMinutes(Mode mode, Level level, int cores, int limit) {
    const LevelSpec s = spec(level);
    if (mode == Mode::PerCore) {
        // Roughly: -15, then 5-point probes toward the limit, and ~2 binary bracket probes.
        // Hardware can require more/fewer probes and repeated confirmations.
        const int probes = 1 + std::max(0, (-limit - 15 + 4) / 5) + 2;
        const int seconds = cores * probes * s.trialCycles * QUICK_SECONDS
                          + cores * s.confirmPerCore * FULL_SECONDS + s.confirmAllCore * FULL_SECONDS;
        return (seconds + 59) / 60;
    }
    const int allTrials = std::max(1, -limit * 2 / 3 / 5) + 3;
    return allTrials * (std::min(s.topCores, cores) + s.trialCycles)
           + cores * s.confirmPerCore + s.confirmAllCore;
}

static QJsonObject walkJson(int pass, int floor, bool coarse, bool done, bool seeded) {
    return {{"pass", pass}, {"floor", floor}, {"coarse", coarse}, {"done", done}, {"seeded", seeded}};
}

QJsonObject Search::save() const {
    QJsonArray cores, per;
    for (const Core &c : cores_) {
        QJsonArray cpus;
        for (int x : c.cpus) cpus << x;
        cores << QJsonObject{{"ccd", c.ccd}, {"slot", c.slot}, {"cpus", cpus}, {"rank", c.rank}};
    }
    for (const Walk &w : per_) per << walkJson(w.pass, w.floor, w.coarse, w.done, w.seeded);
    static const char *stages[] = {"all", "per", "confirm", "done"};
    return {{"version", 2}, {"mode", mode_ == Mode::AllCore ? "all" : "per"}, {"level", name(level_)}, {"limit", limit_},
            {"cores", cores}, {"stage", stages[int(stage_)]}, {"all", walkJson(all_.pass, all_.floor, all_.coarse, all_.done, all_.seeded)},
            {"per", per}, {"turn", turn_}, {"tested", tested_}, {"trials", trials_}, {"cand", cand_}};
}

std::optional<Search> Search::load(const QJsonObject &o) {
    const auto level = levelFrom(o.value("level").toString());
    const QString mode = o.value("mode").toString();
    if (o.value("version").toInt() != 2 || !level || (mode != "all" && mode != "per")) return std::nullopt;
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
    Search s(mode == "all" ? Mode::AllCore : Mode::PerCore, *level, limit, {});
    s.cores_ = cores;  // already in test order
    auto walk = [](const QJsonObject &w) { return Walk{w.value("pass").toInt(), w.value("floor").toInt(), w.value("coarse").toBool(),
                                                      w.value("done").toBool(), w.value("seeded").toBool()}; };
    s.all_ = walk(o.value("all").toObject());
    for (const auto &v : o.value("per").toArray()) s.per_ << walk(v.toObject());
    const QString st = o.value("stage").toString();
    if (st != "all" && st != "per" && st != "confirm" && st != "done") return std::nullopt;
    if ((mode == "per" && st == "all") || (mode == "all" && st == "per")) return std::nullopt;
    s.stage_ = st == "all" ? Stage::AllCore : st == "per" ? Stage::PerCore : st == "confirm" ? Stage::Confirm : Stage::Done;
    if (!s.per_.isEmpty() && s.per_.size() != cores.size()) return std::nullopt;
    if (s.mode_ == Mode::PerCore && s.per_.size() != cores.size()) return std::nullopt;
    for (const Walk &w : s.per_) {
        if (w.pass > 0 || w.pass < limit || w.floor < limit - 1 || w.floor >= w.pass) return std::nullopt;
    }
    if ((s.stage_ == Stage::PerCore || (s.stage_ == Stage::Confirm && s.mode_ == Mode::PerCore)) && s.per_.isEmpty()) return std::nullopt;
    s.turn_ = std::clamp(o.value("turn").toInt(), 0, int(cores.size()) - 1);
    s.tested_ = o.value("tested").toInt(-1);
    if (s.tested_ >= cores.size() || s.tested_ < -1) return std::nullopt;
    s.trials_ = o.value("trials").toInt();
    if (s.trials_ < 0 || s.trials_ > 100000) return std::nullopt;
    s.cand_ = o.value("cand").toInt();
    if (s.cand_ < limit || s.cand_ > 0) return std::nullopt;
    return s;
}

} // namespace cosearch
