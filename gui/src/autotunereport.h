// Formatting of the autotune report's structured fields (centurion-autotune <goal> --json:
// "decisions", "scores", "notes"). Pure, so tests/autotunereport_test.cpp covers it.
#pragma once
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace autotunereport {
// key -> decision object; empty when the JSON predates the field (old report layout then).
QHash<QString, QJsonObject> decisionsByKey(const QJsonObject &report);
// "Rule", "Estimate", "Measured", "Joint", "Pinned", "Evidence", "Audit", "Retired".
QString badge(const QString &source);
// "+0.120 ± 0.040" (in context, else alone), "n 5" and "unseen" when the benchmarks cannot see it.
QString confidence(const QJsonObject &decision);
// key -> "was 512 (new data)" for the keys that changed since the last run ("diff_vs_last").
QHash<QString, QString> changedSinceLast(const QJsonObject &report);
// Score trace "madvise 0.00 → always -0.30" (value → U).
QString scoreTrace(const QJsonArray &trace);

// Trade-off row of one candidate in a score trace: "latency +0.10 · throughput −0.02 · power +0.05".
// Empty for traces from before per-objective effects were recorded.
QString tradeoff(const QJsonObject &candidate);

// What-if for one knob: the trace's winner under `weights` (keys as Weights::KEYS: latency, throughput, power,
// footprint, stability). U = Σ weight × effect − modesty, the helper's own formula. Index into `trace`
// (0 = reference), -1 without per-objective effects.
int winnerUnder(const QJsonArray &trace, const QJsonObject &weights);

// Why the winner changes between two weight sets: the objective whose weight change moved the
// new winner's lead over the old winner the most ("power"); empty when the winner stays.
QString decidingObjective(const QJsonArray &trace, const QJsonObject &before, const QJsonObject &after);
}
