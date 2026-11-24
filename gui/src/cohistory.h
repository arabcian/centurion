#pragma once
#include "cohistoryfields.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>
#include <QMap>
#include <QPair>
#include <QString>

namespace cohistory {
inline QString eventId(const QJsonObject &o) {
    const auto id = o.value("event_id").toString();
    return !QUuid(id).isNull() && QUuid(id).toString(QUuid::WithoutBraces) == id ? id : QString();
}
inline Result outcome(const QJsonObject &o) {
    const auto s = o.value("observed_result").toString();
    const auto pass = o.value("pass");
    if (s == QStringLiteral("pass") && pass.isBool() && pass.toBool()) return Result::Passed;
    if (s == QStringLiteral("fail") && pass.isBool() && !pass.toBool()) return Result::Failed;
    if (s == QStringLiteral("interrupted") && (!pass.isBool() || !pass.toBool())) return Result::Interrupted;
    return Result::Unknown;
}
// An isolated observation must name the same physical slot as the frozen topology.
// Never infer isolation from a final verification with one nonzero offset, or from a legacy row.
inline Summary summarizeRecord(const QJsonObject &record) {
    const auto rows = record.value("topology").toArray();
    QMap<QPair<int,int>, int> keys;
    for (const auto &v : rows) {
        const auto c = v.toObject();
        const auto ccd = c.value("ccd").toInt(-1), slot = c.value("slot").toInt(-1);
        if (ccd >= 0 && ccd <= 511 && slot >= 0 && slot <= 511) ++keys[{ccd,slot}];
    }
    const auto attempts = record.value("attempts").toArray();
    std::vector<Observation> obs;
    // Count duplicate IDs across ALL scopes; a conflicting whole-curve record cannot hide a duplicate.
    QMap<QString,int> ids;
    for (const auto &v : attempts) { const auto id = eventId(v.toObject()); if (!id.isEmpty()) ++ids[id]; }
    std::size_t rejected = 0;
    for (const auto &v : attempts) {
        const auto o = v.toObject();
        const auto id = eventId(o);
        const auto ccd = o.value("ccd").toInt(-1), slot = o.value("slot").toInt(-1);
        const auto index = o.value("core").toInt(-1);
        const auto offset = o.value("value").toInt(1);
        const auto boot = o.value("event_boot").toString();
        if (record.value("mode").toString() != QStringLiteral("per") || o.value("scope").toString() != QStringLiteral("isolated")
            || id.isEmpty() || ids.value(id) != 1 || QUuid(boot).isNull() || QUuid(boot).toString(QUuid::WithoutBraces)!=boot || index < 0 || index >= rows.size()
            || keys.value({ccd,slot}) != 1) { ++rejected; continue; }
        const auto c = rows.at(index).toObject();
        if (c.value("ccd").toInt(-1) != ccd || c.value("slot").toInt(-1) != slot
            || c.value("excluded").toBool()) { ++rejected; continue; }
        obs.push_back({id.toStdString(),ccd,slot,offset,outcome(o)});
    }
    auto result = summarize(obs); result.rejected += rejected; return result;
}
} // namespace cohistory
