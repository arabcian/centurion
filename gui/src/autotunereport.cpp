#include "autotunereport.h"

#include <QStringList>

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
    if (g.size() != 2) g = d.value(QStringLiteral("alone")).toArray();
    if (g.size() == 2) p << QStringLiteral("%1 ± %2").arg(g[0].toDouble(), 0, 'f', 3).arg(g[1].toDouble(), 0, 'f', 3).prepend(g[0].toDouble() >= 0 ? QStringLiteral("+") : QString());
    if (const double n = d.value(QStringLiteral("n")).toDouble(); n > 0) p << QStringLiteral("n %1").arg(n, 0, 'g', 3);
    if (d.value(QStringLiteral("seen")).isBool() && !d.value(QStringLiteral("seen")).toBool()) p << QStringLiteral("unseen");
    return p.join(QStringLiteral(" · "));
}

QString scoreTrace(const QJsonArray &trace) {
    QStringList p;
    for (const auto &v : trace) {
        const QJsonObject o = v.toObject();
        const QJsonValue val = o.value(QStringLiteral("value"));
        const QString s = val.isString() ? val.toString() : QString::number(val.toDouble(), 'g', 12);
        p << QStringLiteral("%1 %2").arg(s).arg(o.value(QStringLiteral("u")).toDouble(), 0, 'f', 2);
    }
    return p.join(QStringLiteral(" → "));
}

}
