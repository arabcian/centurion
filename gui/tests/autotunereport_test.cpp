// Unit test for the autotune report formatting (autotunereport.cpp). Plain asserts, like int64spinbox_test.cpp.
#include "../src/autotunereport.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

using namespace autotunereport;

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QJsonObject rep = QJsonDocument::fromJson(R"json({
        "decisions": [
            {"key": "blk.read_ahead_kb", "value": 1024, "source": "joint", "seen": true, "alone": [0.2, 0.05], "in_context": [0.12, 0.04], "n": 5, "risk": 0},
            {"key": "cpu.epp_boost", "value": "1", "source": "pinned", "seen": null, "alone": null, "in_context": null, "n": null, "risk": 0},
            {"key": "vm.swappiness", "value": 10, "source": "estimate", "seen": false, "alone": null, "in_context": null, "n": null, "risk": -0.1}
        ],
        "scores": {"thp": [{"value": "(leave)", "u": 0}, {"value": "always", "u": -0.3}, {"value": 512, "u": 0.25}]}
    })json").object();

    const auto d = decisionsByKey(rep);
    CHECK(d.size() == 3);
    CHECK(badge(d["blk.read_ahead_kb"]["source"].toString()) == "Joint");
    CHECK(badge("structural") == "Rule" && badge("measured") == "Measured" && badge("retired") == "Retired");
    CHECK(badge("future") == "future");
    CHECK(confidence(d["blk.read_ahead_kb"]) == "+0.120 ± 0.040 · n 5");
    CHECK(confidence(d["cpu.epp_boost"]).isEmpty());
    CHECK(confidence(d["vm.swappiness"]) == "unseen");
    CHECK(confidence(QJsonObject{{"alone", QJsonArray{-0.05, 0.01}}}) == "-0.050 ± 0.010");
    CHECK(scoreTrace(rep["scores"]["thp"].toArray()) == "(leave) 0.00 → always -0.30 → 512 0.25");
    const QJsonObject withDiff = QJsonDocument::fromJson(R"json({"diff_vs_last": [
        {"key": "vm.swappiness", "from": 60, "value": 10, "why": "new data"},
        {"key": "blk.nomerges", "from": null, "value": 2, "why": "weights, context"}]})json").object();
    const auto ch = changedSinceLast(withDiff);
    CHECK(ch.size() == 2);
    CHECK(ch.value("vm.swappiness") == "was 60 (new data)");
    CHECK(ch.value("blk.nomerges") == "was not set (weights, context)");
    CHECK(changedSinceLast(rep).isEmpty());
    // A report from before the field: no decisions, the dialog keeps its old layout.
    CHECK(decisionsByKey(QJsonObject{{"rationale", QJsonObject{}}}).isEmpty());

    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::puts("autotunereport: ok");
    return failures ? 1 : 0;
}
