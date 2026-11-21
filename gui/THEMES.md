# Centurion colour themes

The Home screen **Theme** selector controls the look of the entire Qt application, for both the **Experimental** and **Classic** interfaces. Pick a theme, then accept the restart prompt, or restart the application later. The selection is saved in `~/.config/centurion/gui.ini` under the `theme` key.

Centurion keeps all 21 existing themes and their saved IDs. The following **15 new presets** have been added:

| New theme | Appearance |
| --- | --- |
| Catppuccin Mocha | Dark, mauve on charcoal-blue |
| Catppuccin Macchiato | Dark, lavender on muted navy |
| Catppuccin Frappé | Dark, violet on slate |
| Catppuccin Latte | Light, violet on cool white |
| Nord | Dark, icy cyan and polar navy |
| Dracula | Dark, purple and electric accents |
| One Dark | Dark, blue on desaturated charcoal |
| Solarized Dark | Dark, blue and teal on petrol |
| Solarized Light | Light, deep-blue on warm cream |
| Gruvbox Light | Light, warm orange on parchment |
| Rosé Pine Dawn | Light, pink on muted cream |
| Kanagawa Wave | Dark, blue and wood-toned accents |
| Ayu Mirage | Dark, gold on deep slate |
| GitHub Dark | Dark, blue on near-black |
| Nightfox | Dark, cool blue with forest-green highlights |

The theme selector displays an inline three-colour swatch (background, text and accent) for each preset. A colour change does not change system power profiles, tuning data, or hardware settings.

**For developers:** palettes are defined centrally in `gui/src/theme.cpp`; each preset contains exactly 19 tokens used by the Qt application stylesheet. Light themes use contrast-aware active-selection text and hover surfaces. Run `python gui/tests/theme_palettes_static_test.py` to validate the palette table without building Qt. This is a *static* test, not a GUI rendering or compilation check.
