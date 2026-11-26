// Product-report features (docs/REPORT_FEATURES_2026-11-25.md): the Qt-side presentation logic.
// Plain asserts, like autotunereport_test.cpp.
#include "../src/abcoverage.h"
#include "../src/autotunereport.h"
#include "../src/cofirstfailure.h"
#include "../src/coolcompare.h"
#include "../src/fieldverify.h"
#include "../src/incidentfocus.h"
#include "../src/stresscoverage.h"
#include "../src/uvtransitions.h"
#include "../src/vfcoveragehistory.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static QJsonObject obj(const char *json) { return QJsonDocument::fromJson(json).object(); }
static QJsonArray arr(const char *json) { return QJsonDocument::fromJson(json).array(); }

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    // V/F coverage survives reopening, with exact GPU, curve and point identity.
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const auto path = dir.filePath(QStringLiteral("coverage.json"));
    auto gpu = obj(R"({"schema":1,"mapping":"single_visible_physical_gpu","uuid":"GPU-12345678-abcd-1234-abcd-123456789abc","pci":"0000:00:01.0","driver":"590.1","vbios":"V1"})");
    const auto points = arr(R"([{"domain":"gpu","index":1,"volt_uV":650000,"freq_kHz":800100},{"domain":"gpu","index":2,"volt_uV":750000,"freq_kHz":1200000}])");
    const auto curve = vfcoverage::fingerprint(gpu, points);
    CHECK(!curve.isEmpty());
    auto changedPoints = points;
    auto point = changedPoints.at(0).toObject();
    point["freq_kHz"] = 800101;
    changedPoints.replace(0, point);
    CHECK(vfcoverage::fingerprint(gpu, changedPoints) != curve);
    gpu["uuid"] = QStringLiteral("GPU-12345678-abcd-1234-abcd-123456789abd");
    CHECK(vfcoverage::fingerprint(gpu, points) != curve);
    const vfcoverage::Session first{curve, QDateTime::fromString(QStringLiteral("2026-01-01T10:00:00.000Z"), Qt::ISODateWithMs),
        {650000, 750000}, {{1, 3}, 4}};
    CHECK(vfcoverage::saveSession(path, first));
    const auto loaded = vfcoverage::loadHistory(path);
    CHECK(loaded && loaded->size() == 1);
    if (!loaded || loaded->isEmpty()) return 1;
    CHECK(vfcoverage::comparable(first, loaded->first()));
    CHECK(loaded->first().coverage.samples == 4 && loaded->first().coverage.counts == first.coverage.counts);
    const auto [lo, hi] = vfcoverage::estimatedRange(loaded->first());
    CHECK(lo == 650000 && hi == 750000);
    auto different = loaded->first();
    different.curve = vfcoverage::fingerprint(gpu, points);
    CHECK(!vfcoverage::comparable(first, different));
    different.coverage.counts[0] = vfcoverage::MaxSamples;
    CHECK(!vfcoverage::saveSession(path, different));
    for (int i = 0; i < vfcoverage::HistoryLimit; ++i) {
        auto next = first;
        next.started = first.started.addSecs(i + 1);
        CHECK(vfcoverage::saveSession(path, next));
    }
    const auto retained = vfcoverage::loadHistory(path);
    CHECK(retained && retained->size() == vfcoverage::HistoryLimit
        && retained->first().started == first.started.addSecs(1));
    CHECK(diagsrc::saveJson(path, {{"version", 99}}).isEmpty());
    CHECK(!vfcoverage::saveSession(path, first));
    CHECK(diagsrc::loadJson(path).value("version").toInt() == 99);

    // ── cooling: two runs compared only under equal conditions ──
    const auto a = obj(R"({"t":1,"key":"k","noisy":false,"secs":300,"start_temp":45,"steady_temp":88,"max_temp":91,"fan_rpm":4200,"mhz":4000,"limit_secs":30})");
    auto b = obj(R"({"t":2,"key":"k","noisy":false,"secs":300,"start_temp":47,"steady_temp":84,"max_temp":87,"fan_rpm":5000,"mhz":3800})");
    CHECK(coolcompare::blocker(a, b).isEmpty());
    const auto d = coolcompare::compare(a, b);
    CHECK(d && *d->steadyTemp == -4 && *d->fanRpm == 800 && std::abs(*d->mhzPct + 5.0) < 1e-9);
    CHECK(!d->limitSecs);  // older run without the figure: not recorded, not zero
    b["start_temp"] = 49; CHECK(coolcompare::blocker(a, b) == QLatin1String("start_temp"));
    b["start_temp"] = 47; b["key"] = "other"; CHECK(coolcompare::blocker(a, b) == QLatin1String("context"));
    b["key"] = "k"; b["noisy"] = true; CHECK(coolcompare::blocker(a, b) == QLatin1String("noisy"));
    b["noisy"] = false; b["secs"] = 120; CHECK(coolcompare::blocker(a, b) == QLatin1String("duration"));

    // ── CO Search: first failure record ──
    const auto start = obj(R"({"ev":"start","kernels":["fft-sse","fft-avx2","ntt"]})");
    auto err = obj(R"({"ev":"error","t":12.5,"cpu":9,"seed":24,"kernel_index":3,"data_seed":0,"idle_ms":180})");
    const auto ff = cofirstfailure::build(start, err, obj(R"({"phase":"wake","cycle":0})"), {{0, 8}, {1, 9}}, {0, -25}, 0, {"percore", "--load", "co"});
    CHECK(ff.value("core").toInt() == 1 && ff.value("co").toInt() == -25);
    CHECK(ff.value("kernel").toString() == QLatin1String("wake-fft-sse") && ff.value("idle_ms").toInt() == 180 && ff.value("data_seed").toInt() == 0);
    CHECK(ff.value("args").toArray().size() == 3 && ff.value("after_phase").toString() == QLatin1String("wake"));
    err = obj(R"({"ev":"error","t":1,"cpu":0,"seed":9,"kernel_index":1,"data_seed":1})");
    const auto ff2 = cofirstfailure::build(start, err, {}, {{0, 8}}, {}, -20, {});
    CHECK(ff2.value("kernel").toString() == QLatin1String("fft-avx2") && ff2.value("co").toInt() == -20 && !ff2.contains("idle_ms"));
    CHECK(cofirstfailure::describe(ff2).contains(QLatin1String("seed 1")));
    const auto ff3 = cofirstfailure::build(start, obj(R"({"ev":"error","cpu":77})"), {}, {{0}}, {}, 0, {});
    CHECK(!ff3.contains("core") && !ff3.contains("kernel"));  // unknown stays absent

    // ── Intel undervolt: context transitions ──
    const auto rows = uvtransitions::parse(R"({"version":1,"entries":[
        {"t":10,"event":"boot","source":"AC","ok":true,"fields":[{"what":"core","ok":true,"message":"-80 mV","locked":false}]},
        {"t":20,"event":"resume","source":"BATTERY","ok":false,"fields":[{"what":"pl1","ok":false,"message":"locked","locked":true},{"what":"core","ok":true}]}]})");
    CHECK(rows.size() == 2 && rows[0].event == QLatin1String("resume") && !rows[0].ok && rows[0].locked == 1 && rows[0].fields == 2);
    CHECK(rows[1].ok && rows[1].failed.isEmpty());
    CHECK(uvtransitions::parse(R"({"version":2,"entries":[{"ok":true}]})").isEmpty());
    CHECK(uvtransitions::parse("not json").isEmpty());

    // ── AMD GPU: revert read-back ──
    const auto v = fieldverify::compare(obj(R"({"sclk_max":2400,"voltage_offset":0,"power_cap_w":100})"), obj(R"({"sclk_max":2400,"voltage_offset":-30})"));
    CHECK(v.matched == QStringList{"sclk_max"} && v.differs == QStringList{"voltage_offset"} && v.unreadable == QStringList{"power_cap_w"} && !v.allMatched());
    CHECK(fieldverify::compare(obj(R"({"a":1})"), obj(R"({"a":1,"b":2})")).allMatched());

    // ── Autotune: trade-off row and weight what-if ──
    const auto trace = arr(R"j([
        {"value":"(leave)","u":0,"fx":{"latency":0,"throughput":0,"power":0,"footprint":0,"stability":0},"modesty":0},
        {"value":"fast","u":0.2,"fx":{"latency":0.3,"throughput":0.1,"power":-0.4,"footprint":0,"stability":0},"modesty":0.02},
        {"value":"save","u":0.1,"fx":{"latency":-0.05,"throughput":0,"power":0.3,"footprint":0,"stability":0},"modesty":0.01}])j");
    CHECK(autotunereport::tradeoff(trace[1].toObject()) == QStringLiteral("latency +0.30 · throughput +0.10 · power -0.40"));
    const auto gaming = obj(R"({"latency":1,"throughput":1,"power":0.2,"footprint":0,"stability":1})");
    const auto battery = obj(R"({"latency":0.3,"throughput":0.3,"power":1.5,"footprint":0,"stability":1})");
    CHECK(autotunereport::winnerUnder(trace, gaming) == 1);
    CHECK(autotunereport::winnerUnder(trace, battery) == 2);
    CHECK(autotunereport::decidingObjective(trace, gaming, battery) == QLatin1String("power"));
    CHECK(autotunereport::decidingObjective(trace, gaming, gaming).isEmpty());
    CHECK(autotunereport::winnerUnder(arr(R"([{"value":1,"u":0}])"), gaming) == -1);  // old traces: no what-if

    // ── ABBA coverage ──
    const auto cov = abcoverage::of(obj(R"({"rounds":3,"planned_rounds":4,"skipped":["vm.swappiness"],"ended_early":"thermal",
        "objectives":{"latency":[0.1,0.02],"throughput":[0.05,0.01]},"weights":{"latency":1,"throughput":1,"power":0.5,"footprint":0},
        "decision_ci":[0.01,0.2]})"));
    CHECK(cov.blocks == 3 && cov.planned == 4 && cov.excluded.size() == 2);
    CHECK(cov.measured == (QStringList{"latency", "throughput"}) && cov.unmeasured == QStringList{"power"});
    CHECK(cov.interval && cov.lo == 0.01 && cov.hi == 0.2);
    CHECK(abcoverage::lines(cov).join('\n').contains(QLatin1String("not measurable here (no claim about them): power")));
    CHECK(!abcoverage::of(obj(R"({"decision_ci":[0.3,0.1]})")).interval);

    // ── CPU test coverage ──
    const auto pass = obj(R"({"mode":"allcore","load":"avx2","seconds":1800,"cycles":1,"cpus":[0,1,2,3,8],"temp_min":52,"temp_max":88.4})");
    CHECK(stresscoverage::kind(pass) == stresscoverage::Kind::LongVerification);
    CHECK(stresscoverage::describe(pass) == QStringLiteral("CPUs 0-3,8 (5) · load AVX2 · 52–88 °C · 1800 s × 1 cycle(s) · long verification"));
    CHECK(stresscoverage::kind(obj(R"({"mode":"percore","seconds":24,"cycles":3})")) == stresscoverage::Kind::ShortScan);
    CHECK(stresscoverage::kind(obj(R"({"mode":"percore","seconds":600,"cycles":2})")) == stresscoverage::Kind::LongVerification);
    CHECK(stresscoverage::kind(obj("{}")) == stresscoverage::Kind::Unknown);
    CHECK(stresscoverage::describe(obj(R"({"load":"sse","seconds":60})")).contains(QLatin1String("temperature: unknown")));

    // ── Evidence Lab incident focus ──
    const auto c = incidentfocus::candidates(QStringLiteral("MCE on cpu5 bank 5; AER at 0000:01:00.0, also 01:00.0; Xid 79 then CPU 5 again; xid79"));
    CHECK(c == (QStringList{"CPU 5", "bank 5", "0000:01:00.0", "01:00.0", "Xid 79"}));
    CHECK(incidentfocus::filterFor(QStringLiteral("0000:01:00.0")) == QLatin1String("01:00.0"));
    CHECK(incidentfocus::filterFor(QStringLiteral(" CPU 5 ")) == QLatin1String("CPU 5"));
    CHECK(incidentfocus::candidates(QString()).isEmpty());

    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::puts("report_features: all checks passed");
    return failures ? 1 : 0;
}
