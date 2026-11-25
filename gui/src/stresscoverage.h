#pragma once
// What a CPU test "pass" covered: which CPUs, which load, the temperature range seen and for how long.
// A short scan (CO Search probes, a few minutes) is labelled apart from a long verification, so the
// history does not let a quick pass read like an hour of evidence. Absent fields stay "unknown".
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <algorithm>

namespace stresscoverage {

constexpr qint64 LONG_SECONDS = 20 * 60;  // a run at least this long (all cycles) counts as a verification

enum class Kind { ShortScan, LongVerification, Unknown };

inline Kind kind(const QJsonObject &r) {
    const qint64 s = r.value("seconds").toInteger(-1), cycles = std::max<qint64>(1, r.value("cycles").toInteger(1));
    if (s <= 0) return Kind::Unknown;
    // Per-core: each core gets `seconds`, so a long per-core run still gives each core only that much.
    const qint64 perCpu = r.value("mode").toString() == QLatin1String("percore") ? s * cycles : s;
    return perCpu >= LONG_SECONDS ? Kind::LongVerification : Kind::ShortScan;
}

/// "0-3,8" style list of CPU numbers.
inline QString cpuRanges(QList<int> cpus) {
    std::sort(cpus.begin(), cpus.end());
    cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
    QStringList out;
    for (int i = 0; i < cpus.size();) {
        int j = i;
        while (j + 1 < cpus.size() && cpus[j + 1] == cpus[j] + 1) ++j;
        out << (i == j ? QString::number(cpus[i]) : QStringLiteral("%1-%2").arg(cpus[i]).arg(cpus[j]));
        i = j + 1;
    }
    return out.join(QLatin1Char(','));
}

inline QString describe(const QJsonObject &r) {
    QList<int> cpus;
    for (const auto &c : r.value("cpus").toArray()) cpus << c.toInt();
    QStringList p;
    p << (cpus.isEmpty() ? QStringLiteral("CPUs: unknown") : QStringLiteral("CPUs %1 (%2)").arg(cpuRanges(cpus)).arg(cpus.size()));
    p << QStringLiteral("load %1").arg(r.value("load").toString().toUpper());
    if (r.value("temp_min").isDouble() && r.value("temp_max").isDouble())
        p << QStringLiteral("%1–%2 °C").arg(r.value("temp_min").toDouble(), 0, 'f', 0).arg(r.value("temp_max").toDouble(), 0, 'f', 0);
    else p << QStringLiteral("temperature: unknown");
    const qint64 s = r.value("seconds").toInteger(-1);
    p << (s > 0 ? QStringLiteral("%1 s × %2 cycle(s)").arg(s).arg(std::max<qint64>(1, r.value("cycles").toInteger(1))) : QStringLiteral("duration unknown"));
    const Kind k = kind(r);
    p << (k == Kind::LongVerification ? QStringLiteral("long verification") : k == Kind::ShortScan ? QStringLiteral("short scan") : QStringLiteral("length unknown"));
    return p.join(QStringLiteral(" · "));
}

} // namespace stresscoverage
