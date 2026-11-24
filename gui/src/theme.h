#pragma once
// Single source of truth for the look — port of theme.py.
#include <QColor>
#include <QIcon>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <functional>

class QApplication;

namespace theme {
// Graphite with an amber signal. Surfaces step up in small, even increments
// (window → card → inner panel → control → hover); text you type into sits in
// a recessed WELL, below the card, so "type here" and "press here" read apart
// at a glance. Borders are hairlines that separate, they never frame. Amber is
// reserved for what is active: the selected tab, primary actions, slider fill,
// checked state, focus.
// Values of the active palette (see PALETTES in theme.cpp), set by load()
// before any widget exists; a theme change restarts the app.
inline const char *BG0 = "#121316", *BG1 = "#191a1e", *BG2 = "#202126", *BG3 = "#292a30",
    *BG4 = "#34363d", *WELL = "#0e0f11", *BORDER = "#2f3137", *BORDER_SOFT = "#24252a", *FG = "#e8e6e3",
    *FG_DIM = "#b4b2ae", *MUTED = "#86868d", *ACCENT = "#eba55b", *ACCENT_SOFT = "#a9773f",
    *OK = "#8fc486", *WARN = "#e6c065", *DANGER = "#e8776c", *DANGER_SOFT = "#8e4841",
    *INFO = "#7fb1d5", *PURPLE = "#b79be2";
inline constexpr int FONT_PT = 9, RADIUS = 10;
/// Brand mark at the start of the header bezel (MainWindow): icon size, its x and
/// the gap to the "Centurion" wordmark. The page rail starts at brandInset().
inline constexpr int BRAND_X = 16, BRAND_ICON = 22, BRAND_GAP = 9;
int brandInset();

/// Per-profile colour, after the Legion power-button LED: Quiet/Power Saver
/// blue, Balanced white, Performance red, Custom purple (Extreme: magenta).
QString profileAccent(const QString &profile);

/// Premade palettes: (id, display name), in menu order.
QList<QPair<QString, QString>> themes();
QString currentTheme();
/// Three-colour preview for the theme combobox; no theme changes or I/O.
QIcon themePreviewIcon(const QString &id);
/// Experimental interface is selected by default on this branch; classic remains available.
bool experimentalEnabled();
bool saveUiMode(bool experimental);
/// Density is a startup presentation preference; no hardware state changes.
bool compactDensity();
bool saveCompactDensity(bool compact);
/// Reads the saved choice (~/.config/centurion/gui.ini) and sets the tokens.
void load();
bool saveTheme(const QString &id);
QStringList paletteColors(const QString &id);
QStringList paletteTokenNames();
struct ContrastCheck { QString role, foreground, background; double ratio, target; bool exempt = false; };
/// WCAG sRGB contrast for opaque colors; invalid palette -> no checks.
double contrastRatio(const QColor &a, const QColor &b);
QList<ContrastCheck> contrastChecks(const QStringList &colors);
bool validCustomPalette(const QStringList &colors);
bool saveCustomPalette(const QStringList &colors);
/// Quit, then start again in place with the window open (main() does the exec).
void setRestartGuard(std::function<bool()> guard);
void requestRestart();
bool restartRequested();
/// "rgba(r, g, b, a)" of a theme colour — tints for banners and selections.
QString rgba(const char *hex, double alpha);
/// Callout frame: a faint wash of `color` with a matching hairline. `selector`
/// scopes it (e.g. "#tuneBanner") so child labels keep their own look.
QString banner(const char *color, const QString &selector = QString());
void apply(QApplication &app);
// The same in two steps, for start-up: with the application style sheet already set, every
// addWidget()/addTab() while the window is being built re-matches the whole sheet against the
// subtree it moves (about two thirds of the constructor's time). main() builds the window
// between the two calls, so each widget is matched once.
void applyBase(QApplication &app);
void applySheet(QApplication &app);
/// setStyleSheet() re-polishes the widget (and re-resolves every style rule
/// for it) even when the sheet is identical; the tabs that restyle a label on
/// every poll go through this so an unchanged state costs a string compare.
inline void setSheet(QWidget *w, const QString &css) { if (w->styleSheet() != css) w->setStyleSheet(css); }
}
