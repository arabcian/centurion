#!/usr/bin/env python3
"""Source-level Centurion palette regression checks (does not replace a Qt build)."""
import re
from pathlib import Path

root = Path(__file__).resolve().parents[2]
src = (root / 'gui/src/theme.cpp').read_text()
header = (root / 'gui/src/theme.h').read_text()
ui = (root / 'gui/src/hometab.cpp').read_text()

start = src.index('const Palette PALETTES[]')
end = src.index('QString g_current')
rows = re.findall(r'\{"([a-z0-9-]+)", "([^"]+)", \{([^{}]+)\}\}', src[start:end])
assert len(rows) == 36, f'Expected 36 themes, got {len(rows)}'
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
assert set(ids[21:]) == required, 'Missing or unexpected new themes'


def luminance(color):
    values = [int(color[i:i + 2], 16) / 255.0 for i in (1, 3, 5)]
    r, g, b = (v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4 for v in values)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def ratio(one, two):
    lo, hi = sorted((luminance(one), luminance(two)))
    return (hi + 0.05) / (lo + 0.05)


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

assert 'QIcon themePreviewIcon(const QString &id);' in header
assert 'theme::themePreviewIcon(id)' in ui
assert 'themeBox->setIconSize(' in ui
assert 'theme::saveTheme(id)' in ui
assert 'QSettings::UserScope' in src
assert 'if (!found) return false;' in src
assert 'textOnAccent()' in src and 'static bool lightTheme()' in src
assert 'p.setColor(QPalette::HighlightedText, textOnAccent())' in src
assert '@HILITE' in src and 'isLight ?' in src
print(f'PASS {len(rows)} themes, 15 new; contrast, persistence, previews and legacy IDs')
