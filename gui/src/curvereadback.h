#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QVector>
#include <optional>
#include <cmath>
namespace curvereadback {
struct Identity { QString gpu; QVector<int> indices; QVector<qint64> voltages; };
inline std::optional<Identity> identity(const QJsonObject &o, const QString &request) {
    if (request.isEmpty() || o.value("request_id").toString() != request || !o.value("vf_curve").isArray()) return std::nullopt;
    Identity result; result.gpu = o.value("gpu").toString();
    const auto pts = o.value("vf_curve").toArray();
    if (result.gpu.isEmpty() || pts.isEmpty() || pts.size() > 255) return std::nullopt;
    QSet<int> seen;
    auto integer = [](const QJsonValue &v, double lo, double hi) {
        const double n = v.toDouble();
        return v.isDouble() && std::isfinite(n) && std::trunc(n) == n && n >= lo && n <= hi;
    };
    for (const auto &v : pts) {
        const auto p = v.toObject();
        if (p.value("domain").toString() != QStringLiteral("gpu")) continue;
        const auto i = p.value("index"), volt = p.value("volt_uV"), freq = p.value("freq_kHz"), off = p.value("freq_offset_kHz"), base = p.value("base_kHz");
        // Bounds here protect parsing/arithmetic only; the hardware validator remains authoritative.
        if (!integer(i, 0, 254) || !integer(volt, 0, 10000000) || !integer(freq, 0, 100000000) ||
            !integer(off, -100000000, 100000000) || (!base.isNull() && !base.isUndefined() && !integer(base, 0, 100000000)) || seen.contains(i.toInt()) || (!result.indices.isEmpty() && i.toInt() <= result.indices.last())) return std::nullopt;
        seen.insert(i.toInt()); result.indices.append(i.toInt()); result.voltages.append(volt.toInteger());
    }
    if (result.indices.isEmpty()) return std::nullopt;
    return result;
}
}
