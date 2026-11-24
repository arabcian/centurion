#pragma once
// Presentation arithmetic only: never decides whether a model may be used.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <optional>
#include <limits>

namespace calibrationmath {
struct Prediction { double error, z; bool withinTwoSigma; };
inline std::optional<Prediction> prediction(double mu, double sd, double obs) {
    if (!std::isfinite(mu) || !std::isfinite(sd) || !std::isfinite(obs) || sd <= 0) return {};
    const long double error = static_cast<long double>(obs) - mu;
    const long double z = error / sd;
    const auto max = std::numeric_limits<double>::max();
    if (!std::isfinite(error) || !std::isfinite(z) || std::abs(error) > max || std::abs(z) > max) return {};
    return Prediction{static_cast<double>(error), static_cast<double>(z), std::abs(z) <= 2};
}
// Same asymmetric tolerance as calib::Fingerprint::matches: the saved RAM is the denominator.
inline bool ramMatches(std::uint64_t saved, std::uint64_t current) {
    const auto diff = saved > current ? saved-current : current-saved;
    return diff <= std::max<std::uint64_t>(saved,1)/50;
}
inline double low(std::size_t index) { return index == 4 ? 0.5 : 0.; }
inline double high(std::size_t index) { return index >= 6 ? 1. : 3.; }
inline bool validWeight(std::size_t index, double v) {
    return index < 9 && std::isfinite(v) && v >= low(index) && v <= high(index);
}
// The editor omits a value equal to the default; the helper then adapts that weight.
inline std::optional<double> effectiveWeight(std::size_t index, double edit, double base, double adapted) {
    if (!validWeight(index, edit) || !validWeight(index, base) || !validWeight(index, adapted)) return {};
    return std::abs(edit-base) <= 1e-6 ? adapted : edit;
}
}
