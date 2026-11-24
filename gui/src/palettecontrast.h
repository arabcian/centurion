#pragma once
// Opaque sRGB token correction, independent of Qt and hardware. Never alter a
// surface or a token that already passes the audit. Search the original RGB
// ray toward black and white; choose the smallest squared channel displacement.
#include <array>
#include <cmath>
#include <optional>
#include <limits>
#include <algorithm>

namespace palettecontrast {
using Rgb = std::array<unsigned char, 3>;
using Palette = std::array<Rgb, 19>;
inline double luminance(const Rgb &rgb) {
    const auto channel = [](unsigned char b) {
        const double v = b / 255.0;
        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return .2126 * channel(rgb[0]) + .7152 * channel(rgb[1]) + .0722 * channel(rgb[2]);
}
inline double ratio(double a, double b) {
    return (std::max(a,b) + .05) / (std::min(a,b) + .05);
}
inline constexpr std::array<int, 10> foregrounds{8,9,10,11,12,13,14,15,17,18};
inline bool passes(int token, const Rgb &rgb, const std::array<double,6> &surfaces) {
    const double y = luminance(rgb);
    if (token == 12) return ratio(y,surfaces[2]) >= 3.; // soft accent: chart mark only
    for (double bg : surfaces) if (ratio(y,bg) < 4.5) return false;
    return true;
}
inline std::optional<Palette> repair(const Palette &input) {
    Palette output = input;
    std::array<double,6> surfaces{};
    for (int i=0;i<6;++i) surfaces[i] = luminance(input[i]);
    for (int token : foregrounds) {
        const Rgb original = input[token];
        if (passes(token,original,surfaces)) continue;
        int bestDistance = std::numeric_limits<int>::max();
        std::optional<Rgb> best;
        // Integer rounding is deterministic and never leaves the 8-bit range.
        for (int end : {0,255}) for (int step=1;step<=255;++step) {
            Rgb candidate{}; int distance=0;
            for (int ch=0;ch<3;++ch) {
                candidate[ch] = static_cast<unsigned char>((int(original[ch])*(255-step)+end*step+127)/255);
                const int delta = int(candidate[ch])-int(original[ch]);
                distance += delta*delta;
            }
            if (distance < bestDistance && passes(token,candidate,surfaces)) {
                bestDistance=distance; best=candidate;
            }
        }
        if (!best) return std::nullopt; // no partial palette or surface alteration
        output[token]=*best;
    }
    return output;
}
} // namespace palettecontrast
