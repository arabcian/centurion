#!/usr/bin/env python3
"""Source-level Centurion palette regression checks (does not replace a Qt build)."""
import re
import colorsys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
src = (root / 'gui/src/theme.cpp').read_text()
header = (root / 'gui/src/theme.h').read_text()
ui = (root / 'gui/src/hometab.cpp').read_text()

start = src.index('const Palette PALETTES[]')
end = src.index('QString g_current')
rows = re.findall(r'\{"([a-z0-9-]+)", "([^"]+)", \{([^{}]+)\}\}', src[start:end])
assert len(rows) == 57, f'Expected 57 themes, got {len(rows)}'
ids = [row[0] for row in rows]
assert len(ids) == len(set(ids)), 'Theme ids must be unique'
assert ids[0] == 'graphite', 'Default theme changed'
assert ids[:21] == [
    'graphite', 'gruvbox', 'tokyo-night', 'horizon', 'oxocarbon',
    'monokai-pro', 'rose-pine', 'kanagawa-dragon', 'everforest',
    'synthwave', 'crimson', 'glacier', 'ember', 'mocha', 'bordeaux',
    'plum', 'moss', 'jade', 'olive', 'sakura', 'phosphor',
], 'Existing theme ids/order changed'
required = {
    'catppuccin-mocha', 'catppuccin-macchiato', 'catppuccin-frappe',
    'catppuccin-latte', 'nord', 'dracula', 'one-dark', 'solarized-dark',
    'solarized-light', 'gruvbox-light', 'rose-pine-dawn',
    'kanagawa-wave', 'ayu-mirage', 'github-dark', 'nightfox',
}
assert set(ids[21:36]) == required, 'Existing 15 community themes changed'
pastel_ids = [
    'pastel-mint-cream', 'pastel-peach-sorbet', 'pastel-lilac-mist',
    'pastel-sage-linen', 'pastel-buttercream', 'pastel-rosewater',
    'pastel-pistachio-milk', 'pastel-coral-sand', 'pastel-sea-glass',
    'pastel-orchid-milk', 'pastel-sky-cotton', 'pastel-sandstone',
    'pastel-dusty-orchid', 'pastel-eucalyptus-night',
    'pastel-cinnamon-cocoa', 'pastel-dusty-coral', 'pastel-marigold-ink',
    'pastel-mulberry-velvet', 'pastel-olive-smoke', 'pastel-opal-lagoon',
]
assert ids[36:56] == pastel_ids, 'Pastel themes missing or in unexpected order'
assert sum('Pastel Light' in name for _, name, _ in rows[36:56]) == 12
assert sum('Pastel Dark' in name for _, name, _ in rows[36:56]) == 8


def luminance(color):
    values = [int(color[i:i + 2], 16) / 255.0 for i in (1, 3, 5)]
    r, g, b = (v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4 for v in values)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def ratio(one, two):
    lo, hi = sorted((luminance(one), luminance(two)))
    return (hi + 0.05) / (lo + 0.05)


# Confirm the pastel accent choices span the colour wheel instead of adding
# another dozen nearly identical blues. 45-degree hue sectors are sufficient.
accent_sectors = set()

for ident, display_name, values in rows:
    colors = re.findall(r'"(#[0-9a-fA-F]{6})"', values)
    assert len(colors) == 19, (ident, len(colors))
    for idx, color in enumerate(colors):
        assert color.lower() == color, (ident, idx, color)
    if ident in required:
        # All text should clear standard 4.5:1 on the main card; mute text is
        # intentionally secondary but must remain legible at 3:1.
        assert ratio(colors[8], colors[1]) >= 4.5, (ident, 'text')
        assert ratio(colors[10], colors[1]) >= 3.0, (ident, 'muted')
        assert ratio(colors[11], colors[1]) >= 3.0, (ident, 'accent')
    if ident in pastel_ids:
        # Both the text and the accent must remain readable on the deepest
        # control surface, not only on a card background.
        assert ratio(colors[8], colors[4]) >= 4.5, (ident, 'text on control')
        assert ratio(colors[9], colors[1]) >= 4.5, (ident, 'secondary text')
        assert ratio(colors[10], colors[4]) >= 3.0, (ident, 'muted on control')
        assert ratio(colors[11], colors[4]) >= 3.0, (ident, 'accent on control')
        assert ratio(colors[8], colors[1]) >= 4.5, (ident, 'text on card')
        assert ratio(colors[10], colors[1]) >= 3.0, (ident, 'muted on card')
        # Qt QColor::lightnessF(): HSL lightness, not perceived luminance.
        bg_rgb = [int(colors[0][i:i+2], 16) / 255 for i in (1, 3, 5)]
        bg_lightness = (min(bg_rgb) + max(bg_rgb)) / 2
        if 'Pastel Light' in display_name:
            assert bg_lightness > 0.62, (ident, 'incorrect light theme detection')
            assert luminance(colors[5]) > luminance(colors[0]), (ident, 'well should be lighter')
            assert all(luminance(colors[i]) > luminance(colors[i+1]) for i in range(4)), (ident, 'surface ladder')
        else:
            assert bg_lightness < 0.62, (ident, 'incorrect dark theme detection')
            assert luminance(colors[5]) < luminance(colors[0]), (ident, 'well should be darker')
            assert all(luminance(colors[i]) < luminance(colors[i+1]) for i in range(4)), (ident, 'surface ladder')
        accent_rgb = [int(colors[11][i:i+2], 16) / 255 for i in (1, 3, 5)]
        hue, _, _ = colorsys.rgb_to_hsv(*accent_rgb)
        accent_sectors.add(int(hue * 8) % 8)

assert ids[-1] == 'oled-black'
oled = re.findall(r'"(#[0-9a-fA-F]{6})"', rows[-1][2])
assert oled[0] == '#000000' and oled[5] == '#000000'
for bg in oled[:5] + [oled[5]]:
    for fg in oled[8:11]: assert ratio(fg,bg) >= 4.5, (fg,bg)
assert len(accent_sectors) >= 7, ('Pastel hue coverage too narrow', accent_sectors)


def legible(color, bg):
    """Mirror of theme.cpp legible(): darken a fixed colour in 10 % steps on a lighter background up to 3:1."""
    rgb = [int(color[i:i + 2], 16) / 255.0 for i in (1, 3, 5)]
    hexed = lambda v: '#' + ''.join(f'{round(x * 255):02x}' for x in v)
    for _ in range(20):
        if not (luminance(bg) > luminance(hexed(rgb)) and ratio(hexed(rgb), bg) < 3.0):
            break
        rgb = [x * 0.9 for x in rgb]
    return hexed(rgb)


# The two profile colours that are fixed hex values (not palette tokens) stay visible on every palette's
# profile button (BG2): darkened on the light ones, unchanged on the dark ones.
for fixed in ('#7cc6bd', '#e070b0'):
    assert f'legible("{fixed}")' in src, fixed
    for ident, display_name, values in rows:
        colors = re.findall(r'"(#[0-9a-fA-F]{6})"', values)
        shown = legible(fixed, colors[2])
        if luminance(colors[2]) > luminance(fixed):
            assert ratio(shown, colors[2]) >= 3.0, (ident, fixed, shown)
        else:
            assert shown == fixed, (ident, fixed, shown)

assert 'QIcon themePreviewIcon(const QString &id);' in header
assert 'theme::themePreviewIcon(id)' in ui
assert 'themeBox->setIconSize(' in ui
assert 'theme::saveTheme(id)' in ui
assert 'QSettings::UserScope' in src
assert 'if (!knownPalette(id)) return false;' in src
assert 'textOnAccent()' in src and 'static bool lightTheme()' in src
assert 'p.setColor(QPalette::HighlightedText, textOnAccent())' in src
assert '@HILITE' in src and 'isLight ?' in src
print(f'PASS {len(rows)} themes (36 preserved + 20 pastel + OLED), 12 light / 8 dark; contrast, hue coverage, persistence, previews')
