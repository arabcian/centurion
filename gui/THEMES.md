# Centurion colour themes

The Home screen **Theme** selector controls the look of the entire Qt application, for both the **Experimental** and **Classic** interfaces. Pick a theme to apply it in place. The selector, gallery and valid custom palette save use the same live preference path; changing UI mode, density, typography or language is still a separate restart preference. Legacy diagnostic rich text and cached brushes/icons may still need presentation migration. The selection is saved in `~/.config/centurion/gui.ini` under the `theme` key.

Centurion keeps all 36 previous themes (the original 21 plus 15 community presets) and their saved IDs. Those 15 community presets are:

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


## 20 additional pastel colourways

The newest set deliberately favours **non-blue hues** and uses pastel-tinted *surfaces*,
not just a differently coloured accent. All 20 options work in Classic and Experimental UIs.

| New theme | Style / dominant hue |
| --- | --- |
| Mint Cream | Light · pale mint and deep jade |
| Peach Sorbet | Light · peach / warm apricot |
| Lilac Mist | Light · lilac / muted violet |
| Sage Linen | Light · sage green |
| Buttercream | Light · buttery yellow / honey |
| Rosewater | Light · dusty pink / rose |
| Pistachio Milk | Light · pistachio / olive |
| Coral Sand | Light · warm coral / sandstone |
| Sea Glass | Light · seafoam / jade teal |
| Orchid Milk | Light · pale orchid / mauve |
| Sky Cotton | Light · powder blue (one cool blue option) |
| Sandstone | Light · beige / warm neutral |
| Dusty Orchid | Dark · smoky violet and pastel orchid |
| Eucalyptus Night | Dark · forest green and eucalyptus |
| Cinnamon Cocoa | Dark · cocoa brown and cinnamon |
| Dusty Coral | Dark · dusty brick / pale coral |
| Marigold Ink | Dark · olive-black and soft marigold |
| Mulberry Velvet | Dark · dark berry and pastel mauve |
| Olive Smoke | Dark · olive-charcoal / soft lime |
| Opal Lagoon | Dark · green-tinted graphite / seafoam |

Pastel backgrounds are intentionally soft, while the main accent is a little darker in
light themes for contrast. Every new pastel palette passes static checks for text,
secondary/muted labels, accent readability on the darkest/lightest control surface,
surface ordering, correct light/dark classification, and hue diversity.

**Total: 57 themes** (36 unchanged + 12 light pastels + 8 dark pastels + OLED Black). OLED Black uses a true black background and subdued, contrasting text; it is an appearance option, not a burn-in prevention feature.

The theme selector displays an inline three-colour swatch (background, text and accent) for each preset. A colour change does not change system power profiles, tuning data, or hardware settings.

**For developers:** palettes are defined centrally in `gui/src/theme.cpp`; each preset contains exactly 19 tokens used by the Qt application stylesheet. Light themes use contrast-aware active-selection text and hover surfaces. Run `python gui/tests/theme_palettes_static_test.py` to validate the palette table without building Qt. This is a *static* test, not a GUI rendering or compilation check.
