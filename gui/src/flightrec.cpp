#include "flightrec.h"
#include "diagnostichistory.h"
#include <QJsonDocument>
#include <QMap>
#include <limits>

#include <QCoreApplication>
#include <QDateTime>
#include <QStringList>

namespace flightrec {

QJsonObject parseRing(const QByteArray &bytes) {
    if (bytes.size() < SLOT_BYTES || bytes.size() > RING_BYTES) return {};
    const auto slot = [&bytes](qsizetype index) {
        const qsizetype end = (index + 1) * SLOT_BYTES;
        if (end > bytes.size() || bytes.at(end - 1) != '\n') return QJsonObject{};
        return QJsonDocument::fromJson(bytes.mid(end - SLOT_BYTES, SLOT_BYTES - 1)).object();
    };
    const auto header = slot(0);
    const auto boot = diagnostichistory::fullBoot(header.value("boot").toString().toStdString());
    const auto interval = header.value("interval").toInteger();
    if (boot.empty() || header.contains("s") || interval <= 0 || interval > std::numeric_limits<int>::max()) return {};
    QMap<qint64, QJsonObject> ordered;
    const auto count = bytes.size() / SLOT_BYTES - 1;
    for (qsizetype i = 0; i < count; ++i) {
        const auto sample = slot(i + 1);
        if (sample.contains("boot")) continue;
        const auto seq = sample.value("s").toInteger(-1);
        if (seq < 0 || seq >= (1LL << 53) || seq % SAMPLE_SLOTS != i) continue;
        const auto time = sample.value("t").toInteger();
        if (time <= 0 || time >= 253402300800LL) continue;
        ordered.insert(seq, sample);
    }
    QJsonArray samples;
    for (const auto &sample : ordered) samples.append(sample);
    return {{"boot", QString::fromStdString(boot)}, {"interval", interval}, {"samples", samples}};
}

// The flight recorder's samples (crates/centurion-helpers/src/flight.rs): the peaks of the whole
// stretch, then the last ROWS rows. A missing reading is shown as "—".
QString text(const QJsonArray &samples) {
    auto cell = [](const QJsonObject &o, const char *k, int w) {
        const QJsonValue v = o.value(QLatin1String(k));
        return (v.isDouble() ? QString::number(v.toDouble(), 'f', v.toDouble() == qRound(v.toDouble()) ? 0 : 1) : QStringLiteral("—")).rightJustified(w);
    };
    auto peak = [&](const char *k) {
        double m = -1;
        for (const QJsonValue &v : samples) m = qMax(m, v.toObject().value(QLatin1String(k)).toDouble(-1));
        return m < 0 ? QStringLiteral("—") : QString::number(m, 'f', 0);
    };
    const qint64 first = qint64(samples.first().toObject().value(QStringLiteral("t")).toDouble());
    const qint64 last = qint64(samples.last().toObject().value(QStringLiteral("t")).toDouble());
    QString out = QCoreApplication::translate("BootPage", "── flight recorder: %1 sample(s), %2 – %3 (the last one is the last moment it was alive) ──\n")
                      .arg(samples.size()).arg(QDateTime::fromSecsSinceEpoch(first).toString(QStringLiteral("HH:mm:ss")),
                                               QDateTime::fromSecsSinceEpoch(last).toString(QStringLiteral("HH:mm:ss")));
    out += QCoreApplication::translate("BootPage", "Peaks: CPU %1 °C, GPU %2 °C, CPU package %3 W, load %4\n")
               .arg(peak("cpu_c"), peak("gpu_c"), peak("pkg_w"), peak("load"));
    out += QCoreApplication::translate("BootPage", "time       CPU °C  GPU °C  CPU MHz  CPU W  Batt W  Load  dGPU       Fans (rpm)\n");
    for (qsizetype i = qMax<qsizetype>(0, samples.size() - ROWS); i < samples.size(); ++i) {
        const QJsonObject o = samples.at(i).toObject();
        QStringList fans;
        for (const QJsonValue &f : o.value(QStringLiteral("fan")).toArray()) fans << QString::number(f.toInt());
        out += QStringLiteral("%1 %2 %3 %4 %5 %6 %7  %8 %9\n")
                   .arg(QDateTime::fromSecsSinceEpoch(qint64(o.value(QStringLiteral("t")).toDouble())).toString(QStringLiteral("HH:mm:ss")),
                        cell(o, "cpu_c", 7), cell(o, "gpu_c", 7), cell(o, "mhz", 8), cell(o, "pkg_w", 6), cell(o, "bat_w", 7),
                        cell(o, "load", 5), o.value(QStringLiteral("dgpu")).toString(QStringLiteral("—")).leftJustified(10),
                        fans.isEmpty() ? QStringLiteral("—") : fans.join(QLatin1Char('/')));
    }
    return out;
}

} // namespace flightrec
