#pragma once
// Two cooling runs side by side (Health → Cooling → Compare two runs), e.g. one per fan curve.
// Only runs measured under the same firmware context, both clean, the same duration and a similar
// starting temperature are compared; otherwise the reason is returned and no difference is shown.
#include <QJsonObject>
#include <QString>
#include <cmath>
#include <optional>

namespace coolcompare {

constexpr double MAX_START_DIFF_C = 3.0;

struct Delta {
    std::optional<double> steadyTemp, maxTemp, fanRpm, mhzPct, limitSecs;  // b - a (clock: percent)
};

inline std::optional<double> num(const QJsonObject &o, const char *key) {
    const auto v = o.value(QLatin1String(key));
    if (!v.isDouble() || !std::isfinite(v.toDouble())) return std::nullopt;
    return v.toDouble();
}

/// Empty = comparable; else a short English reason code: "context", "noisy", "duration", "start_temp", "missing".
inline QString blocker(const QJsonObject &a, const QJsonObject &b) {
    if (a.value("key").toString().isEmpty() || a.value("key") != b.value("key")) return QStringLiteral("context");
    if (a.value("noisy").toBool() || b.value("noisy").toBool()) return QStringLiteral("noisy");
    if (!num(a, "secs") || num(a, "secs") != num(b, "secs")) return QStringLiteral("duration");
    const auto sa = num(a, "start_temp"), sb = num(b, "start_temp");
    if (!sa || !sb || !num(a, "steady_temp") || !num(b, "steady_temp")) return QStringLiteral("missing");
    if (std::abs(*sa - *sb) > MAX_START_DIFF_C) return QStringLiteral("start_temp");
    return {};
}

inline std::optional<Delta> compare(const QJsonObject &a, const QJsonObject &b) {
    if (!blocker(a, b).isEmpty()) return std::nullopt;
    const auto diff = [&](const char *k) -> std::optional<double> {
        const auto x = num(a, k), y = num(b, k);
        if (!x || !y) return std::nullopt;
        return *y - *x;
    };
    Delta d{diff("steady_temp"), diff("max_temp"), diff("fan_rpm"), std::nullopt, diff("limit_secs")};
    if (const auto x = num(a, "mhz"), y = num(b, "mhz"); x && y && *x > 0) d.mhzPct = 100.0 * (*y / *x - 1.0);
    return d;
}

} // namespace coolcompare
