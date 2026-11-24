#pragma once
// User-local, bounded evidence. Never authorize Apply, Reset or rollback from
// journal contents. A successful helper reply is not a fresh readback.
#include "curvereadback.h"
#include "tunejournal.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
namespace curvejournal {
inline QJsonObject readResult(const QString &path) {
    QFile f(path);
    constexpr qint64 cap = 1024 * 1024;
    if (!f.open(QIODevice::ReadOnly) || f.size() > cap) return {};
    const auto bytes = f.read(cap+1);
    if (bytes.size() > cap) return {};
    return QJsonDocument::fromJson(bytes).object();
}
inline QJsonObject inspect(const QJsonObject &request, const QJsonObject &result, bool reached, bool ok) {
    if (!reached) return {{"status","unknown"}};
    if (!ok) return {{"status","helper_failed"}};
    const auto id = curvereadback::identity(result,request.value("request_id").toString());
    if (!id) return {{"status","readback_missing"}};
    const auto expected = request.value("expected_device").toObject();
    if (!expected.isEmpty() && expected != id->device) return {{"status","device_changed"}};
    QJsonArray rows;
    const auto data = request.value("profile_data").toObject();
    const auto deltas = data.value("curve_deltas").toObject();
    const auto op = request.value("op").toString();
    const bool edited = op == QStringLiteral("apply_gpu_offsets");
    const bool reset = op == QStringLiteral("reset_gpu_curve");
    const auto flat = data.value("flatten_from");
    bool mismatch = false, unresolved = false;
    if (edited) {
        QJsonArray layout;
        for (qsizetype i=0;i<id->indices.size();++i) layout.append(QJsonArray{id->indices.at(i),id->voltages.at(i)});
        if (layout != request.value("expected_points").toArray() || id->gpu != request.value("expected_gpu").toString()) return {{"status","layout_changed"}};
    }
    for (const auto &v : result.value("vf_curve").toArray()) {
        const auto p = v.toObject();
        if (p.value("domain").toString() != QStringLiteral("gpu")) continue;
        const int index = p.value("index").toInt();
        QJsonObject row{{"index",index},{"volt_uV",p.value("volt_uV")},{"observed_delta_kHz",p.value("freq_offset_kHz")},{"observed_kHz",p.value("freq_kHz")},{"base_kHz",p.value("base_kHz")}};
        if (reset || edited) {
            // Flatten is prepared from the live base in Rust. Do not compare
            // the GUI draft's rounded offset as if it were the requested CT.
            if (edited && flat.isDouble() && index >= flat.toInt()) {
                row["comparison"] = QStringLiteral("flatten_preflight_required"); unresolved = true;
            } else {
                const auto wanted = reset ? 0 : deltas.value(QString::number(index)).toInteger(0);
                row["requested_delta_kHz"] = wanted;
                const bool same = p.value("freq_offset_kHz").toInteger() == wanted;
                row["comparison"] = same ? QStringLiteral("match") : QStringLiteral("mismatch"); mismatch |= !same;
            }
        }
        rows.append(row);
    }
    if (edited) {
        const auto mem = result.value("mem_offset_mhz");
        if (!mem.isDouble() || !std::isfinite(mem.toDouble()) || std::trunc(mem.toDouble()) != mem.toDouble() || std::abs(mem.toDouble()) > 100000) unresolved = true;
        else mismatch |= mem.toInteger() != data.value("mem_offset_mhz").toInteger();
    }
    QString status = QStringLiteral("observed_only");
    if (id->device.isEmpty()) status = QStringLiteral("identity_unavailable");
    else if (mismatch) status = QStringLiteral("point_or_memory_mismatch");
    else if ((edited || reset) && !unresolved) status = QStringLiteral("point_match_only");
    return {{"status",status},{"gpu",id->gpu},{"device",id->device},{"points",rows},{"mem_offset_mhz",result.value("mem_offset_mhz")}};
}
}
