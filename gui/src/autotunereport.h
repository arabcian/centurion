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
// Score trace "madvise 0.00 → always -0.30" (value → U).
QString scoreTrace(const QJsonArray &trace);
}
