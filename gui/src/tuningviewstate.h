#pragma once
#include <QString>
#include <QJsonValue>
#include <QVariant>
namespace tuningview {
inline QString valueText(const QJsonValue &v) {
    if (v.isString()) return v.toString();
    if (v.isDouble()) return QString::number(v.toVariant().toLongLong());
    return {};
}
enum class BootMatch { NotSaved, Unknown, Matches, Different };
inline BootMatch bootMatch(bool saved, bool readable, const QString &live, const QString &boot) {
    if (!saved) return BootMatch::NotSaved;
    if (!readable || live.isEmpty() || boot.isEmpty()) return BootMatch::Unknown;
    return live == boot ? BootMatch::Matches : BootMatch::Different;
}
} // namespace tuningview
