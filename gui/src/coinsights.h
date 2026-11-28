#pragma once
// Read-only judgements CO Search shows next to its results (no widgets, no I/O), kept apart so they
// can be unit-tested (tests/coinsights_test.cpp). None of them changes what the search tests or saves.
#include <QList>
#include <optional>

namespace coinsights {

/// Single-core throughput (work chunks in the same time) with the found curve, against stock.
/// Below this change in percent a core is flagged: clock stretching, or a lower boost than at stock.
inline constexpr double STRETCH_PERCENT = -2.0;
/// Change of `now` against `base` in percent; nullopt when there is no usable baseline.
std::optional<double> changePercent(double base, double now);

/// Peak clock seen while a core was probed alone, as a share of its rated maximum.
/// Below this share the core's "pass" is flagged as weaker evidence: it never ran near its top boost.
inline constexpr double LOW_BOOST = 0.95;
std::optional<double> boostShare(int observedMhz, int ratedMhz);

/// Cores (indices) whose CPPC rank is above the median of the ranked, undervolted cores and whose result is
/// more negative than that of every core ranked below the median. The best cores usually hold the least
/// negative offsets, so such a result deserves a longer test. Needs at least 4 ranked, undervolted cores.
QList<int> rankAnomalies(const QList<int> &ranks, const QList<int> &values);

/// Per-core search: minutes left, from the pace of the cores finished so far (`fallbackTotal` before the
/// first one finishes), plus the 30-minute verification while it is still ahead.
int etaMinutes(int elapsedMin, int coresDone, int coresTotal, int fallbackTotalMin, bool verificationLeft);

}  // namespace coinsights
