#pragma once
// Preview of a Spectrum (per-key) profile as a 4-zone keyboard would show it.
// The keyboard is split left→right into four equal column bands; each zone takes the mean colour of the
// keys in its band that a Static effect colours. Animated effects, random colours and keys outside the
// main grid cannot be carried over and are counted, not guessed.
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace spectrumzone4 {

struct Rgb { int r = 0, g = 0, b = 0; bool operator==(const Rgb &) const = default; };

struct Key { double column; std::optional<Rgb> color; };  // column: 0 (left edge) .. 1 (right edge)

struct Result {
    std::array<std::optional<Rgb>, 4> zones;  // nullopt = no static colour in that band
    int keysUsed = 0, keysWithoutColor = 0;
    int effectsDropped = 0;                   // animated / random-colour effects that a static 4-zone preview cannot show
};

inline int zoneOf(double column) {
    if (!(column >= 0)) return 0;
    if (column >= 1) return 3;
    return static_cast<int>(column * 4);
}

inline Result convert(const std::vector<Key> &keys, int effectsDropped) {
    Result r;
    r.effectsDropped = effectsDropped;
    std::array<std::array<int64_t, 4>, 4> acc{};  // r, g, b, count per zone
    for (const auto &k : keys) {
        if (!k.color) { ++r.keysWithoutColor; continue; }
        auto &z = acc[zoneOf(k.column)];
        z[0] += k.color->r; z[1] += k.color->g; z[2] += k.color->b; ++z[3];
        ++r.keysUsed;
    }
    for (int i = 0; i < 4; ++i)
        if (acc[i][3] > 0)
            r.zones[i] = Rgb{int((acc[i][0] + acc[i][3] / 2) / acc[i][3]), int((acc[i][1] + acc[i][3] / 2) / acc[i][3]),
                             int((acc[i][2] + acc[i][3] / 2) / acc[i][3])};
    return r;
}

} // namespace spectrumzone4
