#pragma once
// Descriptive measurement coverage, NOT the calibration model's validity decision.
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QStringList>
#include <QSet>
#include <algorithm>
#include <array>
#include <cmath>

namespace calibrationcoverage {
struct Group {
    QStringList fields;
    qsizetype rows = 0;
    std::array<qsizetype, 4> measured{};
    qsizetype unknownWeight = 0;
    qint64 latest = 0; // Unix seconds; 0 = unavailable
    QSet<int> benchmarkVersions;
    qsizetype unknownBenchmark = 0;
};
inline QList<Group> summarize(const QJsonArray &rows) {
    QMap<QString, Group> groups;
    for (const auto &v : rows) {
        if (!v.isObject()) continue;
        const auto row = v.toObject();
        const auto phase = row.value("ph");
        const int p = phase.toInt(-1);
        const auto context = row.value("cx").toString();
        const QStringList fields{p >= 0 && p <= 3 ? QString::number(p) : QString(),
            row.value("k").toString(), context, row.value("ps").toString()};
        auto &g = groups[QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(fields)).toJson(QJsonDocument::Compact))];
        g.fields = fields; ++g.rows;
        const auto y = row.value("y").toArray();
        for (int i = 0; i < 4 && i < y.size(); ++i)
            if (y.at(i).isDouble() && std::isfinite(y.at(i).toDouble())) ++g.measured[i];
        const auto t = row.value("t");
        const double seconds = t.toDouble(-1);
        if (t.isDouble() && seconds > 0 && seconds <= 253402300799. && std::trunc(seconds)==seconds)
            g.latest=std::max(g.latest,static_cast<qint64>(seconds));
        const int bv=row.value("bv").toInt(-1);
        if(bv>=1 && bv<=255) g.benchmarkVersions.insert(bv); else ++g.unknownBenchmark;
        const auto w = row.value("w");
        if (!w.isDouble() || !std::isfinite(w.toDouble()) || w.toDouble() <= 0) ++g.unknownWeight;
    }
    return groups.values();
}
} // namespace calibrationcoverage
