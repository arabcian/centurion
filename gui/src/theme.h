#pragma once
// Single source of truth for the look — port of theme.py.
#include <QColor>
#include <QIcon>
#include <QObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <functional>
#include <optional>

class QApplication;

namespace theme {
// Graphite with an amber signal. Surfaces step up in small, even increments
// (window → card → inner panel → control → hover); text you type into sits in
// a recessed WELL, below the card, so "type here" and "press here" read apart
// at a glance. Borders are hairlines that separate, they never frame. Amber is
// reserved for what is active: the selected tab, primary actions, slider fill,
// checked state, focus.
// Values of the active palette (see PALETTES in theme.cpp), set by load()
// before any widget exists. Stable addresses after load(); GUI-thread only.
inline const char *BG0 = "#121316", *BG1 = "#191a1e", *BG2 = "#202126", *BG3 = "#292a30",
    *BG4 = "#34363d", *WELL = "#0e0f11", *BORDER = "#2f3137", *BORDER_SOFT = "#24252a", *FG = "#e8e6e3",
    *FG_DIM = "#b4b2ae", *MUTED = "#9e9ea3", *ACCENT = "#eba55b", *ACCENT_SOFT = "#a9773f",
    *OK = "#8fc486", *WARN = "#e6c065", *DANGER = "#ea8076", *DANGER_SOFT = "#8e4841",
    *INFO = "#7fb1d5", *PURPLE = "#b79be2";
inline constexpr int FONT_PT = 9, RADIUS = 10;
/// Experimental layout scale, on a 4 px grid: margins and gaps in the Experimental
/// shell and pages use these names, not literals. Classic keeps its own values.
inline constexpr int GAP_XS = 4, GAP_S = 8, GAP_M = 12, GAP_L = 16, GAP_XL = 24;
/// Experimental control height: buttons, line edits, combos and spin boxes on one row line up.
inline constexpr int CONTROL_H = 24;
/// Experimental type scale (points at the default 9 pt; the typography preference scales all four).
inline constexpr int TITLE_PT = 13, SECTION_PT = 10, BODY_PT = FONT_PT, SMALL_PT = 8;
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
struct Typography { QString family, numericFamily; int points = FONT_PT; };
Typography typographyPreference();
bool saveTypography(const Typography &value);
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
/// Draft-only correction of the audited opaque token pairs; no settings or hardware writes.
std::optional<QStringList> repairPaletteContrast(const QStringList &colors);
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
class ThemeEvents : public QObject {
    Q_OBJECT
Q_SIGNALS:
    void themeChanged(quint64 revision);
};
ThemeEvents &events();
quint64 revision();
/// Presentation-only refresh bound to the receiver lifetime. Register once at
/// construction; callbacks must not read hardware, mutate drafts or persist settings.
void bindPresentation(QObject *owner, std::function<void()> refresh);
qint64 lastLiveRefreshNanos();
/// In-memory live change for migrated workspaces, not a persistent preference.
/// Callers must refresh cached semantic icons/brushes/local styles on the signal.
bool applyLivePalette(QApplication &app, const QString &id);
/// Persist the selected palette and apply the same validated snapshot in-place.
/// An invalid palette or failed settings sync leaves the running theme unchanged.
bool applyLivePreference(QApplication &app, const QString &id);
bool applyLiveCustomPreference(QApplication &app, const QStringList &colors);
/// Replaceable presentation-only stylesheet binding; one callback per widget.
/// Capture immutable state by value, or read current presentation state at refresh.
void setLiveSheet(QWidget *widget, std::function<QString()> render);
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
