#include "autotunereport.h"

#include <QStringList>
#include <cmath>
#include <optional>

namespace autotunereport {

QHash<QString, QJsonObject> decisionsByKey(const QJsonObject &report) {
    QHash<QString, QJsonObject> out;
    for (const auto &v : report.value(QStringLiteral("decisions")).toArray()) {
        const QJsonObject o = v.toObject();
        out.insert(o.value(QStringLiteral("key")).toString(), o);
    }
    return out;
}

QString badge(const QString &source) {
    static const QHash<QString, QString> names{
        {QStringLiteral("structural"), QStringLiteral("Rule")}, {QStringLiteral("estimate"), QStringLiteral("Estimate")},
        {QStringLiteral("measured"), QStringLiteral("Measured")}, {QStringLiteral("joint"), QStringLiteral("Joint")},
        {QStringLiteral("pinned"), QStringLiteral("Pinned")}, {QStringLiteral("evidence"), QStringLiteral("Evidence")},
        {QStringLiteral("audit"), QStringLiteral("Audit")}, {QStringLiteral("retired"), QStringLiteral("Retired")}};
    return names.value(source, source);
}

QString confidence(const QJsonObject &d) {
    QStringList p;
    QJsonArray g = d.value(QStringLiteral("in_context")).toArray();
    auto valid = [](const QJsonArray &a) { return a.size() == 2 && a[0].isDouble() && a[1].isDouble() && std::isfinite(a[0].toDouble()) && std::isfinite(a[1].toDouble()) && a[1].toDouble() >= 0; };
    if (!valid(g)) g = d.value(QStringLiteral("alone")).toArray();
    if (valid(g)) p << QStringLiteral("%1 ± %2").arg(g[0].toDouble(), 0, 'f', 3).arg(g[1].toDouble(), 0, 'f', 3).prepend(g[0].toDouble() >= 0 ? QStringLiteral("+") : QString());
    if (const double n = d.value(QStringLiteral("n")).toDouble(); std::isfinite(n) && n > 0 && std::trunc(n) == n) p << QStringLiteral("n %1").arg(n, 0, 'g', 3);
    if (d.value(QStringLiteral("seen")).isBool() && !d.value(QStringLiteral("seen")).toBool()) p << QStringLiteral("unseen");
    return p.join(QStringLiteral(" · "));
}

QHash<QString, QString> changedSinceLast(const QJsonObject &report) {
    QHash<QString, QString> out;
    for (const auto &v : report.value(QStringLiteral("diff_vs_last")).toArray()) {
        const QJsonObject o = v.toObject();
        const QJsonValue from = o.value(QStringLiteral("from"));
        const QString was = from.isNull() || from.isUndefined() ? QStringLiteral("not set") : from.isString() ? from.toString() : QString::number(from.toDouble(), 'g', 12);
        out.insert(o.value(QStringLiteral("key")).toString(), QStringLiteral("was %1 (%2)").arg(was, o.value(QStringLiteral("why")).toString()));
    }
    return out;
}

QString scoreTrace(const QJsonArray &trace) {
    QStringList p;
    for (const auto &v : trace) {
        const QJsonObject o = v.toObject();
        const QJsonValue val = o.value(QStringLiteral("value"));
        const QString s = val.isString() ? val.toString() : QString::number(val.toDouble(), 'g', 12);
        const auto score = o.value(QStringLiteral("u"));
        if ((!val.isString() && !val.isDouble()) || !score.isDouble() || !std::isfinite(score.toDouble())) continue;
        p << QStringLiteral("%1 %2").arg(s).arg(score.toDouble(), 0, 'f', 2);
    }
    return p.join(QStringLiteral(" → "));
}


static const char *const OBJECTIVES[] = {"latency", "throughput", "power", "footprint", "stability"};

QString tradeoff(const QJsonObject &candidate) {
    const QJsonObject fx = candidate.value(QStringLiteral("fx")).toObject();
    QStringList p;
    for (const char *k : OBJECTIVES) {
        const QJsonValue v = fx.value(QLatin1String(k));
        if (v.isDouble() && std::isfinite(v.toDouble()) && std::abs(v.toDouble()) >= 0.005)
            p << QStringLiteral("%1 %2").arg(QLatin1String(k)).arg(QString::asprintf("%+.2f", v.toDouble()));
    }
    return p.join(QStringLiteral(" · "));
}

static std::optional<double> utility(const QJsonObject &c, const QJsonObject &w) {
    const QJsonObject fx = c.value(QStringLiteral("fx")).toObject();
    if (fx.isEmpty()) return std::nullopt;
    double u = -c.value(QStringLiteral("modesty")).toDouble(0);
    for (const char *k : OBJECTIVES) u += w.value(QLatin1String(k)).toDouble(0) * fx.value(QLatin1String(k)).toDouble(0);
    return u;
}

int winnerUnder(const QJsonArray &trace, const QJsonObject &weights) {
    int best = -1;
    double bestU = 0;
    for (int i = 0; i < trace.size(); ++i) {
        const auto u = utility(trace.at(i).toObject(), weights);
        if (!u) return -1;
        if (best < 0 || *u > bestU) { best = i; bestU = *u; }
    }
    return best;
}

QString decidingObjective(const QJsonArray &trace, const QJsonObject &before, const QJsonObject &after) {
    const int a = winnerUnder(trace, before), b = winnerUnder(trace, after);
    if (a < 0 || b < 0 || a == b) return {};
    const QJsonObject fa = trace.at(a).toObject().value(QStringLiteral("fx")).toObject();
    const QJsonObject fb = trace.at(b).toObject().value(QStringLiteral("fx")).toObject();
    QString which;
    double most = 0;
    for (const char *k : OBJECTIVES) {
        const double dw = after.value(QLatin1String(k)).toDouble(0) - before.value(QLatin1String(k)).toDouble(0);
        const double gain = dw * (fb.value(QLatin1String(k)).toDouble(0) - fa.value(QLatin1String(k)).toDouble(0));
        if (gain > most) { most = gain; which = QLatin1String(k); }
    }
    return which;
}

}

