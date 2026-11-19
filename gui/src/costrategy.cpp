#include "costrategy.h"

#include <QJsonArray>
#include <QMap>
#include <algorithm>

namespace cosearch {

static constexpr int KERNELS = 3;  // centurion-cpustress --kernel 0..2: fft-sse, fft-avx, ntt

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

int Trial::minutes() const {
    int m = 0;
    for (const Test &t : tests) m += t.minutes;
    return m;
}

int Search::Walk::candidate() const {
    if (done) return 1;
    int c = pass - (coarse ? 5 : 1);
    if (c <= floor) c = pass - 1;  // a coarse step would overshoot the floor: go on in steps of 1
    return c > floor ? c : 1;
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
}

QStringList Search::cpustress(bool perCore, const QList<int> &cpus, int cycles, int kernel) const {
    QStringList a{perCore ? QStringLiteral("percore") : QStringLiteral("allcore"), QStringLiteral("--load"), QStringLiteral("co"),
                  QStringLiteral("--cycles"), QString::number(cycles), QStringLiteral("--kernel"), QString::number(kernel % KERNELS)};
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

Trial Search::allCoreTrial(int value) {
    const LevelSpec s = spec(level_);
    const QList<int> top = topCores();
    Trial t;
    t.allCore = value;
    t.what = QStringLiteral("all-core %1").arg(value);
    t.tests << Test{cpustress(true, top, 1, trials_ + 1), int(top.size())}
            << Test{cpustress(false, {}, s.trialCycles, trials_), s.trialCycles};
    return t;
}

Trial Search::perCoreTrial(int core, int value) {
    const LevelSpec s = spec(level_);
    Trial t;
    t.perCore = values();
    t.perCore[core] = value;
    const Core &c = cores_[core];
    t.what = QStringLiteral("CCD%1 core %2 (CPU %3) at %4").arg(c.ccd).arg(c.slot).arg(c.cpus.first()).arg(value);
    t.tests << Test{cpustress(true, {c.cpus.first()}, s.trialCycles, trials_), s.trialCycles};
    return t;
}

Trial Search::confirmTrial() {
    const LevelSpec s = spec(level_);
    QList<int> firsts;
    for (const Core &c : cores_) firsts << c.cpus.first();
    Trial t;
    if (mode_ == Mode::AllCore) t.allCore = all_.pass;
    else t.perCore = values();
    t.what = mode_ == Mode::AllCore ? QStringLiteral("confirmation of all-core %1").arg(all_.pass) : QStringLiteral("confirmation of the per-core set");
    t.tests << Test{cpustress(true, firsts, s.confirmPerCore, trials_), s.confirmPerCore * int(firsts.size())}
            << Test{cpustress(false, {}, s.confirmAllCore, trials_ + 1), s.confirmAllCore};
    return t;
}

QList<int> Search::values() const {
    QList<int> v;
    for (int i = 0; i < cores_.size(); ++i) v << (per_.isEmpty() ? all_.pass : per_[i].pass);
    return v;
}

void Search::finishAllCore() {
    if (mode_ == Mode::AllCore) {
        stage_ = all_.pass < 0 ? Stage::Confirm : Stage::Done;
        return;
    }
    // Every core starts from the all-core result; its own floor is the limit, since a value that failed
    // with all cores at it says nothing about one core alone.
    per_.clear();
    for (int i = 0; i < cores_.size(); ++i) per_.append(Walk{all_.pass, limit_ - 1, true, false});
    stage_ = Stage::PerCore;
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
            const int c = per_[i].candidate();
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

void Search::report(const Outcome &o) {
    switch (stage_) {
    case Stage::AllCore:
        if (o.pass) all_.pass = cand_;
        else { all_.floor = cand_; all_.coarse = false; }
        break;
    case Stage::PerCore: {
        if (tested_ < 0) return;
        Walk &w = per_[tested_];
        if (o.pass) w.pass = cand_;
        else { w.floor = cand_; w.coarse = false; }
        tested_ = -1;
        break;
    }
    case Stage::Confirm: {
        if (o.pass) { stage_ = Stage::Done; break; }
        if (mode_ == Mode::AllCore) { ++all_.pass; break; }
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
        for (int i : hit) ++per_[i].pass;  // a confirmation only runs while some core is below 0
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
        return QStringLiteral("Per-core descent (%1 of %2 cores still going down)").arg(left).arg(per_.size());
    }
    case Stage::Confirm: return QStringLiteral("Confirmation");
    case Stage::Done: return QStringLiteral("Finished");
    }
    return {};
}

int Search::estimateMinutes(Mode mode, Level level, int cores, int limit) {
    const LevelSpec s = spec(level);
    // About: coarse steps to roughly two thirds of the limit, then three fine steps.
    const int allTrials = std::max(1, -limit * 2 / 3 / 5) + 3;
    int m = allTrials * (std::min(s.topCores, cores) + s.trialCycles);
    if (mode == Mode::PerCore) m += cores * 4 * s.trialCycles;
    return m + cores * s.confirmPerCore + s.confirmAllCore;
}

static QJsonObject walkJson(int pass, int floor, bool coarse, bool done) {
    return {{"pass", pass}, {"floor", floor}, {"coarse", coarse}, {"done", done}};
}

QJsonObject Search::save() const {
    QJsonArray cores, per;
    for (const Core &c : cores_) {
        QJsonArray cpus;
        for (int x : c.cpus) cpus << x;
        cores << QJsonObject{{"ccd", c.ccd}, {"slot", c.slot}, {"cpus", cpus}, {"rank", c.rank}};
    }
    for (const Walk &w : per_) per << walkJson(w.pass, w.floor, w.coarse, w.done);
    static const char *stages[] = {"all", "per", "confirm", "done"};
    return {{"version", 1}, {"mode", mode_ == Mode::AllCore ? "all" : "per"}, {"level", name(level_)}, {"limit", limit_},
            {"cores", cores}, {"stage", stages[int(stage_)]}, {"all", walkJson(all_.pass, all_.floor, all_.coarse, all_.done)},
            {"per", per}, {"turn", turn_}, {"tested", tested_}, {"trials", trials_}, {"cand", cand_}};
}

std::optional<Search> Search::load(const QJsonObject &o) {
    const auto level = levelFrom(o.value("level").toString());
    const QString mode = o.value("mode").toString();
    if (o.value("version").toInt() != 1 || !level || (mode != "all" && mode != "per")) return std::nullopt;
    QList<Core> cores;
    for (const auto &v : o.value("cores").toArray()) {
        const QJsonObject c = v.toObject();
        Core k{c.value("ccd").toInt(-1), c.value("slot").toInt(-1), {}, c.value("rank").toInt(-1)};
        for (const auto &x : c.value("cpus").toArray()) k.cpus << x.toInt(-1);
        if (k.ccd < 0 || k.slot < 0 || k.cpus.isEmpty() || k.cpus.contains(-1)) return std::nullopt;
        cores << k;
    }
    const int limit = o.value("limit").toInt(1);
    if (cores.isEmpty() || limit > 0) return std::nullopt;
    Search s(mode == "all" ? Mode::AllCore : Mode::PerCore, *level, limit, {});
    s.cores_ = cores;  // already in test order
    auto walk = [](const QJsonObject &w) { return Walk{w.value("pass").toInt(), w.value("floor").toInt(), w.value("coarse").toBool(), w.value("done").toBool()}; };
    s.all_ = walk(o.value("all").toObject());
    for (const auto &v : o.value("per").toArray()) s.per_ << walk(v.toObject());
    const QString st = o.value("stage").toString();
    s.stage_ = st == "all" ? Stage::AllCore : st == "per" ? Stage::PerCore : st == "confirm" ? Stage::Confirm : Stage::Done;
    if (!s.per_.isEmpty() && s.per_.size() != cores.size()) return std::nullopt;
    if ((s.stage_ == Stage::PerCore || (s.stage_ == Stage::Confirm && s.mode_ == Mode::PerCore)) && s.per_.isEmpty()) return std::nullopt;
    s.turn_ = std::clamp(o.value("turn").toInt(), 0, int(cores.size()) - 1);
    s.tested_ = o.value("tested").toInt(-1);
    if (s.tested_ >= cores.size()) return std::nullopt;
    s.trials_ = o.value("trials").toInt();
    s.cand_ = o.value("cand").toInt();
    return s;
}

} // namespace cosearch
