#pragma once
// ABBA verification result card: what the measurement covered. Blocks done of those planned, why anything
// was left out, which weighted objectives were measured and which were not (e.g. no power sensor), and the
// interval. An objective the user weighted but that has no measurement is never presented as a gain.
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <cmath>

namespace abcoverage {

struct Coverage {
    int blocks = -1, planned = -1;
    QStringList excluded;          // skipped settings and the early-termination reason
    QStringList measured, unmeasured;  // weighted objectives with / without a measured gain
    bool interval = false;
    double lo = 0, hi = 0;
};

inline Coverage of(const QJsonObject &r) {
    Coverage c;
    const auto count = [](const QJsonValue &v) { return v.isDouble() && v.toDouble() >= 0 && std::trunc(v.toDouble()) == v.toDouble() ? int(v.toDouble()) : -1; };
    c.blocks = count(r.value("rounds"));
    c.planned = count(r.value("planned_rounds"));
    for (const auto &s : r.value("skipped").toArray()) c.excluded << (s.isString() ? s.toString() : QStringLiteral("(unnamed setting)"));
    if (r.value("ended_early").isString()) c.excluded << QStringLiteral("ended early: ") + r.value("ended_early").toString();
    const QJsonObject objectives = r.value("objectives").toObject();
    const QJsonObject weights = r.value("weights").toObject();
    for (const char *k : {"latency", "throughput", "power", "footprint"}) {
        const QString key = QLatin1String(k);
        const bool has = objectives.value(key).isArray() && objectives.value(key).toArray().size() == 2;
        if (has) c.measured << key;
        else if (weights.value(key).toDouble(0) > 0) c.unmeasured << key;
    }
    for (auto it = objectives.begin(); it != objectives.end(); ++it)  // phase-specific objectives the list above does not name
        if (!c.measured.contains(it.key()) && it.value().isArray()) c.measured << it.key();
    const auto ci = r.value("decision_ci").toArray().isEmpty() ? r.value("ci95").toArray() : r.value("decision_ci").toArray();
    if (ci.size() == 2 && ci[0].isDouble() && ci[1].isDouble() && std::isfinite(ci[0].toDouble()) && std::isfinite(ci[1].toDouble())
        && ci[0].toDouble() <= ci[1].toDouble()) {
        c.interval = true; c.lo = ci[0].toDouble(); c.hi = ci[1].toDouble();
    }
    return c;
}

inline QStringList lines(const Coverage &c) {
    QStringList out;
    out << QStringLiteral("Blocks: %1 of %2 planned complete")
               .arg(c.blocks < 0 ? QStringLiteral("unknown") : QString::number(c.blocks), c.planned < 0 ? QStringLiteral("unknown") : QString::number(c.planned));
    out << QStringLiteral("Excluded: %1").arg(c.excluded.isEmpty() ? QStringLiteral("nothing") : c.excluded.join(QStringLiteral("; ")));
    out << QStringLiteral("Measured objectives: %1").arg(c.measured.isEmpty() ? QStringLiteral("none") : c.measured.join(QStringLiteral(", ")));
    if (!c.unmeasured.isEmpty())
        out << QStringLiteral("Weighted but not measurable here (no claim about them): %1").arg(c.unmeasured.join(QStringLiteral(", ")));
    out << (c.interval ? QStringLiteral("Decision interval: %1 … %2").arg(c.lo, 0, 'g', 6).arg(c.hi, 0, 'g', 6)
                       : QStringLiteral("Decision interval: not available"));
    return out;
}

} // namespace abcoverage
