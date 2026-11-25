#pragma once
// The values of a curve journal line (ocjournal.rs: "values", "fp") as text: which core got which
// offset. Read-only presentation; nothing here authorizes an Apply.
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <algorithm>
#include <array>
#include <optional>

namespace curvevalues {

inline QString tr(const char *s) { return QCoreApplication::translate("curvevalues", s); }

inline QString number(const QJsonValue &v) {
    if (v.isDouble()) { const double d = v.toDouble(); return d == qint64(d) ? QString::number(qint64(d)) : QString::number(d, 'g', 6); }
    if (v.isString()) return v.toString();
    if (v.isBool()) return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return QString::fromUtf8(QJsonDocument(v.isArray() ? QJsonDocument(v.toArray()) : QJsonDocument(v.toObject())).toJson(QJsonDocument::Compact));
}

/// A Curve Optimizer key "ccd.ccx.core" as (ccd, ccx, core); nullopt for anything else.
inline std::optional<std::array<int, 3>> coKey(const QString &k) {
    const QStringList p = k.split(QLatin1Char('.'));
    if (p.size() != 3) return std::nullopt;
    std::array<int, 3> out{};
    for (int i = 0; i < 3; ++i) { bool ok = false; out[size_t(i)] = p.at(i).toInt(&ok); if (!ok || out[size_t(i)] < 0 || out[size_t(i)] > 511) return std::nullopt; }
    return out;
}

/// One line per value: "CCD0 core 3: -20" for a per-core Curve Optimizer state, "All cores: -20", else "key: value".
inline QStringList lines(const QString &kind, const QJsonValue &values) {
    QStringList out;
    if (!values.isObject()) return values.isUndefined() || values.isNull() ? out : QStringList{number(values)};
    const QJsonObject o = values.toObject();
    if (kind == QLatin1String("cpu_co")) {
        if (o.contains(QStringLiteral("all"))) out << tr("All cores: %1").arg(number(o.value(QStringLiteral("all"))));
        QList<std::pair<std::array<int, 3>, QString>> cores;
        for (auto it = o.begin(); it != o.end(); ++it)
            if (const auto k = coKey(it.key())) cores.append({*k, number(it.value())});
        std::sort(cores.begin(), cores.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        for (const auto &[k, v] : cores)
            out << (k[1] ? tr("CCD%1 CCX%2 core %3: %4").arg(k[0]).arg(k[1]).arg(k[2]).arg(v) : tr("CCD%1 core %2: %3").arg(k[0]).arg(k[2]).arg(v));
        return out;
    }
    for (auto it = o.begin(); it != o.end(); ++it) out << QStringLiteral("%1: %2").arg(it.key(), number(it.value()));
    return out;
}

/// The Curve Optimizer offset of one physical core (ccd, slot) in a recorded state: its per-core value,
/// else the all-core value; nullopt when the state does not say.
inline std::optional<int> coOffset(const QJsonValue &values, int ccd, int slot) {
    const QJsonObject o = values.toObject();
    const QJsonValue v = o.value(QStringLiteral("%1.0.%2").arg(ccd).arg(slot));
    if (v.isDouble()) return int(v.toDouble());
    if (o.value(QStringLiteral("all")).isDouble()) return int(o.value(QStringLiteral("all")).toDouble());
    return std::nullopt;
}

/// "'Daily' #1f2e3d4c": the state with its fingerprint, so equal names with other values read apart.
inline QString label(const QString &prettyState, const QString &fp) {
    return fp.isEmpty() ? prettyState : QStringLiteral("%1 #%2").arg(prettyState, fp);
}

}  // namespace curvevalues
