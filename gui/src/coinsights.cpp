#include "coinsights.h"
#include "costrategy.h"

#include <algorithm>

namespace coinsights {

std::optional<double> changePercent(double base, double now) {
    if (base <= 0 || now < 0) return std::nullopt;
    return (now - base) / base * 100.0;
}

std::optional<double> boostShare(int observedMhz, int ratedMhz) {
    if (observedMhz <= 0 || ratedMhz <= 0) return std::nullopt;
    return double(observedMhz) / ratedMhz;
}

QList<int> rankAnomalies(const QList<int> &ranks, const QList<int> &values) {
    QList<int> idx;
    for (int i = 0; i < std::min(ranks.size(), values.size()); ++i)
        if (ranks[i] >= 0 && values[i] < 0) idx << i;
    if (idx.size() < 4) return {};
    QList<int> sorted;
    for (int i : idx) sorted << ranks[i];
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted.size() % 2 ? sorted[sorted.size() / 2]
                                            : (sorted[sorted.size() / 2 - 1] + sorted[sorted.size() / 2]) / 2.0;
    std::optional<int> deepestLow;  // most negative result among the cores ranked below the median
    for (int i : idx)
        if (ranks[i] < median) deepestLow = deepestLow ? std::min(*deepestLow, values[i]) : values[i];
    if (!deepestLow) return {};
    QList<int> out;
    for (int i : idx)
        if (ranks[i] > median && values[i] < *deepestLow) out << i;
    return out;
}

int etaMinutes(int elapsedMin, int coresDone, int coresTotal, int fallbackTotalMin, bool verificationLeft) {
    const int verify = verificationLeft ? cosearch::VERIFY_SECONDS / 60 : 0;
    if (coresDone <= 0) return std::max(0, fallbackTotalMin - elapsedMin);
    const int left = std::max(0, coresTotal - coresDone);
    return int(double(elapsedMin) / coresDone * left + 0.5) + verify;
}

}  // namespace coinsights
