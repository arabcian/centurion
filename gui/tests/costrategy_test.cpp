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

/// Offsets in the simulated CPU's order (by first CPU), whatever order the search tests them in.
static QMap<int, int> byCpu(const Search &s, const QList<int> &v) {
    QMap<int, int> out;
    for (int i = 0; i < s.cores().size(); ++i) out[s.cores()[i].cpus.first()] = v[i];
    return out;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QMap<int, int> stable{{0, -17}, {1, -23}, {2, -30}, {3, -12}, {4, -26}, {5, -9}, {6, -21}, {7, -35}};

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

    // Direct -15 per physical core, with all other cores at stock during probing.
    {
        Search s(Mode::PerCore, Level::Fast, -30, cpu(4));
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
        CHECK(s.next()->perCore.first() == std::max(-15, limit));
    }

    // Manual starting offset, coarse stride and post-failure 3 -> 2 -> 1 refinement.
    {
        Search s(Mode::PerCore, Level::Fast, -40, cpu(2), Tuning{-18, 7, 3});
        const auto first = s.next();
        CHECK(first && first->perCore.first() == -18);
        const QJsonObject snap = s.save();
        CHECK(snap.value("version").toInt() == 3);
        auto restored = Search::load(snap);
        CHECK(restored && restored->tuning() == (Tuning{-18, 7, 3}));
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
        // Changing the stride during a pending trial must not change that trial.
        CHECK(s.setTuning(Tuning{-20, 9, 3}));
        CHECK(s.inFlight() && s.inFlight()->perCore == t->perCore);
        auto saved = Search::load(s.save());
        CHECK(saved && saved->tuning() == (Tuning{-20, 9, 3}));
        CHECK(saved && saved->inFlight() && saved->inFlight()->perCore == t->perCore);
        CHECK(!s.setTuning(Tuning{-20, 0, 3}));
    }
    {
        Search s(Mode::PerCore, Level::Fast, -30, cpu(2), Tuning{-15, 5, 3});
        // Fixed representative core: quick probe chronology includes the fail at -25,
        // followed by values -23, -22, -21 as the boundary narrows.
        for (int i=0; i<4; ++i) { const auto t=s.next(); CHECK(t.has_value()); s.report({true,{}}); }
        for (int i=0; i<4; ++i) { const auto t=s.next(); CHECK(t.has_value()); s.report({true,{}}); }
        const auto fail = s.next();
        CHECK(fail && fail->perCore.first() == -25);
        s.report({false,{}});
        // The turn order alternates; other cores receive their own -25 attempts.
        for (int i=0; i<3; ++i) { const auto t=s.next(); CHECK(t.has_value()); s.report({false,{}}); }
        const auto fine = s.next();
        CHECK(fine && fine->perCore.first() == -23);
    }
    {
        // Old v2 strategy snapshots remain loadable so v3 recovery journals from
        // the previous release do not silently become unrecoverable.
        Search s(Mode::PerCore, Level::Fast, -30, cpu(2));
        auto old = s.save();
        old["version"] = 2;
        old.remove("tuning");
        auto loaded = Search::load(old);
        CHECK(loaded.has_value());
        CHECK(loaded && loaded->tuning() == Tuning{});
        auto malformed = s.save();
        QJsonObject t = malformed.value("tuning").toObject();
        t["coarse_step"] = 0;
        malformed["tuning"] = t;
        CHECK(!Search::load(malformed));
    }

    // A core that cannot hold -15 is bracketed toward zero, not automatically marked -15 stable.
    {
        Search s(Mode::PerCore, Level::Fast, -30, cpu(2));
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
        while ((t = s.next()) && !t->what.startsWith("confirmation")) {
            int nonzero = 0;
            for (int v : t->perCore) nonzero += v != 0;
            CHECK(nonzero == 1);
            CHECK(t->tests[0].args.contains("24"));
            s.report(sim.run(s, *t));
        }
        CHECK(t.has_value());
        CHECK(t->perCore == s.values());
        CHECK(!t->tests[0].args.contains("24"));
        CHECK(t->tests[0].seconds == 4 * 2 * 60);
        CHECK(t->tests[1].seconds == 2 * 60);
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
        while ((t = s2.next()) && !t->what.startsWith("confirmation")) s2.report(descent.run(s2, *t));
        CHECK(t && t->tests.size() == 2);
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
        while ((t = s.next()) && !t->what.startsWith("confirmation")) s.report(descent.run(s, *t));
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
    // Recovery must never repeat -25 or even try -24; the recovery snapshot is AFTER next().
    {
        Search s(Mode::PerCore, Level::Fast, -30, cpu(5));
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
                !t->what.startsWith("confirmation")) {
                auto resumed = Search::load(s.save());
                CHECK(resumed.has_value());
                if (!resumed) break;
                resumed->report(Outcome{false, {}, true}); // hard reboot is an unsafe trial
                const QJsonObject resolved = resumed->save();
                auto secondBoot = Search::load(resolved);
                CHECK(secondBoot.has_value());
                CHECK(secondBoot && !secondBoot->inFlight().has_value());
                if (secondBoot) {
                    Sim sim;
                    sim.stable[9] = -23;
                    for (int k = 0; k < 400; ++k) {
                        const auto next = secondBoot->next();
                        if (!next) break;
                        if (!next->perCore.isEmpty()) CHECK(next->perCore[i9] >= -23);
                        secondBoot->report(sim.run(*secondBoot, *next));
                    }
                    CHECK(secondBoot->finished());
                    CHECK(secondBoot->values()[i9] >= -23);
                }
                rebooted = true;
                break;
            }
            s.report(Outcome{true, {}});
        }
        CHECK(rebooted);
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
            if (!t->perCore.isEmpty()) CHECK(t->perCore[i9] >= -23);
            s.report(sim.run(s, *t));
        }
        CHECK(s.finished());
        CHECK(s.values()[i9] == -23);
    }

    // Confirmation-stage hard reboot has no reliable culprit: conservatively back off
    // every non-stock core by 2 rather than repeating the whole crashing set.
    {
        Search s(Mode::PerCore, Level::Fast, -30, cpu(2));
        Sim sim;
        std::optional<Trial> t;
        while ((t = s.next()) && !t->what.startsWith("confirmation")) s.report(sim.run(s, *t));
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
        o = s.save(); o["version"] = 1;
        CHECK(!Search::load(o));  // older snapshots used all-core presearch and cannot safely resume
        o = s.save(); o["per"] = QJsonArray{};
        CHECK(!Search::load(o));
    }

    // Levels: more testing per trial as they go up; the estimate grows with them.
    {
        CHECK(spec(Level::Fast).trialCycles < spec(Level::Long).trialCycles);
        CHECK(Search::estimateMinutes(Mode::AllCore, Level::Fast, 16, -30) < Search::estimateMinutes(Mode::AllCore, Level::Long, 16, -30));
        CHECK(Search::estimateMinutes(Mode::AllCore, Level::Fast, 16, -30) < Search::estimateMinutes(Mode::PerCore, Level::Fast, 16, -30));
        CHECK(levelFrom("medium") == Level::Medium && !levelFrom("x"));
    }

    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("costrategy: all checks passed");
    return 0;
}
