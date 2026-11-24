// Unit test for the CO search strategy (costrategy.cpp) against a simulated CPU whose every core has a known
// most-negative stable offset. Plain asserts, like cpustress_test.cpp.
#include "../src/costrategy.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QMap>
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

using namespace cosearch;

/// 2 CCDs x `perCcd` cores, CPUs numbered like Linux on a 2-CCD Ryzen with SMT (core k: CPU k and k+n).
static QList<Core> cpu(int perCcd, QList<int> ranks = {}) {
    QList<Core> out;
    const int n = 2 * perCcd;
    for (int i = 0; i < n; ++i) out << Core{i / perCcd, i % perCcd, {i, i + n}, ranks.value(i, -1)};
    return out;
}

/// Most negative stable value per core (by first CPU). A trial passes when every core is at or above its
/// threshold; a failure names the CPUs of the cores below it (`named` = false: a crash, no CPU known).
struct Sim {
    QMap<int, int> stable;
    bool named = true;
    int trials = 0;
    int minValue = 0;
    Outcome run(const Search &s, const Trial &t) {
        ++trials;
        Outcome o{true, {}};
        for (int i = 0; i < s.cores().size(); ++i) {
            const int v = t.allCore ? *t.allCore : t.perCore[i];
            minValue = std::min(minValue, v);
            const Core &c = s.cores()[i];
            if (v < stable.value(c.cpus.first())) { o.pass = false; if (named) o.cpus << c.cpus.first(); }
        }
        return o;
    }
};

static QList<int> runAll(Search &s, Sim &sim) {
    for (int guard = 0; guard < 1000; ++guard) {
        auto t = s.next();
        if (!t) return s.values();
        s.report(sim.run(s, *t));
    }
    CHECK(false && "search did not end");
    return {};
}

/// Per-core: the same start for every core.
static void startAll(Search &s, int v) {
    for (const Core &c : s.cores()) s.setStart(c.ccd, c.slot, v);
}

/// The final whole-set trial: all-core confirmation or per-core 20-minute verification.
static bool isFinal(const Trial &t) { return t.what.startsWith("confirmation") || t.what.startsWith("20-minute"); }

/// Offsets in the simulated CPU's order (by first CPU), whatever order the search tests them in.
static QMap<int, int> byCpu(const Search &s, const QList<int> &v) {
    QMap<int, int> out;
    for (int i = 0; i < s.cores().size(); ++i) out[s.cores()[i].cpus.first()] = v[i];
    return out;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QMap<int, int> stable{{0, -17}, {1, -23}, {2, -30}, {3, -12}, {4, -26}, {5, -9}, {6, -21}, {7, -35}};

    // Read-only decision metadata follows the real dispatch, without changing durable snapshots.
    {
        Search s(Mode::PerCore,Level::Fast,-30,cpu(2),Tuning{-5,3}); startAll(s,-15);
        const auto before=s.save(); const auto preview=s.previewNext();
        CHECK(preview && preview->reason==DecisionReason::InitialProbe && s.save()==before);
        auto actual=s.next(); CHECK(actual && preview && actual->perCore==preview->perCore && actual->reason==preview->reason);
        s.report(Outcome{false,{}});
        for(int i=0;i<3;++i){ auto t=s.next(); CHECK(t); s.report(Outcome{true,{}}); }
        const auto recovered=s.previewNext(); CHECK(recovered && recovered->reason==DecisionReason::FailedStartRecovery);
        CHECK(recovered && recovered->failureBoundary== -15);
    }
    // Forecasts use the actual dispatcher on a copy, including verification.
    for (Mode mode : {Mode::AllCore, Mode::PerCore}) for (Level level : {Level::Fast, Level::Medium, Level::Long}) {
        Search s(mode,level,-30,cpu(2),Tuning{-5,3});
        const auto first=s.next(); CHECK(first); if(!first) continue;
        const auto saved=s.save();
        for(bool fail : {false,true}) {
            const auto forecast=s.remainingIfPass(true,fail);
            CHECK(forecast); CHECK(s.save()==saved);
            Search copy=s; copy.report(Outcome{!fail,{}});
            qint64 seconds=0; int count=0, tests=0;
            while(auto t=copy.next()) { seconds+=t->seconds(); ++count; tests+=int(t->tests.size()); copy.report(Outcome{true,{}}); if(count>4096) break; }
            CHECK(copy.finished());
            if(forecast) { CHECK(forecast->seconds==seconds); CHECK(forecast->trials==count); CHECK(forecast->tests==tests); }
            const auto terminal=copy.remainingIfPass(false); CHECK(terminal && terminal->seconds==0 && terminal->trials==0);
        }
    }
    // Test order alternates between CCDs.
    {
        Search s(Mode::AllCore, Level::Fast, -30, cpu(4));
        QList<int> order;
        for (const Core &c : s.cores()) order << c.cpus.first();
        CHECK(order == (QList<int>{0, 4, 1, 5, 2, 6, 3, 7}));
    }

    // First all-core trial: the best-ranked cores alone (taken from both CCDs), then all cores, 60 s cycles.
    {
        Search s(Mode::AllCore, Level::Medium, -30, cpu(4, {200, 210, 190, 180, 170, 175, 220, 160}));
        const Trial t = *s.next();
        CHECK(t.allCore == -5);
        CHECK(t.tests.size() == 2);
        CHECK(t.tests[0].args == (QStringList{"percore", "--load", "co", "--cycles", "1", "--kernel", "2", "--cpus", "1,6,0,5"}));
        CHECK(t.tests[1].args == (QStringList{"allcore", "--load", "co", "--cycles", "2", "--kernel", "1"}));
        CHECK(t.seconds() == (4 + 2) * 60);
    }

    // All-core: the weakest core decides; steps of 5, then 1, then a confirmation.
    {
        Search s(Mode::AllCore, Level::Fast, -30, cpu(4));
        Sim sim{stable};
        const QList<int> v = runAll(s, sim);
        CHECK(s.finished());
        CHECK(v == QList<int>(8, -9));
        // -5 pass, -10 fail, -6..-9 pass, confirmation
        CHECK(sim.trials == 7);  // -5, -10 (fails), -6, -7, -8, -9, confirmation
    }

    // Start at each core's start value, with all other cores at stock during probing.
    {
        Search s(Mode::PerCore, Level::Fast, -30, cpu(4));
        startAll(s, -15);
        const Trial first = *s.next();
        CHECK(!first.allCore.has_value());
        CHECK(first.perCore == (QList<int>{-15, 0, 0, 0, 0, 0, 0, 0}));
        CHECK(first.tests.size() == 1);
        CHECK(first.tests[0].args == (QStringList{"percore", "--load", "co", "--cycles", "1", "--kernel", "1", "--seconds", "24", "--cpus", "0"}));
        CHECK(first.seconds() == 24);
        s.report(Outcome{true, {}});
        const Trial second = *s.next();
        CHECK(second.perCore == (QList<int>{0, -15, 0, 0, 0, 0, 0, 0}));
        CHECK(second.tests[0].args.contains("24"));
    }

    // The configured limit must also govern the initial probe: no overshoot of a -5 limit.
    for (int limit : {-5, -10, -15, -30, -50}) {
        Search s(Mode::PerCore, Level::Fast, limit, cpu(2));
        startAll(s, -15);
        CHECK(s.next()->perCore.first() == std::max(-15, limit));
    }

    // Manual starting offset and step; a step change applies to future trials only.
    {
        Search s(Mode::PerCore, Level::Fast, -40, cpu(2), Tuning{-5, 7});
        startAll(s, -18);
        const auto first = s.next();
        CHECK(first && first->perCore.first() == -18);
        const QJsonObject snap = s.save();
        CHECK(snap.value("version").toInt() == 5);
        auto restored = Search::load(snap);
        CHECK(restored && restored->tuning() == (Tuning{-5, 7}));
        CHECK(restored && restored->coreState(1).start == -18);
        CHECK(restored && restored->inFlight() && restored->inFlight()->perCore == first->perCore);
        s.report(Outcome{true, {}});
        for (int k=0; k<3; ++k) {
            auto t=s.next();
            CHECK(t.has_value());
            if (k == 0) CHECK(t->perCore.value(1) == -18); // alternating CCD
            s.report(Outcome{true, {}});
        }
        auto t=s.next();
        CHECK(t && t->perCore.first() == -25);
        // Changing the step during a pending trial must not change that trial.
        CHECK(s.setTuning(Tuning{-5, 9}));
        CHECK(s.inFlight() && s.inFlight()->perCore == t->perCore);
        auto saved = Search::load(s.save());
        CHECK(saved && saved->tuning() == (Tuning{-5, 9}));
        CHECK(saved && saved->inFlight() && saved->inFlight()->perCore == t->perCore);
        CHECK(!s.setTuning(Tuning{-5, 0}));
        CHECK(!s.setTuning(Tuning{-5, 11}));
        CHECK(!s.setTuning(Tuning{0, 1}));
        s.report(Outcome{true, {}});
        for (int k=0; k<3; ++k) { CHECK(s.next().has_value()); s.report(Outcome{true, {}}); }
        auto later = s.next();
        CHECK(later && later->perCore.first() == -34);  // -25 - 9
    }

    // The first failure after a pass ends the core: its result is the previous pass, and it is never
    // probed again (not even between the pass and the failure).
    {
        Search s(Mode::PerCore, Level::Fast, -30, cpu(2), Tuning{-5, 5});
        startAll(s, -15);
        int core0 = -1;
        for (int i = 0; i < s.cores().size(); ++i) if (s.cores()[i].cpus.first() == 0) core0 = i;
        Sim sim{{{0, -22}, {1, -30}, {2, -30}, {3, -30}}};
        QList<int> tried0;
        std::optional<Trial> t;
        while ((t = s.next()) && !isFinal(*t)) {
            if (t->perCore[core0] != 0) tried0 << t->perCore[core0];
            s.report(sim.run(s, *t));
        }
        CHECK(tried0 == (QList<int>{-15, -20, -25}));
        CHECK(s.values()[core0] == -20);
        CHECK(s.coreState(core0).done && s.coreState(core0).failed == -25);
    }
    {
        // Version 3 snapshots (coarse/refine search) still resume, with the default step; a core that
        // already failed after a pass keeps that pass. Older snapshots are refused.
        Search s(Mode::PerCore, Level::Fast, -30, cpu(2));
        QJsonObject old = s.save();
        old["version"] = 3;
        old["tuning"] = QJsonObject{{"initial", -12}, {"coarse_step", 5}, {"refine_step", 3}};
        QJsonArray per = old.value("per").toArray();
        QJsonObject w = per[0].toObject();
        w["pass"] = -20; w["floor"] = -25; w["coarse"] = false; w["seeded"] = true; w.remove("excluded");
        per[0] = w;
        old["per"] = per;
        auto loaded = Search::load(old);
        CHECK(loaded.has_value());
        CHECK(loaded && loaded->tuning() == (Tuning{-5, 1}));
        CHECK(loaded && loaded->coreState(0).done && loaded->values()[0] == -20);
        CHECK(loaded && loaded->coreState(1).start == -12);
        // Version 4: one "initial" for every core; a failed start that was descending from stock now climbs.
        QJsonObject v4 = s.save();
        v4["version"] = 4;
        v4["tuning"] = QJsonObject{{"initial", -15}, {"step", 2}};
        per = v4.value("per").toArray();
        w = per[1].toObject();
        w["pass"] = 0; w["floor"] = -15; w["seeded"] = true; w.remove("start"); w.remove("up");
        per[1] = w;
        v4["per"] = per;
        auto l4 = Search::load(v4);
        CHECK(l4 && l4->tuning() == (Tuning{-5, 2}) && l4->coreState(0).start == -15);
        if (l4) {
            auto t4 = l4->next();
            CHECK(t4 && t4->perCore[0] == -15 && t4->perCore[1] == 0);  // core 0: unseeded start
            l4->report({true, {}});
            t4 = l4->next();
            CHECK(t4 && t4->perCore[1] == -13);  // core 1: climbs from -15 by 2
        }
        auto v2 = s.save();
        v2["version"] = 2;
        CHECK(!Search::load(v2));
        auto malformed = s.save();
        QJsonObject t = malformed.value("tuning").toObject();
        t["step"] = 0;
        malformed["tuning"] = t;
        CHECK(!Search::load(malformed));
    }

    // Excluded cores stay at stock in every trial, including the confirmation, and survive a snapshot.
    {
        Search s(Mode::PerCore, Level::Fast, -30, cpu(2));
        startAll(s, -12);
        CHECK(s.exclude(1, 0));   // CPU 2
        CHECK(!s.exclude(3, 0));  // no such core
        Search all(Mode::AllCore, Level::Fast, -30, cpu(2));
        CHECK(!all.exclude(1, 0));
        int ex = -1;
        for (int i = 0; i < s.cores().size(); ++i) if (s.cores()[i].ccd == 1 && s.cores()[i].slot == 0) ex = i;
        CHECK(s.coreState(ex).excluded && s.coreState(ex).done);
        auto r = Search::load(s.save());
        CHECK(r && r->coreState(ex).excluded);
        Sim sim{{{0, -20}, {1, -18}, {2, -30}, {3, -16}}};
        bool confirmed = false;
        for (int k = 0; k < 200; ++k) {
            auto t = r->next();
            if (!t) break;
            CHECK(t->perCore[ex] == 0);
            confirmed |= isFinal(*t);
            r->report(sim.run(*r, *t));
        }
        CHECK(confirmed && r->finished());
        CHECK(byCpu(*r, r->values()) == (QMap<int, int>{{0, -20}, {1, -18}, {2, 0}, {3, -16}}));
        QJsonObject bad = s.save();
        QJsonArray per = bad.value("per").toArray();
        QJsonObject w = per[ex].toObject();
        w["pass"] = -5;
        per[ex] = w;
        bad["per"] = per;
        CHECK(!Search::load(bad));  // an excluded core is always at stock
    }

    // A journal's all-core walk is bounded like a per-core one: never past the limit, never above stock.
    {
        Search s(Mode::AllCore, Level::Fast, -20, cpu(2));
        CHECK(Search::load(s.save()));
        for (auto [pass, floor] : {std::pair(-35, -40), std::pair(5, -21), std::pair(-10, -60), std::pair(-10, -10)}) {
            QJsonObject bad = s.save();
            QJsonObject w = bad.value("all").toObject();
            w["pass"] = pass;
            w["floor"] = floor;
            bad["all"] = w;
            CHECK(!Search::load(bad));
        }
    }

    // Exhaustive: every start, step, limit and stable threshold. Values on a core only ever get more
    // negative, nothing at or below a failure is retried, the result is the last pass, and with a step
    // of 1 the result is exactly the threshold.
    {
        int cases = 0;
        for (int initial : {0, -1, -5, -15, -28, -50})
            for (int step : {1, 2, 3, 5, 10})
                for (int limit : {-5, -17, -30, -50})
                    for (int stable = limit - 2; stable <= 0; ++stable) {
                        Search s(Mode::PerCore, Level::Fast, limit, cpu(1), Tuning{-5, step});
                        startAll(s, initial);
                        Sim sim{{{0, stable}, {1, 0}}};
                        int lastPass = 0, failedAt = 1;  // 1 = no failure yet
                        bool ended = false;              // failed after a pass, or passed after a failed start
                        int probes = 0;
                        for (int k = 0; k < 200; ++k) {
                            auto t = s.next();
                            if (!t) break;
                            const int v = t->perCore[0];
                            if (isFinal(*t) || v == 0) { s.report(sim.run(s, *t)); continue; }
                            CHECK(!ended && "probed again after the core's result was found");
                            CHECK(v >= limit && v < lastPass);
                            CHECK(failedAt > 0 || v > failedAt);
                            if (probes++ == 0) CHECK(v == std::max(initial < 0 ? initial : -step, limit));
                            const Outcome o = sim.run(s, *t);
                            if (o.pass) { ended = failedAt <= 0; lastPass = v; }
                            else { ended = lastPass < 0; failedAt = failedAt > 0 ? v : std::max(failedAt, v); }
                            s.report(o);
                        }
                        CHECK(s.finished());
                        if (step == 1) CHECK(s.values()[0] == std::min(0, std::max(stable, limit)));
                        CHECK(s.values()[0] == lastPass);
                        CHECK(s.values()[0] >= stable);
                        ++cases;
                    }
        std::printf("exhaustive per-core walks: %d\n", cases);
    }

    // Recorded crash at -25 on a core: no new search ever probes -25 or anything more negative there.
    {
        int cases = 0;
        for (int guard = -30; guard <= -1; ++guard)
            for (int initial : {-1, -10, -15, -30})
                for (int step : {1, 3, 7})
                    for (int stable = -31; stable <= 0; stable += 3) {
                        Search s(Mode::PerCore, Level::Fast, -30, cpu(1), Tuning{-5, step});
                        startAll(s, initial);
                        CHECK(s.guardAgainstCrash(0, 0, {0, 2}, guard));
                        Sim sim{{{0, stable}, {1, 0}}};
                        for (int k = 0; k < 200; ++k) {
                            auto t = s.next();
                            if (!t) break;
                            CHECK(t->perCore[0] > guard);
                            s.report(sim.run(s, *t));
                        }
                        CHECK(s.finished() && s.values()[0] > guard);
                        ++cases;
                    }
        std::printf("crash-guard walks: %d\n", cases);
    }

    // A core that cannot hold -15 is bracketed toward zero, not automatically marked -15 stable.
    {
        Search s(Mode::PerCore, Level::Fast, -30, cpu(2));
        startAll(s, -15);
        Sim sim{{{0, -2}, {1, -8}, {2, -14}, {3, 0}}};
        const QMap<int, int> got = byCpu(s, runAll(s, sim));
        CHECK(got == sim.stable);
        CHECK(s.finished());
    }

    // Quick probes use isolation; the combined confirmation uses the whole discovered curve.
    {
        Search s(Mode::PerCore, Level::Medium, -30, cpu(2));
        Sim sim{{{0, -25}, {1, -30}, {2, -20}, {3, -18}}};
        std::optional<Trial> t;
        while ((t = s.next()) && !isFinal(*t)) {
            int nonzero = 0;
            for (int v : t->perCore) nonzero += v != 0;
            CHECK(nonzero == 1);
            CHECK(t->tests[0].args.contains("24"));
            s.report(sim.run(s, *t));
        }
        CHECK(t.has_value());
        CHECK(t && t->what.startsWith("20-minute"));
        CHECK(t->perCore == s.values());
        CHECK(t->seconds() == VERIFY_SECONDS);
    }

    // A start that fails climbs back towards stock by the step; the first pass is the result.
    {
        Search s(Mode::PerCore, Level::Fast, -30, cpu(1), Tuning{-5, 3});
        CHECK(s.setStart(0, 0, -20));
        CHECK(s.setStart(1, 0, 0));
        CHECK(!s.setStart(2, 0, -5));
        Sim sim{{{0, -16}, {1, -4}}};
        QList<int> tried0, tried1;
        std::optional<Trial> t;
        while ((t = s.next()) && !isFinal(*t)) {
            if (t->perCore[0]) tried0 << t->perCore[0];
            if (t->perCore[1]) tried1 << t->perCore[1];
            s.report(sim.run(s, *t));
        }
        CHECK(tried0 == (QList<int>{-20, -17, -14}));  // -20, -17 fail; -14 passes: done
        CHECK(tried1 == (QList<int>{-3, -6}));         // from stock: -3 passes, -6 fails: done
        CHECK(s.values() == (QList<int>{-14, -3}));
        // The core is marked done by report() itself (the dialog logs it right then), whichever way it ends.
        Search u(Mode::PerCore, Level::Fast, -30, cpu(1), Tuning{-5, 2});
        startAll(u, -10);
        u.next(); u.report({false, {}});                           // core 0: -10 fails
        u.next(); u.report({true, {}});                            // core 1: -10 passes
        CHECK(u.next()->perCore[0] == -8); u.report({true, {}});   // core 0 climbs: -8 passes
        CHECK(u.coreState(0).done && u.values()[0] == -8);
        CHECK(u.next()->perCore[1] == -12); u.report({false, {}}); // core 1: -12 fails after a pass
        CHECK(u.coreState(1).done && u.values()[1] == -10);
        // A start below the limit or above stock is clamped.
        Search c(Mode::PerCore, Level::Fast, -20, cpu(1));
        CHECK(c.setStart(0, 0, -35) && c.coreState(0).start == -20);
        CHECK(c.setStart(1, 0, 7) && c.coreState(1).start == 0);
    }

    // All-core: a configurable start; a failed start keeps steps of 5 from stock, then steps of 1.
    {
        Search s(Mode::AllCore, Level::Fast, -30, cpu(2), Tuning{-15, 1});
        Sim sim{{{0, -12}, {1, -20}, {2, -20}, {3, -20}}};
        QList<int> tried;
        std::optional<Trial> t;
        while ((t = s.next()) && !isFinal(*t)) { tried << *t->allCore; s.report(sim.run(s, *t)); }
        CHECK(tried == (QList<int>{-15, -5, -10, -11, -12, -13}));
        CHECK(s.values() == QList<int>(4, -12));
        Search d(Mode::AllCore, Level::Fast, -30, cpu(2));
        CHECK(d.next()->allCore == -5);  // default start: the previous fixed behaviour
    }

    // The 20-minute verification: whole curve applied; light and heavy parts alternate, power limit twice.
    for (int n : {1, 2, 3, 4, 6, 8, 16, 17, 18, 25}) {  // 2n cores
        Search s(Mode::PerCore, Level::Fast, -30, cpu(n));
        startAll(s, -10);
        Sim sim;
        for (const Core &c : s.cores()) sim.stable[c.cpus.first()] = -10;
        std::optional<Trial> t;
        while ((t = s.next()) && !isFinal(*t)) s.report(sim.run(s, *t));
        CHECK(t && t->what.startsWith("20-minute") && t->perCore == s.values());
        if (!t) continue;
        const int cores = 2 * n;
        QStringList firsts;
        for (const Core &c : s.cores()) firsts << QString::number(c.cpus.first());
        CHECK(t->tests[0].args == (QStringList{"percore", "--load", "co", "--cycles", "1", "--kernel", "0", "--seconds", "24", "--cpus", firsts.join(',')}));
        CHECK(t->tests[1].args == (QStringList{"allcore", "--load", "power", "--seconds", "180"}) && t->tests[1].seconds == 180);
        CHECK(t->tests[2].args == (QStringList{"allcore", "--load", "co", "--cycles", "2", "--kernel", "1"}));
        CHECK(t->tests.last().args.mid(0, 3) == (QStringList{"allcore", "--load", "power"}) && t->tests.last().seconds >= 60);
        CHECK(t->tests.last().args.last() == QString::number(t->tests.last().seconds));
        CHECK(t->seconds() == std::max(VERIFY_SECONDS, cores * 24 + 360));
        for (const Test &x : t->tests) {
            CHECK(x.seconds > 0);
            if (x.args.contains("sse")) CHECK(x.args.contains("--transitions") && x.seconds / cores >= 5);
            const qsizetype c = x.args.indexOf("--cycles");
            if (c >= 0) CHECK(x.args[c + 1].toInt() >= 1 && x.args[c + 1].toInt() <= 100);
        }
        if (cores == 8) CHECK(t->tests.size() == 6 && t->tests[4].args.contains("sse"));  // room for classic SSE too
        if (cores == 16) {                                                                // no room for classic SSE
            CHECK(t->tests.size() == 5 && t->tests[3].seconds == 384 && t->tests[4].seconds == 132);
        }
    }
    {
        // Only undervolted cores are loaded alone; excluded / stock cores are not.
        Search s(Mode::PerCore, Level::Fast, -30, cpu(2));
        startAll(s, -10);
        CHECK(s.exclude(1, 1));
        Sim sim{{{0, -10}, {1, -10}, {2, 0}, {3, -10}}};
        std::optional<Trial> t;
        while ((t = s.next()) && !isFinal(*t)) s.report(sim.run(s, *t));
        CHECK(t && t->tests[0].args.last() == "0,1");  // CPU 2 (stable 0) and CPU 3 (excluded) at stock
    }

    // Per-core: each core goes down to its own threshold, never past the limit.
    {
        Search s(Mode::PerCore, Level::Fast, -30, cpu(4));
        Sim sim{stable};
        const QMap<int, int> got = byCpu(s, runAll(s, sim));
        QMap<int, int> want = stable;
        want[7] = -30;  // limit
        CHECK(got == want);
        CHECK(sim.minValue == -30);
        CHECK(s.finished());
    }

    // The confirmation finds what the single trials missed: +1 only for the core it names, until it passes.
    {
        QMap<int, int> want = stable;
        want[7] = -30;
        // Core 2 turns out 2 steps weaker under the full set than alone.
        Search s2(Mode::PerCore, Level::Fast, -30, cpu(4));
        Sim descent{stable};
        std::optional<Trial> t;
        while ((t = s2.next()) && !isFinal(*t)) s2.report(descent.run(s2, *t));
        CHECK(t && t->tests.size() >= 2);
        Sim strict{stable};
        strict.stable[2] = -28;
        while (t) { s2.report(strict.run(s2, *t)); t = s2.next(); }
        want[2] = -28;
        CHECK(byCpu(s2, s2.values()) == want);
    }

    // A confirmation failure that names no CPU (a crash) moves every undervolted core up by one.
    {
        Search s(Mode::PerCore, Level::Fast, -30, cpu(2));
        Sim descent{{{0, -10}, {1, -20}, {2, 0}, {3, -15}}};
        std::optional<Trial> t;
        while ((t = s.next()) && !isFinal(*t)) s.report(descent.run(s, *t));
        CHECK(byCpu(s, s.values()) == (QMap<int, int>{{0, -10}, {1, -20}, {2, 0}, {3, -15}}));
        s.report(Outcome{false, {}});
        CHECK(byCpu(s, s.values()) == (QMap<int, int>{{0, -9}, {1, -19}, {2, 0}, {3, -14}}));
        // A failure named on a core already at stock cannot be that core's: same as unnamed.
        CHECK(s.next());
        s.report(Outcome{false, {2}});
        CHECK(byCpu(s, s.values()) == (QMap<int, int>{{0, -8}, {1, -18}, {2, 0}, {3, -13}}));
    }

    // Nothing below stock is stable: the search ends at 0 without a confirmation.
    {
        Search s(Mode::AllCore, Level::Fast, -30, cpu(2));
        Sim sim{{{0, 3}}};
        CHECK(runAll(s, sim) == QList<int>(4, 0));
        CHECK(sim.trials == 2);  // -5 fails, then -1 fails
    }

    // A crash mid-search: the snapshot taken before the trial resumes it; the trial counts as failed.
    for (Mode mode : {Mode::AllCore, Mode::PerCore}) {
        Search ref(mode, Level::Medium, -30, cpu(4, {1, 2, 3, 4, 5, 6, 7, 8}));
        Sim simRef{stable};
        const QList<int> want = runAll(ref, simRef);
        for (int crashAt = 0; crashAt < 40; crashAt += 3) {
            Search s(mode, Level::Medium, -30, cpu(4, {1, 2, 3, 4, 5, 6, 7, 8}));
            Sim sim{stable};
            for (int i = 0; i < crashAt; ++i) {
                auto t = s.next();
                if (!t) break;
                s.report(sim.run(s, *t));
            }
            if (s.finished()) continue;
            const QJsonObject snap = s.save();  // written before next(): next() on the snapshot gives the same trial
            const Trial t = *s.next();
            auto r = Search::load(snap);
            CHECK(r.has_value());
            const Trial again = *r->next();
            CHECK(again.what == t.what && again.allCore == t.allCore && again.perCore == t.perCore);
            CHECK(again.tests.size() == t.tests.size() && again.tests[0].args == t.tests[0].args);
            if (!sim.run(s, t).pass) {
                // This trial would have failed anyway: a crash counted as its failure gives the same result.
                r->report(Outcome{false, {}});
                CHECK(runAll(*r, sim) == want);
            }
            CHECK(Search::load(snap)->save() == snap);
        }
    }

    // HARD REBOOT regression: CCD1/slot4 (logical CPU 9) reboots at -25.
    // Recovery must never repeat -25 or go deeper; the recovery snapshot is AFTER next().
    {
        Search s(Mode::PerCore, Level::Fast, -30, cpu(5));
        startAll(s, -15);
        bool rebooted = false;
        for (int attempt = 0; attempt < 200; ++attempt) {
            const auto t = s.next();
            CHECK(t.has_value());
            if (!t) break;
            const auto pending = Search::load(s.save());
            CHECK(pending.has_value());
            if (pending) {
                const auto inflight = pending->inFlight();
                CHECK(inflight.has_value());
                CHECK(inflight && inflight->perCore == t->perCore && inflight->allCore == t->allCore && inflight->what == t->what);
            }
            int i9 = -1;
            for (int i = 0; i < s.cores().size(); ++i)
                if (s.cores()[i].cpus.contains(9)) i9 = i;
            CHECK(i9 >= 0);
            if (i9 >= 0 && !t->perCore.isEmpty() && t->perCore[i9] == -25 &&
                !isFinal(*t)) {
                auto resumed = Search::load(s.save());
                CHECK(resumed.has_value());
                if (!resumed) break;
                resumed->report(Outcome{false, {}, true}); // hard reboot is an unsafe trial
                const QJsonObject resolved = resumed->save();
                auto secondBoot = Search::load(resolved);
                CHECK(secondBoot.has_value());
                CHECK(secondBoot && !secondBoot->inFlight().has_value());
                if (secondBoot) {
                    // The crashed core is finished at its last pass (-24) and only reappears in the
                    // confirmation; there it fails (stable -23 under the full set) and backs off.
                    CHECK(secondBoot->coreState(i9).done && secondBoot->values()[i9] == -24);
                    Sim sim;
                    sim.stable[9] = -23;
                    for (int k = 0; k < 400; ++k) {
                        const auto next = secondBoot->next();
                        if (!next) break;
                        if (!next->perCore.isEmpty()) {
                            CHECK(next->perCore[i9] > -25);
                            if (!isFinal(*next)) CHECK(next->perCore[i9] == 0);
                        }
                        secondBoot->report(sim.run(*secondBoot, *next));
                    }
                    CHECK(secondBoot->finished());
                    CHECK(secondBoot->values()[i9] == -23);
                }
                rebooted = true;
                break;
            }
            s.report(Outcome{true, {}});
        }
        CHECK(rebooted);
    }

    // An all-core crash guard: a new all-core search never goes to that offset or below, confirmation included.
    {
        for (int start : {-5, -20, -30}) {
            Search s(Mode::AllCore, Level::Fast, -30, cpu(2), Tuning{start, 1});
            CHECK(s.guardAllCore(-20));
            CHECK(!s.guardAllCore(-40));  // past the limit: nothing to guard
            Search per(Mode::PerCore, Level::Fast, -30, cpu(2));
            CHECK(!per.guardAllCore(-20));  // per-core probes are not limited by an all-core crash
            Sim sim{{{0, -28}, {1, -28}, {2, -28}, {3, -28}}};
            runAll(s, sim);
            CHECK(sim.minValue > -20 && s.finished() && s.values().first() == -19);
        }
    }

    // A crash guard persists into a BRAND NEW search on the same topology.
    {
        Search s(Mode::PerCore, Level::Fast, -30, cpu(5));
        CHECK(s.guardAgainstCrash(1, 4, {9, 19}, -25));
        CHECK(s.guardAgainstCrash(1, 4, {9, 20}, -25)); // logical CPU numbering changed; physical core guard survives
        CHECK(!s.guardAgainstCrash(1, 3, {8, 18}, -60)); // invalid CO failure cannot be applied
        int i9 = -1;
        for (int i = 0; i < s.cores().size(); ++i) if (s.cores()[i].cpus.contains(9)) i9 = i;
        CHECK(i9 >= 0);
        Sim sim;
        sim.stable[9] = -23;
        for (int k = 0; k < 500; ++k) {
            auto t = s.next();
            if (!t) break;
            if (!t->perCore.isEmpty()) CHECK(t->perCore[i9] > -25);
            s.report(sim.run(s, *t));
        }
        CHECK(s.finished());
        CHECK(s.values()[i9] == -23);
    }

    // Confirmation-stage hard reboot has no reliable culprit: conservatively back off
    // every non-stock core by 2 rather than repeating the whole crashing set.
    {
        Search s(Mode::PerCore, Level::Fast, -30, cpu(2));
        Sim sim{{{0, -10}, {1, -12}, {2, -5}, {3, -8}}};
        std::optional<Trial> t;
        while ((t = s.next()) && !isFinal(*t)) s.report(sim.run(s, *t));
        CHECK(t.has_value());
        const QList<int> before = s.values();
        s.report(Outcome{false, {}, true});
        for (int i = 0; i < before.size(); ++i)
            CHECK(s.values()[i] == std::min(0, before[i] + 2));
    }

    // Snapshots that do not fit are refused.
    {
        Search s(Mode::PerCore, Level::Long, -40, cpu(2));
        QJsonObject o = s.save();
        CHECK(Search::load(o).has_value());
        o["level"] = "extreme";
        CHECK(!Search::load(o));
        o = s.save(); o["limit"] = 5;
        CHECK(!Search::load(o));
        o = s.save(); o["cores"] = QJsonArray{};
        CHECK(!Search::load(o));
        o = s.save(); o["version"] = 2;
        CHECK(!Search::load(o));  // older snapshots used all-core presearch and cannot safely resume
        o = s.save(); o["per"] = QJsonArray{};
        CHECK(!Search::load(o));
    }

    // Verification only (a saved profile): no probes, straight to the whole-set verification; a failing
    // core backs off until the set passes; the state survives save / load.
    {
        Search s(Mode::PerCore, Level::Fast, -30, cpu(4));
        QList<int> v;
        for (const Core &c : s.cores()) v << std::max(-30, stable.value(c.cpus.first()) - 2);  // 2 points too deep
        CHECK(!s.startVerification(v.mid(1)));          // wrong length
        QList<int> bad = v;
        bad[0] = 3;
        CHECK(!s.startVerification(bad));               // positive offset
        CHECK(s.startVerification(v));
        CHECK(s.confirming() && !s.perCoreSearching());
        Sim sim{stable};
        int guard = 0;
        while (auto t = s.next()) {
            CHECK(t->tests.size() > 1);  // the verification, never a single-core probe
            const auto back = Search::load(s.save());
            CHECK(back && back->confirming());
            s.report(sim.run(s, *t));
            if (++guard > 200) break;
        }
        CHECK(s.finished());
        for (int i = 0; i < s.cores().size(); ++i) CHECK(s.values()[i] >= stable.value(s.cores()[i].cpus.first()));
        CHECK(!s.startVerification(v));                 // only before the first next()

        Search a(Mode::AllCore, Level::Fast, -30, cpu(4));
        CHECK(a.startVerification(QList<int>(a.cores().size(), -8)));
        const auto t = a.next();
        CHECK(t && t->allCore == -8);
        a.report(Outcome{true, {}});
        CHECK(a.finished() && a.values().first() == -8);
    }

    // Levels: more testing per trial as they go up; the estimate grows with them.
    {
        CHECK(spec(Level::Fast).trialCycles < spec(Level::Long).trialCycles);
        CHECK(Search::estimateMinutes(Mode::AllCore, Level::Fast, 16, -30) < Search::estimateMinutes(Mode::AllCore, Level::Long, 16, -30));
        CHECK(Search::estimateMinutes(Mode::AllCore, Level::Fast, 16, -30) < Search::estimateMinutes(Mode::PerCore, Level::Fast, 16, -30));
        CHECK(levelFrom("medium") == Level::Medium && !levelFrom("x"));
    }

    // Clean exit (application quit after a confirmed stock reset): the dialog resumes from the journal's
    // after-next() snapshot and runs the in-flight trial again instead of calling next(). For a quit at every
    // trial of every mode, that must be the very trial that was cut short, and the search must end exactly
    // like one that was never interrupted: same values, same trials (the repeat replaces the cut-short run), nothing below the limit.
    {
        int replays = 0;
        for (const Mode mode : {Mode::PerCore, Mode::AllCore}) {
            auto fresh = [&] {
                Search s(mode, Level::Fast, -30, cpu(4), Tuning{-5, 2});
                if (mode == Mode::PerCore) startAll(s, -15);
                return s;
            };
            Sim ref{stable};
            Search whole = fresh();
            const QList<int> expected = runAll(whole, ref);
            for (int quitAt = 1; quitAt <= ref.trials; ++quitAt) {
                Search s = fresh();
                Sim sim{stable};
                std::optional<Search> resumed;
                for (int n = 1; n <= quitAt; ++n) {
                    const auto t = s.next();
                    if (!t) break;
                    if (n < quitAt) { s.report(sim.run(s, *t)); continue; }
                    resumed = Search::load(s.save());  // the pending journal, written after next()
                    const auto again = resumed ? resumed->inFlight() : std::nullopt;
                    CHECK(again && again->what == t->what && again->perCore == t->perCore && again->allCore == t->allCore);
                    CHECK(again && again->tests.size() == t->tests.size() && again->tests.first().args == t->tests.first().args);
                    if (!again) break;
                    resumed->report(sim.run(*resumed, *again));  // the repeat, reported on the resumed search
                    ++replays;
                }
                if (!resumed) continue;
                CHECK(runAll(*resumed, sim) == expected);
                CHECK(sim.trials == ref.trials);  // the cut-short run gave no result; its repeat is the one counted
                CHECK(sim.minValue >= -30);
            }
        }
        CHECK(replays > 20);
        std::printf("clean-exit replays: %d\n", replays);
    }

    // A preview must not consume a trial or mutate the persisted search state.
    for (const auto mode : {Mode::PerCore, Mode::AllCore}) {
        Search search(mode, Level::Fast, -30, cpu(4), Tuning{-5,2});
        if (mode == Mode::PerCore) startAll(search,-15);
        Sim sim{stable};
        for (int i=0; i<1000; ++i) {
            const auto before=search.save();
            const auto preview=search.previewNext();
            CHECK(search.save()==before);
            const auto actual=search.next();
            CHECK(bool(preview)==bool(actual));
            if (!actual || !preview) break;
            CHECK(preview->what==actual->what && preview->perCore==actual->perCore && preview->allCore==actual->allCore);
            CHECK(preview->seconds()==actual->seconds());
            search.report(sim.run(search,*actual));
        }
    }
    // A damaged or aliased topology may not dispatch a stress/offset trial.
    {
        auto duplicate = cpu(2);
        duplicate[1].cpus = duplicate[0].cpus;
        Search bad(Mode::PerCore, Level::Fast, -30, duplicate);
        CHECK(!bad.next());
        CHECK(!Search::load(bad.save()));
        Search empty(Mode::AllCore, Level::Fast, -30, {});
        CHECK(!empty.startVerification({}));
        CHECK(!empty.next());
        Search all(Mode::AllCore, Level::Fast, -20, cpu(1));
        CHECK(!all.startVerification({-10, -11}));
        CHECK(!all.startVerification({-21, -21}));
        CHECK(all.guardAllCore(-12));
        CHECK(!all.startVerification({-12, -12}));
        CHECK(all.startVerification({-11, -11}));
        Search per(Mode::PerCore, Level::Fast, -20, cpu(1));
        const auto first = per.cores().first();
        CHECK(per.exclude(first.ccd, first.slot));
        CHECK(!per.startVerification({-1, 0}));
        CHECK(per.startVerification({0, -1}));
    }
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("costrategy: all checks passed");
    return 0;
}
