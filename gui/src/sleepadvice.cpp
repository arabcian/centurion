#include "sleepadvice.h"
#include <QMap>
#include <QSet>
#include <algorithm>

namespace sleepadvice {

Sleep parse(const QJsonObject &o) {
    Sleep s;
    s.start = o.value("start").toInteger();
    s.end = o.value("end").toInteger();
    for (const QString &w : o.value("woke_by").toString().split(',', Qt::SkipEmptyParts)) s.wokeBy << w.trimmed();
    s.statusBefore = o.value("status_before").toString();
    s.energyBefore = o.value("energy_before").toDouble();
    s.energyAfter = o.value("energy_after").toDouble();
    s.full = o.value("full").toDouble();
    s.hasHwSleep = !o.value("hw_sleep_us").toString().isEmpty();
    s.hwSleepUs = o.value("hw_sleep_us").toString().toDouble();
    return s;
}

bool hwSleepIdle(const QList<Sleep> &rows) {
    int counted = 0;
    for (auto it = rows.crbegin(); it != rows.crend() && counted < HW_IDLE_SLEEPS; ++it) {
        const qint64 len = it->end - it->start;
        if (len < HW_IDLE_MIN_S || !it->hasHwSleep) continue;
        if (it->hwSleepUs / 1e6 / double(len) >= HW_IDLE_SHARE) return false;
        ++counted;
    }
    return counted == HW_IDLE_SLEEPS;
}

bool isUserWake(const QString &n) {
    // PNP0C0D lid, PNP0C0C power button, PNP0C0E sleep button, LNXPWRBN / LNXSLPBN fixed buttons.
    for (const char *id : {"PNP0C0D", "PNP0C0C", "PNP0C0E", "LNXPWRBN", "LNXSLPBN"})
        if (n.startsWith(QLatin1String(id), Qt::CaseInsensitive)) return true;
    return n.startsWith(QLatin1String("LID"), Qt::CaseInsensitive) || n.compare(QLatin1String("PWRB"), Qt::CaseInsensitive) == 0
           || n.compare(QLatin1String("SLPB"), Qt::CaseInsensitive) == 0;
}

QList<Advice> analyse(const QList<Sleep> &rows, const std::function<QString(const QString &)> &resolve) {
    QMap<QString, Advice> by;
    for (int i = 0; i < rows.size(); ++i) {
        const Sleep &s = rows.at(i);
        if (s.wokeBy.isEmpty() || std::any_of(s.wokeBy.begin(), s.wokeBy.end(), isUserWake)) continue;
        QSet<QString> devices;
        for (const QString &w : s.wokeBy)
            if (const QString d = resolve(w); !d.isEmpty() && !isUserWake(d)) devices.insert(d);
        if (devices.isEmpty()) continue;

        double pct = 0;
        if (i + 1 < rows.size()) {
            const Sleep &n = rows.at(i + 1);
            const qint64 gap = n.start - s.end;
            const bool battery = s.statusBefore == QLatin1String("Discharging") && n.statusBefore == QLatin1String("Discharging");
            if (battery && gap > 0 && gap <= MAX_AWAKE_S && s.full > 0 && s.energyAfter >= n.energyBefore)
                pct = 100.0 * (s.energyAfter - n.energyBefore) / s.full;
        }
        for (const QString &d : devices) {
            Advice &a = by[d];
            a.device = d;
            ++a.wakes;
            a.awakePct += pct;
        }
    }
    QList<Advice> out;
    for (const Advice &a : by)
        if (a.wakes >= MIN_WAKES) out << a;
    std::sort(out.begin(), out.end(), [](const Advice &a, const Advice &b) {
        return a.awakePct != b.awakePct ? a.awakePct > b.awakePct : a.wakes > b.wakes;
    });
    return out;
}

} // namespace sleepadvice
