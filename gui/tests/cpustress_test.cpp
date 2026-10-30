// Unit test for Health → Curve stability → CPU test (cpustress.cpp). Plain asserts, like gpustress_test.cpp.
#include "../src/cpustress.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

using namespace cpustress;

static Run fed(const QStringList &lines) {
    Run r;
    for (const QString &l : lines) feed(r, l);
    return r;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    // Plan: cycles only for the per-core test; transitions for both.
    CHECK(plan(Mode::PerCore, "sse", 180, false).args == (QStringList{"percore", "--load", "sse", "--seconds", "180"}));
    CHECK(plan(Mode::PerCore, "avx2", 60, true, 3).args
          == (QStringList{"percore", "--load", "avx2", "--seconds", "60", "--cycles", "3", "--transitions"}));
    CHECK(plan(Mode::AllCore, "avx512", 600, true, 3).args == (QStringList{"allcore", "--load", "avx512", "--seconds", "600", "--transitions"}));
    CHECK(plan(Mode::AllCore, "avx", 600, false).error == QLatin1String("load") && plan(Mode::AllCore, "avx", 600, false).args.isEmpty());
    CHECK(plan(Mode::PerCore, "sse", 0, false).error == QLatin1String("seconds"));
    CHECK(plan(Mode::PerCore, "sse", 60, false, MAX_CYCLES + 1).error == QLatin1String("seconds"));

    // info: cores with their siblings, only the loads this CPU runs.
    const Info info = parseInfo(R"j({"cores":[{"cpus":[0,16],"l3":0},{"cpus":[1,17],"l3":0},{"cpus":[],"l3":1}],"loads":{"sse":true,"avx2":true,"avx512":false}})j");
    CHECK(info.cores.size() == 2 && info.cores[1] == (QList<int>{1, 17}) && info.loads == (QStringList{"sse", "avx2"}));
    CHECK(parseInfo("garbage").cores.isEmpty());

    // Output lines: a pass of the per-core test.
    Run r;
    CHECK(!feed(r, R"j({"ev":"start","mode":"percore","load":"sse","seconds":60,"cycles":1,"transitions":false,"cpus":[0,1]})j"));
    CHECK(feed(r, R"j({"ev":"core","t":0.0,"cpu":0,"cycle":0})j") && r.cpu == 0);
    feed(r, "not json");
    feed(r, R"j({"ev":"progress","t":1.0,"chunks":70,"errors":0,"cpu":0})j");
    feed(r, R"j({"ev":"core_end","t":60.0,"cpu":0,"chunks":4000,"errors":0})j");
    CHECK(feed(r, R"j({"ev":"core","t":60.0,"cpu":1,"cycle":0})j") && r.cpu == 1);
    feed(r, R"j({"ev":"end","status":"pass","t":120.0,"chunks":8000,"errors":0,"failed":[],"msg":""})j");
    CHECK(r.cpus == (QList<int>{0, 1}) && r.done == (QList<int>{0}) && r.chunks == 8000 && r.ended && r.failed.isEmpty());
    CHECK(verdict(r, 0, false, false, 0) == Verdict::Pass);
    CHECK(verdict(r, 0, false, false, 1) == Verdict::Fail);      // a machine check during the run
    CHECK(verdict(r, 0, false, true, 0) == Verdict::Incomplete); // stopped by the user
    CHECK(verdict(r, 0, true, false, 0) == Verdict::Incomplete);
    CHECK(verdict(fed({}), 0, false, false, 0) == Verdict::Incomplete);

    // Wrong results: the failing CPU comes from the error line and from the end line; the counts never go down.
    Run bad = fed({R"j({"ev":"error","t":3.0,"cpu":5,"seed":2,"expected":"0x1","actual":"0x0"})j"});
    CHECK(bad.errors == 1 && bad.failed == (QList<int>{5}));
    feed(bad, R"j({"ev":"progress","t":4.0,"chunks":9,"errors":3})j");
    feed(bad, R"j({"ev":"end","status":"fail","t":9.0,"chunks":20,"errors":3,"failed":[5,7],"msg":"wrong results"})j");
    CHECK(bad.errors == 3 && bad.failed == (QList<int>{5, 7}) && verdict(bad, 1, false, false, 0) == Verdict::Fail);
    CHECK(verdict(bad, 4, false, true, 0) == Verdict::Fail);  // an error found is real even when stopped
    CHECK(verdictName(Verdict::Pass) == "pass" && verdictName(Verdict::Fail) == "fail" && verdictName(Verdict::Incomplete) == "incomplete");

    // Kernel events: machine checks and lockups count, with the CPU they name.
    CHECK(isCpuFault("mce") && isCpuFault("lockup") && !isCpuFault("xid") && !isCpuFault("nvme"));
    CHECK(eventCpu("mce: [Hardware Error]: CPU 3: Machine Check: 0 Bank 5: be00000000800400") == 3);
    CHECK(eventCpu("watchdog: BUG: soft lockup - CPU#12 stuck for 22s! [stress:1234]") == 12);
    CHECK(eventCpu("mce: [Hardware Error]: Machine check events logged") == -1);

    // Storage: a running record is replaced by its own result, another run's is not touched.
    QTemporaryDir tmp;
    qputenv("XDG_STATE_HOME", tmp.path().toUtf8());
    CHECK(load().isEmpty());
    CHECK(store({{"id", 1}, {"verdict", "pass"}}));
    CHECK(store({{"id", 2}, {"verdict", "running"}, {"boot", "b1"}, {"cpu", 0}}));
    CHECK(store({{"id", 2}, {"verdict", "running"}, {"boot", "b1"}, {"cpu", 4}}));
    CHECK(load().size() == 2 && load().last().toObject().value("cpu").toInt() == 4);
    CHECK(store({{"id", 2}, {"verdict", "fail"}}));
    CHECK(load().size() == 2 && load().last().toObject().value("verdict").toString() == "fail");
    for (int i = 0; i < KEEP + 5; ++i) store({{"id", 100 + i}, {"verdict", "pass"}});
    CHECK(load().size() == KEEP && load().first().toObject().value("id").toInt() == 105);

    // A run left "running": crashed when it belongs to another boot, incomplete in this one.
    store({{"id", 500}, {"verdict", "running"}, {"boot", "old"}, {"cpu", 6}});
    QJsonArray crashed = settle("new");
    CHECK(crashed.size() == 1 && crashed[0].toObject().value("cpu").toInt() == 6);
    CHECK(load().last().toObject().value("verdict").toString() == "crashed");
    CHECK(settle("new").isEmpty());
    store({{"id", 501}, {"verdict", "running"}, {"boot", "new"}});
    CHECK(settle("new").isEmpty() && load().last().toObject().value("verdict").toString() == "incomplete");

    // Verification: the newest finished test of the profile with the same content; incomplete runs do not count.
    const QString h = profileHash("{\"coall\":-10}");
    CHECK(h.size() == 16 && h != profileHash("{\"coall\":-15}"));
    QJsonArray all{QJsonObject{{"curve", "profile:daily"}, {"profile_hash", h}, {"verdict", "pass"}, {"t", 1}},
                   QJsonObject{{"curve", "profile:daily"}, {"profile_hash", h}, {"verdict", "incomplete"}, {"t", 2}},
                   QJsonObject{{"curve", "profile:other"}, {"profile_hash", h}, {"verdict", "fail"}, {"t", 3}}};
    CHECK(verification(all, "daily", h).value("t").toInt() == 1);
    CHECK(verification(all, "daily", "0000").isEmpty());  // the profile was edited since
    all.append(QJsonObject{{"curve", "profile:daily"}, {"profile_hash", h}, {"verdict", "crashed"}, {"t", 4}});
    CHECK(verification(all, "daily", h).value("verdict").toString() == "crashed");

    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::printf("cpustress_test: all checks passed\n");
    return failures ? 1 : 0;
}
