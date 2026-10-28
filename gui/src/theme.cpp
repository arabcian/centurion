#include "theme.h"
#include <QApplication>
#include <QFontMetrics>
#include <QHash>
#include <QPalette>
#include <QProxyStyle>
#include <QSettings>
#include <QStyleFactory>

namespace theme {

namespace {
struct Palette {
    const char *id, *name;
    //          BG0  BG1  BG2  BG3  BG4  WELL BORDER BORDER_SOFT FG  FG_DIM MUTED ACCENT ACCENT_SOFT
    //          OK   WARN DANGER DANGER_SOFT INFO PURPLE
    const char *c[19];
};
const Palette PALETTES[] = {
    {"graphite", "Graphite", {"#121316", "#191a1e", "#202126", "#292a30", "#34363d", "#0e0f11", "#2f3137", "#24252a",
        "#e8e6e3", "#b4b2ae", "#86868d", "#eba55b", "#a9773f", "#8fc486", "#e6c065", "#e8776c", "#8e4841", "#7fb1d5", "#b79be2"}},
    {"gruvbox", "Gruvbox", {"#1d2021", "#282828", "#302e2d", "#3c3836", "#504945", "#161819", "#45403d", "#34302e",
        "#ebdbb2", "#d5c4a1", "#928374", "#fe8019", "#b35f1c", "#b8bb26", "#fabd2f", "#fb4934", "#8f2a22", "#83a598", "#d3869b"}},
    {"tokyo-night", "Tokyo Night", {"#16161e", "#1a1b26", "#1f2335", "#292e42", "#343a55", "#121218", "#2f344d", "#232639",
        "#c0caf5", "#a9b1d6", "#737aa2", "#7aa2f7", "#4b6cb7", "#9ece6a", "#e0af68", "#f7768e", "#8c4050", "#7dcfff", "#bb9af7"}},
    {"horizon", "Horizon", {"#16171d", "#1c1e26", "#232530", "#2e303e", "#393b4d", "#121318", "#3d3f52", "#272936",
        "#fdf0ed", "#d5d8da", "#6c6f93", "#e95678", "#93384b", "#29d398", "#fab795", "#f43e5c", "#8a2536", "#26bbd9", "#ee64ac"}},
    {"oxocarbon", "Oxocarbon", {"#0f0f0f", "#161616", "#1e1e1e", "#262626", "#393939", "#0b0b0b", "#353535", "#222222",
        "#f2f4f8", "#dde1e6", "#8d8d8d", "#42be65", "#2a7a41", "#08bdba", "#f1c21b", "#ee5396", "#8a2f56", "#78a9ff", "#be95ff"}},
    {"monokai-pro", "Monokai Pro", {"#19181a", "#221f22", "#2d2a2e", "#363337", "#403e41", "#151416", "#444145", "#2f2c30",
        "#fcfcfa", "#c1c0c0", "#939293", "#ffd866", "#a88f45", "#a9dc76", "#fc9867", "#ff6188", "#9a3b55", "#78dce8", "#ab9df2"}},
    {"rose-pine", "Rosé Pine", {"#13111c", "#191724", "#1f1d2e", "#26233a", "#312e48", "#100e17", "#403d52", "#21202e",
        "#e0def4", "#908caa", "#6e6a86", "#ebbcba", "#9a7a79", "#9ccfd8", "#f6c177", "#eb6f92", "#8f4259", "#569fba", "#c4a7e7"}},
    {"kanagawa-dragon", "Kanagawa Dragon", {"#0d0c0c", "#181616", "#201d1d", "#282727", "#393836", "#0a0909", "#403e3b", "#242222",
        "#c5c9c5", "#a6a69c", "#737c73", "#b6927b", "#7a6252", "#87a987", "#c4b28a", "#c4746e", "#7a4541", "#8ba4b0", "#a292a3"}},
    {"everforest", "Everforest", {"#232a2e", "#2d353b", "#343f44", "#3d484d", "#475258", "#1e2326", "#4f585e", "#384147",
        "#d3c6aa", "#9da9a0", "#7a8478", "#a7c080", "#6d7e52", "#83c092", "#dbbc7f", "#e67e80", "#8a4b4c", "#7fbbb3", "#d699b6"}},
    {"synthwave", "Synthwave '84", {"#1b1628", "#241b2f", "#262335", "#2f2a42", "#3b3452", "#17121f", "#463d5e", "#2c2640",
        "#ffffff", "#d6cfe4", "#848bbd", "#ff7edb", "#a34f8c", "#72f1b8", "#fede5d", "#fe4450", "#8f2830", "#36f9f6", "#b893ce"}},
    // Originals: Legion black with its red, and a cold blue-green night.
    {"crimson", "Crimson", {"#111112", "#18181a", "#1f1f22", "#28282c", "#333338", "#0c0c0d", "#2e2e33", "#222226",
        "#eeeeee", "#b8b8bc", "#87878e", "#e5484d", "#9c3236", "#7fc98f", "#f0c05a", "#ff7a59", "#8f4632", "#7aa7d9", "#b39ddb"}},
    {"glacier", "Glacier", {"#0f1519", "#141c21", "#1a242a", "#213038", "#2b3d46", "#0b1013", "#26363e", "#1c282e",
        "#e2eef2", "#a9c0c8", "#6f8a94", "#5ed3c4", "#3a8a80", "#8ed49a", "#e8c86a", "#ef7b7b", "#8a4646", "#7ab8e8", "#b4a2e6"}},
    // Non-blue surfaces: each tints its whole surface ladder with its own hue.
    {"ember", "Ember", {"#161110", "#1d1715", "#251d1b", "#2f2522", "#3b2e2a", "#100d0c", "#352a26", "#29211e",
        "#eee5e2", "#bfb4b0", "#928581", "#f2784b", "#a34a2a", "#9cc77a", "#f0b85a", "#ef5a5a", "#8c3434", "#7fb3c9", "#c79bd6"}},
    {"mocha", "Mocha", {"#161310", "#1c1915", "#24201b", "#2e2823", "#3a322b", "#100e0c", "#342d27", "#29231f",
        "#eee8e2", "#bfb7b0", "#928981", "#d9a066", "#94683d", "#a7c07e", "#e8c15f", "#e07a6a", "#88463c", "#8fb1c2", "#c39ac4"}},
    {"bordeaux", "Bordeaux", {"#171011", "#1d1416", "#261a1c", "#302124", "#3c292d", "#110c0d", "#362528", "#2a1d20",
        "#eee2e5", "#bfb0b3", "#928185", "#e86a8c", "#9c3f58", "#93c98a", "#ecc068", "#ff6b5b", "#8f3a33", "#82aed6", "#c49be0"}},
    {"plum", "Plum", {"#161016", "#1c151c", "#241b24", "#2e232e", "#3a2b3a", "#100c10", "#342734", "#291f29",
        "#eee2ee", "#bfb0bf", "#928192", "#d68be8", "#8e5a9c", "#8fd1a0", "#ebc46a", "#f07a8a", "#8c4250", "#86b4e0", "#f29ac8"}},
    {"moss", "Moss", {"#111611", "#171c16", "#1d241c", "#252e23", "#2e392c", "#0d100c", "#293327", "#21281f",
        "#e4eee2", "#b3bfb0", "#849281", "#9ccc65", "#62803d", "#6fd3a3", "#e3c55e", "#e8776c", "#8a4740", "#82b8c8", "#c0a0d8"}},
    {"jade", "Jade", {"#101715", "#141d1b", "#1a2622", "#21302b", "#293c36", "#0c110f", "#253631", "#1d2a26",
        "#e2eeea", "#b0bfba", "#81928d", "#3fd1a0", "#24876a", "#a6d47a", "#e8c766", "#ef7b7b", "#8a4646", "#78bfd6", "#c0a2e2"}},
    {"olive", "Olive", {"#151511", "#1c1c16", "#24231c", "#2d2d23", "#39382c", "#10100c", "#333228", "#28281f",
        "#eeede2", "#bfbeb0", "#929181", "#d8c25a", "#8d7e38", "#9fcf7c", "#f0a85a", "#e8776c", "#8a4740", "#8fb5c4", "#c2a0cf"}},
    {"sakura", "Sakura", {"#151113", "#1b1619", "#231d20", "#2c2428", "#382d32", "#0f0d0e", "#32292d", "#272024",
        "#eee2e8", "#bfb0b8", "#92818a", "#f4a6c0", "#a36a7e", "#9bd19a", "#efc77a", "#f27373", "#8f4444", "#8cb8e0", "#c6a6f0"}},
    {"phosphor", "Phosphor", {"#0f1711", "#141e16", "#1a261d", "#203024", "#283d2d", "#0b110d", "#243629", "#1d2b20",
        "#e2eee5", "#b0bfb4", "#819286", "#4af08a", "#2a9455", "#b5f07a", "#e8e05a", "#ff6a5a", "#94382f", "#5ed8d0", "#b6a0f0"}},
};
QString g_current = QStringLiteral("graphite");
bool g_restart = false;
QSettings settings() {
    return QSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("centurion"), QStringLiteral("gui"));
}
} // namespace

QList<QPair<QString, QString>> themes() {
    QList<QPair<QString, QString>> out;
    for (const Palette &p : PALETTES) out.append({QString::fromLatin1(p.id), QString::fromLatin1(p.name)});
    return out;
}
QString currentTheme() { return g_current; }

void load() {
    const QString want = settings().value(QStringLiteral("theme")).toString();
    const Palette *p = &PALETTES[0];
    for (const Palette &x : PALETTES) if (want == QLatin1String(x.id)) p = &x;
    g_current = QString::fromLatin1(p->id);
    const char **tok[] = {&BG0, &BG1, &BG2, &BG3, &BG4, &WELL, &BORDER, &BORDER_SOFT, &FG, &FG_DIM, &MUTED,
                          &ACCENT, &ACCENT_SOFT, &OK, &WARN, &DANGER, &DANGER_SOFT, &INFO, &PURPLE};
    for (int i = 0; i < 19; ++i) *tok[i] = p->c[i];
}

bool saveTheme(const QString &id) {
    QSettings s = settings();
    s.setValue(QStringLiteral("theme"), id);
    s.sync();
    return s.status() == QSettings::NoError;
}

void requestRestart() { g_restart = true; QCoreApplication::quit(); }
bool restartRequested() { return g_restart; }

QString profileAccent(const QString &p) {
    static const QHash<QString, QString> m{
        {"low-power", INFO}, {"quiet", INFO}, {"cool", "#7cc6bd"}, {"balanced", FG},
        {"balanced-performance", WARN}, {"performance", DANGER}, {"max-power", "#e070b0"}, {"custom", PURPLE}};
    return m.value(p, MUTED);
}

QString rgba(const char *hex, double alpha) {
    const QColor c(QString::fromLatin1(hex));
    return QStringLiteral("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(alpha, 0, 'f', 2);
}

QString banner(const char *color, const QString &selector) {
    const QString body = QStringLiteral("background: %1; border: 1px solid %2; border-radius: %3px; color: %4;")
                             .arg(rgba(color, 0.08), rgba(color, 0.34)).arg(RADIUS - 1).arg(FG);
    return selector.isEmpty() ? body : selector + QStringLiteral(" { ") + body + QStringLiteral(" }");
}

static QPalette palette() {
    QPalette p;
    auto c = [](const char *s) { return QColor(QString::fromLatin1(s)); };
    p.setColor(QPalette::Window, c(BG0));        p.setColor(QPalette::WindowText, c(FG));
    p.setColor(QPalette::Base, c(WELL));         p.setColor(QPalette::AlternateBase, c(BG1));
    p.setColor(QPalette::ToolTipBase, c(BG3));   p.setColor(QPalette::ToolTipText, c(FG));
    p.setColor(QPalette::Text, c(FG));           p.setColor(QPalette::Button, c(BG3));
    p.setColor(QPalette::ButtonText, c(FG));     p.setColor(QPalette::BrightText, c(DANGER));
    p.setColor(QPalette::Link, c(INFO));         p.setColor(QPalette::Highlight, c(ACCENT));
    p.setColor(QPalette::HighlightedText, c(BG0)); p.setColor(QPalette::PlaceholderText, c(MUTED));
    p.setColor(QPalette::Mid, c(BORDER));        p.setColor(QPalette::Light, c(BG4));
    p.setColor(QPalette::Dark, c(BG0));          p.setColor(QPalette::Shadow, c(WELL));
    for (auto r : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, r, c(MUTED));
    p.setColor(QPalette::Disabled, QPalette::Base, c(BG1));
    p.setColor(QPalette::Disabled, QPalette::Button, c(BG1));
    return p;
}

/// `a` moved towards `b` by `t` (0..1), as #rrggbb: derived surfaces that stay
/// inside each palette's own hue instead of a fixed grey.
static QString mix(const char *a, const char *b, double t) {
    const QColor x(QString::fromLatin1(a)), y(QString::fromLatin1(b));
    auto ch = [t](int p, int q) { return int(p + (q - p) * t + 0.5); };
    return QColor(ch(x.red(), y.red()), ch(x.green(), y.green()), ch(x.blue(), y.blue())).name();
}

int brandInset() {
    // Same font the style sheet gives QLabel#appWordmark (family from the desktop).
    QFont f = QApplication::font();
    f.setPointSize(FONT_PT + 2);
    f.setWeight(QFont::Bold);
    return BRAND_X + BRAND_ICON + BRAND_GAP + QFontMetrics(f).horizontalAdvance(QStringLiteral("Centurion")) + 22;
}

// Selectors (objectName / dynamic properties) are the contract with the tabs:
// btnAccent, btnDanger, btnMini, terminal, miniSlider, box_<colour>,
// role=muted/title/value, mainTabs. Only the look behind them changes here.
//
// Grammar: the window is an instrument panel. A darker bezel carries the brand
// and a recessed selector rail whose current key is lit; cards are machined
// plates (hairline edge, lighter top edge); controls sit one step above the
// card, text you type into sits in a well below it. Radii follow the hierarchy:
// panel 10, rail key / control 7, tick 4.
static QString stylesheet() {
    QString s = QStringLiteral(R"QSS(
QWidget { background: @BG0; color: @FG; font-size: @FONTpt; }
/* Plain layout containers (exact class QWidget, not subclasses) take their
   parent's surface instead of punching a window-coloured hole into a card. */
.QWidget { background: transparent; }
QToolTip { background: @BG2; color: @FG; border: 1px solid @BORDER; border-left: 3px solid @ACCENT; border-radius: 7px; padding: 6px 10px; }

/* ── header bezel: brand mark, then the page selector rail ── */
QTabWidget#mainTabs { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 @BEZEL_TOP, stop:1 @BEZEL_BOT); }
QTabWidget#mainTabs::pane { border: none; border-top: 1px solid @BORDER;
  background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 @BG0, stop:0.55 @BG0, stop:1 @PAGE_BOT); top: 0; }
QTabWidget#mainTabs::tab-bar { left: @BRANDWpx; }
/* The rail: a recessed track (dark, shadowed top edge, lit bottom edge). */
QTabWidget#mainTabs > QTabBar { background: @RAIL; border: 1px solid @BORDER_SOFT; border-top-color: @RAIL_SHADE;
  border-bottom-color: @EDGE; border-radius: 10px; margin: 8px 0; }
/* Keys in the rail; the sub-tabs inside a page keep an underline, so the two levels never read alike.
   One weight for every state: the rail is sized to its keys (MainWindow::fitTabs) and must not change width with the selection. */
QTabWidget#mainTabs > QTabBar::tab { background: transparent; color: @MUTED; border: 1px solid transparent;
  border-radius: 7px; padding: 4px 15px; margin: 11px 1px 11px 0; font-weight: 600; }
QTabWidget#mainTabs > QTabBar::tab:first { margin-left: 3px; }
QTabWidget#mainTabs > QTabBar::tab:last { margin-right: 3px; }
QTabWidget#mainTabs > QTabBar::tab:hover:!selected { background: @HILITE; color: @FG; }
/* The current page: a raised key with a lit amber edge. */
QTabWidget#mainTabs > QTabBar::tab:selected { color: @ACCENT_HI;
  background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 @KEY_TOP, stop:1 @BG3);
  border-color: @BORDER; border-top-color: @EDGE; border-bottom: 2px solid @ACCENT; }
QTabWidget#mainTabs > QTabBar::scroller { width: 48px; }
QTabWidget#mainTabs > QTabBar QToolButton { background: @BG2; border: 1px solid @BORDER; border-radius: 7px; margin: 10px 2px; }
QTabWidget#mainTabs > QTabBar QToolButton:hover { background: @BG3; border-color: @ACCENT_LINE; }
QWidget#appMark { background: transparent; }
QLabel#appWordmark { background: transparent; color: @FG; font-size: @BRANDPTpt; font-weight: 700; }

/* ── sub-tabs (inside a page): quieter, an underline ── */
QTabWidget::pane { border: none; border-top: 1px solid @BORDER_SOFT; background: transparent; top: -1px; }
QTabBar { qproperty-drawBase: 0; background: transparent; }
QTabBar::tab { background: transparent; color: @MUTED; padding: 5px 11px 4px 11px; margin-right: 2px;
  border: none; border-bottom: 2px solid transparent; }
QTabBar::tab:hover:!selected { color: @FG; border-bottom-color: @BORDER; }
QTabBar::tab:selected { color: @FG; font-weight: 600; border-bottom-color: @ACCENT; }
QTabBar::tab:disabled { color: @BG4; }
QTabBar::scroller { width: 24px; }
QTabBar QToolButton { background: @BG1; border: none; border-radius: 5px; margin: 2px 0; }
QTabBar QToolButton:hover { background: @BG3; }

/* ── sections: a machined plate, its title set inside, top left ── */
QGroupBox { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 @CARD_TOP, stop:0.16 @BG1, stop:1 @BG1);
  border: 1px solid @BORDER; border-top-color: @EDGE; border-bottom-color: @BORDER_SOFT;
  border-radius: @RADpx; margin-top: 0; padding: 27px 6px 5px 6px; font-weight: 600; color: @FG; }
QGroupBox::title { subcontrol-origin: border; subcontrol-position: top left; left: 12px; top: 8px;
  padding: 0 0 0 8px; color: @ACCENT; background: transparent; font-size: @FONTpt; font-weight: 700;
  border-left: 3px solid @ACCENT; }
QGroupBox#box_blue::title { color: @INFO; border-left-color: @INFO; } QGroupBox#box_purple::title { color: @PURPLE; border-left-color: @PURPLE; }
QGroupBox#box_yellow::title { color: @WARN; border-left-color: @WARN; } QGroupBox#box_green::title { color: @OK; border-left-color: @OK; }
QGroupBox#box_grey::title { color: @FG_DIM; border-left-color: @MUTED; }
QGroupBox#box_red::title { color: @DANGER; border-left-color: @DANGER; }
/* A section inside a section is not another card: a divider and a heading. */
QGroupBox QGroupBox { background: transparent; border: none; border-top: 1px solid @BORDER_SOFT;
  border-radius: 0; padding: 24px 0 0 0; }
QGroupBox QGroupBox::title { left: 2px; top: 7px; color: @ACCENT_SOFT; border-left: none; padding: 0; font-size: 8pt; }

QLabel { background: transparent; }
QLabel[role="muted"] { color: @MUTED; font-size: 8pt; }
QLabel[role="title"] { color: @FG; font-size: 15pt; font-weight: 700; }
QLabel[role="value"] { color: @FG; font-weight: 600; }
QFrame[frameShape="4"], QFrame[frameShape="5"] { color: @BORDER_SOFT; }

/* ── buttons: raised one step above the card (lit top edge); amber = primary ── */
QPushButton { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 @KEY_TOP, stop:1 @BG3); color: @FG;
  border: 1px solid @BORDER; border-top-color: @EDGE; border-bottom-color: @BTN_SHADE; border-radius: 7px;
  padding: 3px 10px; font-weight: 500; min-height: 16px; }
QPushButton:hover { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 @BG4, stop:1 @KEY_TOP); border-color: @ACCENT_LINE; border-top-color: @ACCENT_SOFT; }
QPushButton:pressed { background: @BG2; border-color: @BORDER; border-top-color: @BTN_SHADE; }
QPushButton:focus { border-color: @ACCENT_SOFT; border-bottom-color: @ACCENT; }
QPushButton:disabled { background: transparent; color: @MUTED; border-color: @BORDER_SOFT; }
QPushButton:checked { background: @ACCENT_TINT; color: @ACCENT_HI; border-color: @ACCENT_SOFT; }
QPushButton#btnAccent { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 @ACCENT_HI, stop:1 @ACCENT); border-color: @ACCENT_SOFT;
  border-top-color: @ACCENT_TOP; border-bottom-color: @ACCENT_DEEP; color: @ON_ACCENT; font-weight: 700; }
QPushButton#btnAccent:hover { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 @ACCENT_TOP, stop:1 @ACCENT_HI); border-color: @ACCENT_HI; }
QPushButton#btnAccent:pressed { background: @ACCENT_SOFT; border-color: @ACCENT_SOFT; }
QPushButton#btnAccent:focus { border-color: @FG; }
QPushButton#btnDanger { background: @DANGER_WASH; border-color: @DANGER_SOFT; color: @DANGER; }
QPushButton#btnDanger:hover { background: @DANGER_TINT; border-color: @DANGER; }
QPushButton#btnDanger:pressed { background: @DANGER_SOFT; color: @FG; }
QPushButton#btnAccent:disabled, QPushButton#btnDanger:disabled { background: transparent; color: @MUTED; border-color: @BORDER_SOFT; }
QPushButton#btnMini { padding: 0 7px; min-height: 14px; font-size: 8pt; border-radius: 6px; }
QToolButton { background: transparent; border: 1px solid transparent; border-radius: 7px; padding: 2px; }
QToolButton:hover { background: @BG3; border-color: @BORDER; }

/* ── inputs: recessed wells (shadowed top edge); the frame lights up only on focus ── */
QLineEdit, QSpinBox, QDoubleSpinBox, Int64SpinBox, QComboBox { background: @WELL; color: @FG; border: 1px solid @BORDER_SOFT;
  border-top-color: @RAIL_SHADE; border-bottom-color: @BORDER;
  border-radius: 7px; padding: 3px 7px; min-height: 16px; selection-background-color: @ACCENT_SOFT; selection-color: @FG; }
QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover, Int64SpinBox:hover, QComboBox:hover { border-color: @BORDER; border-bottom-color: @EDGE; }
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, Int64SpinBox:focus, QComboBox:focus, QComboBox:on { border-color: @ACCENT_SOFT; border-bottom-color: @ACCENT; }
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, Int64SpinBox:disabled, QComboBox:disabled { background: transparent; color: @MUTED; border-color: @BORDER_SOFT; }
QLineEdit:read-only { background: @BG1; }
QPlainTextEdit, QTextEdit { background: @WELL; color: @FG; border: 1px solid @BORDER_SOFT; border-top-color: @RAIL_SHADE; border-radius: 10px;
  padding: 6px 8px; selection-background-color: @ACCENT_SOFT; }
QComboBox { padding-right: 22px; }
QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: center right; border: none; width: 20px; }
QComboBox::down-arrow { image: url(:/theme/chev-down.png); width: 10px; height: 10px; }
QComboBox::down-arrow:disabled { image: url(:/theme/chev-down-off.png); }
QComboBox QAbstractItemView { background: @BG2; color: @FG; border: 1px solid @BORDER; border-radius: 10px; padding: 5px;
  selection-background-color: @ACCENT_TINT; selection-color: @FG; outline: none; }
QComboBox QAbstractItemView::item { min-height: 24px; padding: 0 8px; border-radius: 6px; }
QSpinBox, QDoubleSpinBox, Int64SpinBox { padding-right: 18px; }
QSpinBox::up-button, QDoubleSpinBox::up-button, Int64SpinBox::up-button { subcontrol-origin: border; subcontrol-position: top right;
  width: 17px; border: none; border-top-right-radius: 7px; background: transparent; }
QSpinBox::down-button, QDoubleSpinBox::down-button, Int64SpinBox::down-button { subcontrol-origin: border; subcontrol-position: bottom right;
  width: 17px; border: none; border-bottom-right-radius: 7px; background: transparent; }
QSpinBox::up-button:hover, QSpinBox::down-button:hover,
QDoubleSpinBox::up-button:hover, Int64SpinBox::up-button:hover, QDoubleSpinBox::down-button:hover, Int64SpinBox::down-button:hover { background: @BG3; }
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow, Int64SpinBox::up-arrow { image: url(:/theme/chev-up.png); width: 8px; height: 8px; }
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow, Int64SpinBox::down-arrow { image: url(:/theme/chev-down.png); width: 8px; height: 8px; }
QSpinBox::up-arrow:disabled, QSpinBox::up-arrow:off,
QDoubleSpinBox::up-arrow:disabled, Int64SpinBox::up-arrow:disabled, QDoubleSpinBox::up-arrow:off, Int64SpinBox::up-arrow:off { image: url(:/theme/chev-up-off.png); }
QSpinBox::down-arrow:disabled, QSpinBox::down-arrow:off,
QDoubleSpinBox::down-arrow:disabled, Int64SpinBox::down-arrow:disabled, QDoubleSpinBox::down-arrow:off, Int64SpinBox::down-arrow:off { image: url(:/theme/chev-down-off.png); }
QPlainTextEdit#terminal { background: @WELL; color: @FG_DIM; border: 1px solid @BORDER_SOFT; border-top-color: @RAIL_SHADE; border-radius: 10px; font-family: monospace; }

/* ── lists and tables ── */
QListView, QTreeView, QTableView { background: @WELL; color: @FG; border: 1px solid @BORDER_SOFT; border-top-color: @RAIL_SHADE; border-radius: 10px;
  padding: 4px; outline: none; alternate-background-color: @ROW_ALT; gridline-color: @BORDER_SOFT; }
QListView::item { padding: 3px 7px; border-radius: 6px; }
QListView::item:hover:!selected { background: @BG2; }
QListView::item:selected { background: @ACCENT_TINT; color: @FG; border-left: 2px solid @ACCENT; }
QTreeView::item, QTableView::item { padding: 2px 3px; }
QTreeView::item:hover:!selected, QTableView::item:hover:!selected { background: @HILITE; }
QTreeView::item:selected, QTableView::item:selected { background: @ACCENT_TINT; color: @FG; }
QHeaderView { background: transparent; border: none; }
QHeaderView::section { background: @BG1; color: @FG_DIM; border: none; border-bottom: 1px solid @BORDER;
  border-right: 1px solid @BORDER_SOFT; padding: 5px 8px; font-size: 8pt; font-weight: 700; }
QHeaderView::section:first { border-top-left-radius: 7px; }
QHeaderView::section:last { border-right: none; border-top-right-radius: 7px; }
QHeaderView::section:only-one { border-top-left-radius: 7px; border-top-right-radius: 7px; }
QHeaderView::section:hover { color: @FG; background: @BG2; }
QTableCornerButton::section { background: @BG1; border: none; }

/* ── toggles ── */
QCheckBox, QRadioButton { background: transparent; spacing: 8px; }
QCheckBox::indicator, QRadioButton::indicator { width: 15px; height: 15px; border: 1px solid @BORDER; border-top-color: @RAIL_SHADE; background: @WELL; }
QCheckBox::indicator { border-radius: 4px; } QRadioButton::indicator { border-radius: 8px; }
QCheckBox::indicator:hover, QRadioButton::indicator:hover { border-color: @ACCENT; background: @ACCENT_TINT; }
QCheckBox::indicator:checked { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 @ACCENT_HI, stop:1 @ACCENT); border-color: @ACCENT_SOFT; border-top-color: @ACCENT_TOP; image: url(:/theme/check.png); }
QCheckBox::indicator:indeterminate { background: @ACCENT_SOFT; border-color: @ACCENT_SOFT; }
QRadioButton::indicator:checked { border-color: @ACCENT;
  background: qradialgradient(cx:0.5, cy:0.5, radius:0.5, fx:0.5, fy:0.5, stop:0 @ACCENT_HI, stop:0.40 @ACCENT, stop:0.50 @WELL, stop:1 @WELL); }
QCheckBox::indicator:disabled, QRadioButton::indicator:disabled { background: transparent; border-color: @BORDER_SOFT; }
QCheckBox::indicator:checked:disabled { background: @BG4; border-color: @BG4; }
QRadioButton::indicator:checked:disabled { border-color: @BG4;
  background: qradialgradient(cx:0.5, cy:0.5, radius:0.5, fx:0.5, fy:0.5, stop:0 @BG4, stop:0.42 @BG4, stop:0.52 @BG1, stop:1 @BG1); }
QCheckBox:disabled, QRadioButton:disabled { color: @MUTED; }

/* ── sliders: a recessed rail, amber fill, a knob with a halo ── */
QSlider { background: transparent; }
QSlider::groove:horizontal { height: 5px; background: @WELL; border: 1px solid @BORDER_SOFT; border-top-color: @RAIL_SHADE; border-radius: 3px; margin: 0; }
QSlider::sub-page:horizontal { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 @ACCENT_SOFT, stop:1 @ACCENT_HI); border-radius: 3px; }
QSlider::handle:horizontal { background: @ACCENT_HI; border: 3px solid @ACCENT_HALO; width: 8px; height: 8px; margin: -6px 0; border-radius: 7px; }
QSlider::handle:horizontal:hover { background: @FG; border-color: @ACCENT_LINE; }
QSlider::handle:horizontal:disabled { background: @MUTED; border-color: transparent; }
QSlider::sub-page:horizontal:disabled { background: @BG4; }
QSlider#miniSlider::groove:horizontal { height: 3px; border-radius: 2px; }
QSlider#miniSlider::sub-page:horizontal { border-radius: 1px; }
QSlider#miniSlider::handle:horizontal { width: 4px; height: 4px; margin: -5px 0; border-radius: 5px; }

/* ── scrolling ── */
QScrollArea { border: none; background: transparent; }
QScrollArea > QWidget > QWidget { background: transparent; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 3px 1px; }
QScrollBar::handle:vertical { background: @BG4; border-radius: 4px; min-height: 32px; margin: 0 1px; }
QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover { background: @ACCENT_SOFT; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 1px 3px; }
QScrollBar::handle:horizontal { background: @BG4; border-radius: 4px; min-width: 32px; margin: 1px 0; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QProgressBar { background: @WELL; border: 1px solid @BORDER_SOFT; border-top-color: @RAIL_SHADE; border-radius: 6px; text-align: center; color: @FG; font-size: 8pt; font-weight: 600; min-height: 14px; }
QProgressBar::chunk { border-radius: 5px; background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 @ACCENT_SOFT, stop:1 @ACCENT_HI); }
QStatusBar { background: @BEZEL_BOT; color: @MUTED; font-size: 8pt; border-top: 1px solid @BORDER; } QStatusBar::item { border: none; }
QDialog, QMessageBox { background: @BG0; }

/* ── menus (tray) ── */
QMenu { background: @BG2; color: @FG; border: 1px solid @BORDER; border-top-color: @EDGE; border-radius: 10px; padding: 6px; }
QMenu::item { padding: 6px 26px 6px 12px; border-radius: 6px; background: transparent; }
QMenu::item:selected { background: @ACCENT_TINT; color: @ACCENT_HI; }
QMenu::right-arrow { image: url(:/theme/chev-down.png); width: 8px; height: 8px; }
QMenu::item:disabled { color: @MUTED; }
QMenu::separator { height: 1px; background: @BORDER_SOFT; margin: 5px 8px; }
QMenu::indicator { width: 12px; height: 12px; left: 6px; }
QMenu::indicator:checked { image: url(:/theme/check.png); background: @ACCENT; border-radius: 3px; }
QMenuBar { background: @BG0; } QMenuBar::item:selected { background: @BG3; }
)QSS");
    const QColor accent(QString::fromLatin1(ACCENT));
    // Text on the amber key: the darkest surface, unless the accent itself is dark.
    const QString onAccent = accent.lightnessF() < 0.45 ? QString::fromLatin1(FG) : QString::fromLatin1(BG0);
    // Longest names first so @BG doesn't eat @BG0 etc.
    const std::pair<const char *, QString> toks[] = {
        {"@BORDER_SOFT", BORDER_SOFT}, {"@ACCENT_SOFT", ACCENT_SOFT}, {"@DANGER_SOFT", DANGER_SOFT},
        {"@DANGER_TINT", rgba(DANGER, 0.14)}, {"@DANGER_WASH", rgba(DANGER, 0.05)}, {"@ACCENT_TINT2", rgba(ACCENT, 0.10)},
        {"@ACCENT_LINE", rgba(ACCENT, 0.32)}, {"@ACCENT_TINT", rgba(ACCENT, 0.16)}, {"@ACCENT_DEEP", accent.darker(135).name()},
        {"@HILITE2", "rgba(255, 255, 255, 0.09)"}, {"@HILITE", "rgba(255, 255, 255, 0.05)"}, {"@ACCENT_HALO", rgba(ACCENT, 0.28)},
        {"@ACCENT_TOP", accent.lighter(125).name()}, {"@ACCENT_HI", accent.lighter(112).name()}, {"@ON_ACCENT", onAccent},
        // Derived surfaces (each palette's own hue): bezel and page floor below the window colour,
        // the rail and well shadows below that; edges are the border lifted towards the text colour.
        {"@BEZEL_TOP", mix(BG1, BG0, 0.35)}, {"@BEZEL_BOT", mix(BG0, WELL, 0.55)}, {"@PAGE_BOT", mix(BG0, WELL, 0.65)},
        {"@RAIL_SHADE", mix(WELL, "#000000", 0.35)}, {"@RAIL", mix(WELL, BG0, 0.25)},
        {"@BTN_SHADE", mix(BORDER, "#000000", 0.35)}, {"@CARD_TOP", mix(BG1, BG2, 0.55)}, {"@ROW_ALT", mix(WELL, BG1, 0.6)},
        {"@KEY_TOP", mix(BG3, BG4, 0.45)}, {"@EDGE", mix(BORDER, FG, 0.13)},
        {"@BRANDW", QString::number(brandInset())}, {"@BRANDPT", QString::number(FONT_PT + 2)},
        {"@FG_DIM", FG_DIM}, {"@BORDER", BORDER}, {"@ACCENT", ACCENT}, {"@DANGER", DANGER},
        {"@MUTED", MUTED}, {"@PURPLE", PURPLE}, {"@INFO", INFO}, {"@WARN", WARN}, {"@WELL", WELL}, {"@OK", OK},
        {"@BG0", BG0}, {"@BG1", BG1}, {"@BG2", BG2}, {"@BG3", BG3}, {"@BG4", BG4}, {"@FG", FG},
        {"@FONT", QString::number(FONT_PT)}, {"@RAD", QString::number(RADIUS)}};
    for (const auto &[k, v] : toks) s.replace(QString::fromLatin1(k), v);
    return s;
}

namespace {
/// Fusion with one fix: a layout nested in a grid whose horizontal and vertical
/// spacing differ has no spacing of its own to inherit, and Fusion's fallback
/// is 0 — button pairs such as "Copy… / Delete" or "Fan curve… / Max all fans"
/// were drawn touching. Everything else is Fusion's.
class Style : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;
    int pixelMetric(PixelMetric m, const QStyleOption *o = nullptr, const QWidget *w = nullptr) const override {
        if (m == PM_LayoutHorizontalSpacing || m == PM_LayoutVerticalSpacing) return 4;
        return QProxyStyle::pixelMetric(m, o, w);
    }
    // Used per widget pair when the layout has no spacing at all (the nested case).
    int layoutSpacing(QSizePolicy::ControlType, QSizePolicy::ControlType, Qt::Orientation,
                      const QStyleOption * = nullptr, const QWidget * = nullptr) const override {
        return 4;
    }
};
} // namespace

void applyBase(QApplication &app) {
    // Fusion honours all of the QSS; native styles each ignore a different subset.
    app.setStyle(new Style(QStyleFactory::create(QStringLiteral("Fusion"))));
    app.setPalette(palette());
}

void applySheet(QApplication &app) { app.setStyleSheet(stylesheet()); }

void apply(QApplication &app) {
    applyBase(app);
    applySheet(app);
}

} // namespace theme
