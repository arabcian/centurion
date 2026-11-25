#pragma once
// Requested settings compared field by field with what the driver reports afterwards (AMD GPU trial revert).
// A field the driver no longer exposes (e.g. after a driver reset) is "unreadable", never counted as restored.
#include <QJsonObject>
#include <QStringList>

namespace fieldverify {

struct Result { QStringList matched, differs, unreadable; bool allMatched() const { return differs.isEmpty() && unreadable.isEmpty(); } };

inline Result compare(const QJsonObject &requested, const QJsonObject &readBack) {
    Result r;
    for (auto it = requested.begin(); it != requested.end(); ++it) {
        if (!readBack.contains(it.key())) r.unreadable << it.key();
        else if (readBack.value(it.key()) == it.value()) r.matched << it.key();
        else r.differs << it.key();
    }
    return r;
}

} // namespace fieldverify
