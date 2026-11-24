#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <optional>

namespace ryzenprofilediff {
// Compare by SMU slot instead of file array order. Never infer missing offsets
// from all-core or fill with zero; the two apply modes have different semantics.
inline std::optional<QJsonObject> normalize(const QJsonObject &profile) {
    if (!profile.value("cores").isUndefined() && !profile.value("cores").isArray()) return std::nullopt;
    const auto cores=profile.value("cores").toArray();
    if(cores.size()>512)return std::nullopt;
    const auto validOffset=[](const QJsonValue &v){const auto x=v.toInt(21);return v.isUndefined()||v.isNull()||(x>=-50&&x<=20);};
    if(!validOffset(profile.value("coall")))return std::nullopt;
    QJsonObject slotMap;
    for(const auto &v:cores){
        if(!v.isObject())return std::nullopt;
        auto c=v.toObject();
        const auto ccd=c.value("ccd").toInt(-1), ccx=c.contains("ccx")?c.value("ccx").toInt(-1):0;
        const auto slot=c.contains("slot")?c.value("slot").toInt(-1):c.value("core").toInt(-1);
        if(ccd<0||ccd>511||ccx<0||ccx>511||slot<0||slot>511||!validOffset(c.value("coper"))
           ||(c.contains("disabled")&&!c.value("disabled").isBool())
           ||(c.contains("slot")&&c.contains("core")&&c.value("core").toInt(-1)!=slot))return std::nullopt;
        const QString key=QStringLiteral("CCD %1 / CCX %2 / Slot %3").arg(ccd).arg(ccx).arg(slot);
        if(slotMap.contains(key))return std::nullopt;
        c.remove("ccd");c.remove("ccx");c.remove("slot");c.remove("core");
        slotMap[key]=c;
    }
    auto out=profile;if(profile.contains("cores"))out["cores"]=slotMap;return out;
}
} // namespace ryzenprofilediff
