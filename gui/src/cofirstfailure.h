#pragma once
// The first wrong result of a CO Search trial, with what is needed to run it again: the core and its CO value,
// the kernel that was running, its data seed, the wait before the failing wake burst and the exact test command.
// Built from centurion-cpustress JSON lines; fields the tool did not report stay absent (never guessed).
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QStringList>

namespace cofirstfailure {

/// `start`: the tool's "start" line (kernels list); `error`: the first "error" line; `lastPhase`: the newest
/// "phase" line seen before it (may be empty); `coreCpus`: logical CPUs of each core, in core order;
/// `coValues`: the trial's CO value per core (same order; empty = all-core value `allCore`).
inline QJsonObject build(const QJsonObject &start, const QJsonObject &error, const QJsonObject &lastPhase,
                         const QList<QList<int>> &coreCpus, const QList<int> &coValues, int allCore, const QStringList &args) {
    QJsonObject r{{"t", error.value("t")}, {"cpu", error.value("cpu")}, {"args", QJsonArray::fromStringList(args)}};
    const int cpu = error.value("cpu").toInt(-1);
    for (int i = 0; i < coreCpus.size(); ++i)
        if (coreCpus[i].contains(cpu)) {
            r["core"] = i;
            r["co"] = coValues.isEmpty() ? allCore : coValues.value(i);
            break;
        }
    if (error.value("data_seed").isDouble()) r["data_seed"] = error.value("data_seed");
    if (error.value("idle_ms").isDouble()) r["idle_ms"] = error.value("idle_ms");
    const auto kernels = start.value("kernels").toArray();
    if (error.value("kernel_index").isDouble()) {
        const int k = error.value("kernel_index").toInt();
        if (k >= 0 && k < kernels.size()) r["kernel"] = kernels.at(k);
        else if (k == kernels.size() && !kernels.isEmpty()) r["kernel"] = QStringLiteral("wake-fft-sse");  // the wake block follows the steady ones
    }
    if (!lastPhase.isEmpty()) {
        r["after_phase"] = lastPhase.value("phase");
        r["cycle"] = lastPhase.value("cycle");
    }
    return r;
}

/// One line for the log / tooltip.
inline QString describe(const QJsonObject &r) {
    QStringList parts;
    if (r.contains("core")) parts << QStringLiteral("core %1 at CO %2").arg(r.value("core").toInt()).arg(r.value("co").toInt());
    parts << QStringLiteral("CPU %1").arg(r.value("cpu").toInt());
    if (r.contains("kernel")) parts << QStringLiteral("kernel %1").arg(r.value("kernel").toString());
    if (r.contains("data_seed")) parts << QStringLiteral("seed %1").arg(r.value("data_seed").toInt());
    if (r.contains("idle_ms")) parts << QStringLiteral("after %1 ms idle").arg(r.value("idle_ms").toInt());
    if (r.contains("after_phase")) parts << QStringLiteral("after phase %1").arg(r.value("after_phase").toString());
    return parts.join(QStringLiteral(" · "));
}

} // namespace cofirstfailure
