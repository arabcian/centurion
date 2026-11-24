#include "theme.h"
#include <QApplication>
#include <QFontMetrics>
#include <QHash>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <cmath>
#include <utility>
#include <QPalette>
#include <QProxyStyle>
#include <QSettings>
#include <QRegularExpression>
#include <array>
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
    // Community colourways. Existing IDs stay unchanged for saved gui.ini preferences.
    // Catppuccin: four flavour variants, including the light Latte palette.
    {"catppuccin-mocha", "Catppuccin Mocha", {"#181825", "#1e1e2e", "#252538", "#313244", "#45475a", "#11111b", "#45465e", "#313248",
        "#cdd6f4", "#bac2de", "#9399b2", "#cba6f7", "#9370c4", "#a6e3a1", "#f9e2af", "#f38ba8", "#974b65", "#89b4fa", "#b4befe"}},
    {"catppuccin-macchiato", "Catppuccin Macchiato", {"#1e2030", "#24273a", "#2c3043", "#363a4f", "#494d64", "#181926", "#4b5067", "#34384d",
        "#cad3f5", "#b8c0e0", "#939ab7", "#b7bdf8", "#797fc1", "#a6da95", "#eed49f", "#ed8796", "#974b57", "#8aadf4", "#c6a0f6"}},
    {"catppuccin-frappe", "Catppuccin Frappé", {"#292c3c", "#303446", "#393e50", "#414559", "#51576d", "#232634", "#555b70", "#3d4256",
        "#c6d0f5", "#b5bfe2", "#949cbb", "#ca9ee6", "#896bb2", "#a6d189", "#e5c890", "#e78284", "#9a4c4e", "#8caaee", "#babbf1"}},
    {"catppuccin-latte", "Catppuccin Latte · Light", {"#eff1f5", "#e6e9ef", "#dce0e8", "#ccd0da", "#bcc0cc", "#ffffff", "#bcc0cc", "#d1d5df",
        "#4c4f69", "#5c5f77", "#6c6f85", "#8839ef", "#6926bd", "#287a26", "#986000", "#d20f39", "#9b2639", "#1e66f5", "#8031a7"}},
    {"nord", "Nord", {"#242933", "#2e3440", "#343c49", "#3b4252", "#434c5e", "#20252e", "#4c566a", "#343d4b",
        "#eceff4", "#d8dee9", "#a5b4c6", "#88c0d0", "#5c929f", "#a3be8c", "#ebcb8b", "#bf616a", "#7c4149", "#81a1c1", "#b48ead"}},
    {"dracula", "Dracula", {"#21222c", "#282a36", "#303241", "#383a4c", "#44475a", "#1c1d27", "#44475a", "#353748",
        "#f8f8f2", "#d9d9db", "#9296ad", "#bd93f9", "#825eb1", "#50fa7b", "#f1fa8c", "#ff5555", "#9c3434", "#8be9fd", "#ff79c6"}},
    {"one-dark", "One Dark", {"#21252b", "#282c34", "#30353e", "#353b45", "#424953", "#1b1e23", "#414854", "#303640",
        "#abb2bf", "#b7bdc7", "#858f9e", "#61afef", "#3679b5", "#98c379", "#e5c07b", "#e06c75", "#8e4249", "#56b6c2", "#c678dd"}},
    {"solarized-dark", "Solarized Dark", {"#00252d", "#002b36", "#073642", "#154651", "#205662", "#001d24", "#25535e", "#12404a",
        "#eee8d5", "#93a1a1", "#839496", "#268bd2", "#146195", "#859900", "#b58900", "#dc322f", "#8e2728", "#2aa198", "#6c71c4"}},
    {"solarized-light", "Solarized Light", {"#fdf6e3", "#eee8d5", "#e2dcc8", "#d5cfbd", "#c5c0b0", "#fffdf4", "#c4beac", "#ded7c4",
        "#354e55", "#526a70", "#677d83", "#17649b", "#135080", "#637500", "#8b6500", "#c12c29", "#8c2926", "#167c74", "#6c71c4"}},
    {"gruvbox-light", "Gruvbox Light", {"#fbf1c7", "#f2e5bc", "#ebdbb2", "#d5c4a1", "#bdae93", "#fff9e1", "#c5b38d", "#e2d2a9",
        "#3c3836", "#504945", "#665c54", "#a84a00", "#854000", "#647300", "#946200", "#ad3028", "#85271f", "#076678", "#8f3f71"}},
    {"rose-pine-dawn", "Rosé Pine Dawn · Light", {"#faf4ed", "#f2e9e1", "#ede2d9", "#dfdad9", "#cecacd", "#fffaf5", "#c7bdc0", "#e4dcdb",
        "#575279", "#686284", "#797593", "#b45b70", "#8b4054", "#287b83", "#906618", "#b13b60", "#842944", "#286983", "#907aa9"}},
    {"kanagawa-wave", "Kanagawa Wave", {"#1b1c25", "#1f1f28", "#252535", "#2a2a37", "#363646", "#16161d", "#3b3b4d", "#292938",
        "#dcd7ba", "#c8c093", "#9699a3", "#7e9cd8", "#4c70a6", "#98bb6c", "#e6c384", "#e46876", "#8d404a", "#7fb4ca", "#957fb8"}},
    {"ayu-mirage", "Ayu Mirage", {"#191e26", "#1f2430", "#272d39", "#303846", "#3c4554", "#141922", "#3a4352", "#2c3340",
        "#cbccc6", "#b7babe", "#8a919d", "#ffcc66", "#ad8736", "#bae67e", "#ffd580", "#f28779", "#9a534a", "#5ccfe6", "#d4bfff"}},
    {"github-dark", "GitHub Dark", {"#0d1117", "#161b22", "#1e2530", "#272e38", "#303946", "#090c10", "#30363d", "#252c35",
        "#e6edf3", "#b1bac4", "#8b949e", "#58a6ff", "#326aab", "#3fb950", "#d29922", "#f85149", "#a52e32", "#79c0ff", "#bc8cff"}},
    {"nightfox", "Nightfox", {"#171c28", "#192330", "#212e3d", "#29394a", "#374a5b", "#131923", "#3d4a5c", "#263544",
        "#cdcecf", "#aeafb0", "#888f98", "#719cd6", "#486fa8", "#81b29a", "#dbc074", "#c94f6d", "#8b334a", "#63cdcf", "#9d79d6"}},
    // Original pastel colourways: twelve light and eight dark, with non-blue surface hues.
    // Pastel surfaces are soft; active accents are deliberately deeper for readable text.
    {"pastel-mint-cream", "Mint Cream · Pastel Light", {"#e5f4ed", "#d7e9df", "#c8ded2", "#b7d2c2", "#a5c5b3", "#f6fcf8", "#abc9b8", "#c6dcd0",
        "#24483d", "#3f6054", "#4e6a5d", "#227058", "#195b49", "#347651", "#8a6419", "#b44750", "#87333b", "#317488", "#78518d"}},
    {"pastel-peach-sorbet", "Peach Sorbet · Pastel Light", {"#faefe4", "#f2e2d4", "#e8d3c2", "#dcc1ad", "#cba993", "#fff9f1", "#d2b39d", "#ead4c3",
        "#51392f", "#694b3f", "#705449", "#904330", "#853b2a", "#3d7350", "#886018", "#ae4050", "#86303c", "#367083", "#7f528a"}},
    {"pastel-lilac-mist", "Lilac Mist · Pastel Light", {"#f0ebf9", "#e6def4", "#d9cfec", "#cbbde3", "#bcaad6", "#fbf8ff", "#c6b7de", "#e0d5f0",
        "#3e3357", "#55456f", "#645675", "#6d4a9b", "#573983", "#326c58", "#875c1c", "#af4255", "#872f41", "#356a88", "#8253a5"}},
    {"pastel-sage-linen", "Sage Linen · Pastel Light", {"#eaf0e5", "#dfe8d8", "#d1dfc9", "#c2d3b6", "#afc3a2", "#f8fbf3", "#b8c9aa", "#d3ddc9",
        "#334d36", "#4e6750", "#566856", "#436d3d", "#355d35", "#2e795f", "#8a651a", "#b34b4b", "#873639", "#356e83", "#76558e"}},
    {"pastel-buttercream", "Buttercream · Pastel Light", {"#faf3d9", "#f3e9cb", "#eadbb8", "#dfcda5", "#d2bd8f", "#fffbee", "#d8c392", "#ecddba",
        "#51462a", "#6c5937", "#75633e", "#835f18", "#755317", "#3b7554", "#95631a", "#b54543", "#8c3435", "#366e85", "#79568e"}},
    {"pastel-rosewater", "Rosewater · Pastel Light", {"#f8eaf0", "#f1dce5", "#e8ccd9", "#dbb8c9", "#cda5ba", "#fff8fb", "#d5afc1", "#e9cfdc",
        "#523545", "#6f4b5b", "#705260", "#943a60", "#842d54", "#367454", "#8e651c", "#ac3f55", "#842e42", "#386d89", "#86528e"}},
    {"pastel-pistachio-milk", "Pistachio Milk · Pastel Light", {"#edf4dd", "#e3edce", "#d5e2bb", "#c4d3a6", "#b2c18f", "#f9fcf1", "#bdcba3", "#dce5c4",
        "#3e4c2a", "#56663c", "#5c6645", "#546a21", "#4c641f", "#2f7659", "#8b6316", "#b04c47", "#863532", "#356f83", "#77528b"}},
    {"pastel-coral-sand", "Coral Sand · Pastel Light", {"#faebe3", "#f3ded4", "#eccdc0", "#dfbaa9", "#d3a793", "#fff8f4", "#d9b19e", "#ebcfc2",
        "#51372f", "#6b4e44", "#6e554c", "#94403c", "#8d3434", "#397456", "#895e18", "#b33d46", "#842a35", "#356f86", "#81528b"}},
    {"pastel-sea-glass", "Sea Glass · Pastel Light", {"#e3f3ef", "#d4e9e5", "#c4ddd8", "#b1d1ca", "#9dc3bb", "#f5fbfa", "#a5c9c1", "#c1dcd5",
        "#264b49", "#406362", "#4b6765", "#1f6d68", "#175d59", "#337456", "#92611b", "#b2444c", "#87333b", "#26718b", "#78588d"}},
    {"pastel-orchid-milk", "Orchid Milk · Pastel Light", {"#f7ebf6", "#f0dcf0", "#e6cce6", "#dcbada", "#cea6cb", "#fff8fd", "#d4b1d2", "#ebd0e9",
        "#503550", "#6a496b", "#705270", "#834383", "#773c76", "#357458", "#8b641a", "#b44354", "#873242", "#356d86", "#86519a"}},
    {"pastel-sky-cotton", "Sky Cotton · Pastel Light", {"#e8f2fa", "#dbe9f5", "#ccdfef", "#bad0e5", "#a8bfd7", "#f7fbff", "#b0c8db", "#c9dced",
        "#30465a", "#49617a", "#516576", "#316792", "#295981", "#367652", "#8c6119", "#b0414c", "#87313e", "#2e7591", "#79518e"}},
    {"pastel-sandstone", "Sandstone · Pastel Light", {"#f3efe5", "#eae3d5", "#ddd2bf", "#d0c1aa", "#c2ae93", "#fdfaf3", "#cabba3", "#e5dbca",
        "#4b4235", "#635647", "#665948", "#725632", "#70502b", "#397452", "#8b6117", "#b44348", "#87333b", "#357188", "#79548d"}},
    {"pastel-dusty-orchid", "Dusty Orchid · Pastel Dark", {"#1e1925", "#28202e", "#332939", "#403246", "#514059", "#16121b", "#50405a", "#382e40",
        "#ede2f0", "#cdbad4", "#a995b2", "#c7a2df", "#896d9d", "#9accae", "#dfc48e", "#e99aa7", "#925560", "#93c4cd", "#b9a1e5"}},
    {"pastel-eucalyptus-night", "Eucalyptus Night · Pastel Dark", {"#17221e", "#1e2c25", "#27382f", "#304639", "#3d5747", "#111a16", "#3d5746", "#2b3b32",
        "#e0ece4", "#b6cdbd", "#92a99a", "#a1d8b3", "#689a79", "#a5dbaa", "#e5c98b", "#e99e9b", "#925a57", "#96c9d1", "#c7a9d3"}},
    {"pastel-cinnamon-cocoa", "Cinnamon Cocoa · Pastel Dark", {"#211a17", "#2b211b", "#362920", "#45342a", "#574236", "#191310", "#574235", "#3c2e25",
        "#f0e7df", "#d4c2b4", "#b39d8b", "#e5b18e", "#a37960", "#a7d3ad", "#e5c98c", "#e69c9b", "#945c58", "#96c6d0", "#cbb1dd"}},
    {"pastel-dusty-coral", "Dusty Coral · Pastel Dark", {"#261b1c", "#302224", "#3b2a2c", "#4b3436", "#5d4043", "#1b1314", "#5b4144", "#402e31",
        "#f2e3e3", "#d7bfbf", "#bba1a3", "#efa5a1", "#a87575", "#a5d3b3", "#e3c68b", "#efa2a4", "#95565b", "#98c5cf", "#c9a7d9"}},
    {"pastel-marigold-ink", "Marigold Ink · Pastel Dark", {"#222018", "#2d281d", "#383223", "#473d2a", "#594c34", "#1a1710", "#584c37", "#403627",
        "#f0ead9", "#d7caaf", "#b6a98d", "#e2c689", "#a08752", "#a6d0aa", "#eacb8c", "#e6a19e", "#965c59", "#94c5cc", "#c8addb"}},
    {"pastel-mulberry-velvet", "Mulberry Velvet · Pastel Dark", {"#241923", "#30202e", "#3b2938", "#4b3447", "#5d4157", "#1b121a", "#5a4055", "#412d3e",
        "#f0e4ee", "#d9c0d5", "#b8a0b4", "#e4accf", "#a57294", "#a5d4b0", "#e7c78f", "#e99da5", "#92555e", "#97c9d0", "#c4afe3"}},
    {"pastel-olive-smoke", "Olive Smoke · Pastel Dark", {"#1f2319", "#282d20", "#333929", "#414834", "#515b42", "#171a12", "#4c563e", "#37402e",
        "#e9eddf", "#c9d1bb", "#a4ae94", "#c7d68f", "#8d9d60", "#a0d4b0", "#e6cd8a", "#e9a09d", "#965954", "#96c9d0", "#c7b0dd"}},
    {"pastel-opal-lagoon", "Opal Lagoon · Pastel Dark", {"#172225", "#1e2c2f", "#27383b", "#30474a", "#3b585c", "#10191b", "#3d5457", "#2a3c3f",
        "#e0eced", "#b7cfd0", "#92abad", "#9dd8cc", "#619d92", "#a6d5ad", "#e8ca8b", "#e59fa2", "#945960", "#98c9e0", "#c5b0dd"}},
    {"oled-black", "OLED Black", {"#000000", "#101010", "#181818", "#222222", "#303030", "#000000", "#414141", "#282828",
        "#dedede", "#bcbcbc", "#a0a0a0", "#9bc8b4", "#5b8873", "#a4ce9a", "#e5c78b", "#e8a19e", "#8a4846", "#99c7da", "#c5acd9"}},
};
QString g_current = QStringLiteral("graphite");
std::array<QByteArray, 19> customStorage;
bool g_restart = false;
bool g_experimental = true;
bool g_compact = false;
std::function<bool()> g_restartGuard;
QSettings settings() {
    return QSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("centurion"), QStringLiteral("gui"));
}
} // namespace

QList<QPair<QString, QString>> themes() {
    QList<QPair<QString, QString>> out;
    for (const Palette &p : PALETTES) out.append({QString::fromLatin1(p.id), QString::fromLatin1(p.name)});
    if (validCustomPalette(settings().value(QStringLiteral("customTheme/colors")).toStringList()))
        out.append({QStringLiteral("custom"), QCoreApplication::translate("theme", "Custom palette")});
    return out;
}
QStringList paletteTokenNames() {
    return {"BG0", "BG1", "BG2", "BG3", "BG4", "WELL", "BORDER", "BORDER_SOFT", "FG", "FG_DIM", "MUTED", "ACCENT", "ACCENT_SOFT", "OK", "WARN", "DANGER", "DANGER_SOFT", "INFO", "PURPLE"};
}
QStringList paletteColors(const QString &id) {
    if (id == QLatin1String("custom")) {
        const auto colors = settings().value(QStringLiteral("customTheme/colors")).toStringList();
        if (validCustomPalette(colors)) return colors;
    }
    const Palette *selected = &PALETTES[0];
    for (const Palette &p : PALETTES) if (id == QLatin1String(p.id)) selected = &p;
    QStringList result;
    for (const char *c : selected->c) result << QString::fromLatin1(c);
    return result;
}
bool validCustomPalette(const QStringList &colors) {
    if (colors.size() != 19) return false;
    static const QRegularExpression rgb(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    for (const QString &c : colors) if (!rgb.match(c).hasMatch()) return false;
    return true;
}
bool saveCustomPalette(const QStringList &colors) {
    if (!validCustomPalette(colors)) return false;
    auto s = settings();
    s.setValue(QStringLiteral("customTheme/colors"), colors);
    s.sync(); return s.status() == QSettings::NoError;
}
QString currentTheme() { return g_current; }

QIcon themePreviewIcon(const QString &id) {
    const QStringList colors = paletteColors(id);
    // Three actual colours from the preset: surface, foreground, and accent.
    // Drawn at 2x so the chooser stays sharp on high-DPI / OLED displays.
    QPixmap preview(76, 36);
    preview.setDevicePixelRatio(2.0);
    preview.fill(Qt::transparent);
    QPainter painter(&preview);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QColor(colors[6]));
    painter.setBrush(QColor(colors[1]));
    painter.drawRoundedRect(QRectF(0.5, 0.5, 37, 17), 4, 4);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(colors[0]));
    painter.drawRoundedRect(QRectF(3, 3, 9, 12), 2, 2);
    painter.setBrush(QColor(colors[8]));
    painter.drawRoundedRect(QRectF(14, 3, 9, 12), 2, 2);
    painter.setBrush(QColor(colors[11]));
    painter.drawRoundedRect(QRectF(25, 3, 9, 12), 2, 2);
    return QIcon(preview);
}

bool experimentalEnabled() { return g_experimental; }

static bool readExperimentalChoice() {
    const QByteArray overrideMode = qgetenv("CENTURION_UI").trimmed().toLower();
    if (overrideMode == "classic") return false;
    if (overrideMode == "experimental") return true;
    return settings().value(QStringLiteral("ui/mode"), QStringLiteral("experimental")).toString()
           != QLatin1String("classic");
}

bool saveUiMode(bool experimental) {
    QSettings s = settings();
    s.setValue(QStringLiteral("ui/mode"), experimental ? QStringLiteral("experimental") : QStringLiteral("classic"));
    s.sync();
    return s.status() == QSettings::NoError;
}


bool compactDensity() { return g_compact; }
bool saveCompactDensity(bool compact) {
    auto s = settings(); s.setValue(QStringLiteral("ui/compact_density"), compact);
    s.sync(); return s.status() == QSettings::NoError;
}

void load() {
    g_compact = settings().value(QStringLiteral("ui/compact_density"), false).toBool();
    g_experimental = readExperimentalChoice();
    const QString want = settings().value(QStringLiteral("theme")).toString();
    const Palette *p = &PALETTES[0];
    for (const Palette &x : PALETTES) if (want == QLatin1String(x.id)) p = &x;
    g_current = QString::fromLatin1(p->id);
    const char **tok[] = {&BG0, &BG1, &BG2, &BG3, &BG4, &WELL, &BORDER, &BORDER_SOFT, &FG, &FG_DIM, &MUTED,
                          &ACCENT, &ACCENT_SOFT, &OK, &WARN, &DANGER, &DANGER_SOFT, &INFO, &PURPLE};
    const auto custom = settings().value(QStringLiteral("customTheme/colors")).toStringList();
    if (want == QLatin1String("custom") && validCustomPalette(custom)) {
        g_current = want;
        for (int i = 0; i < 19; ++i) { customStorage[i] = custom[i].toLatin1(); *tok[i] = customStorage[i].constData(); }
    } else for (int i = 0; i < 19; ++i) *tok[i] = p->c[i];
}

bool saveTheme(const QString &id) {
    // Never persist an unrecognised id: a typo should not silently select Graphite.
    bool found = id == QLatin1String("custom") && validCustomPalette(settings().value(QStringLiteral("customTheme/colors")).toStringList());
    for (const Palette &p : PALETTES) {
        if (id == QLatin1String(p.id)) { found = true; break; }
    }
    if (!found) return false;
    QSettings s = settings();
    s.setValue(QStringLiteral("theme"), id);
    s.sync();
    return s.status() == QSettings::NoError;
}

void setRestartGuard(std::function<bool()> guard) { g_restartGuard = std::move(guard); }
void requestRestart() {
    if (g_restart || (g_restartGuard && !g_restartGuard())) return;
    g_restart = true;
    QCoreApplication::quit();
}
bool restartRequested() { return g_restart; }

static double luminance(const QColor &c) {
    auto ch = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * ch(c.redF()) + 0.7152 * ch(c.greenF()) + 0.0722 * ch(c.blueF());
}

/// A fixed (not palette) profile colour, darkened in 10 % steps on a light palette until its dot reads
/// at 3:1 against the profile button (BG2), the minimum for a non-text mark. Dark palettes keep it as is.
static QString legible(const char *hex) {
    QColor c(QString::fromLatin1(hex));
    const double bg = luminance(QColor(QString::fromLatin1(BG2)));
    for (int i = 0; i < 20 && bg > luminance(c) && (bg + 0.05) / (luminance(c) + 0.05) < 3.0; ++i)
        c = QColor::fromRgbF(float(c.redF() * 0.9), float(c.greenF() * 0.9), float(c.blueF() * 0.9));
    return c.name();
}

QString profileAccent(const QString &p) {
    static const QHash<QString, QString> m{
        {"low-power", INFO}, {"quiet", INFO}, {"cool", legible("#7cc6bd")}, {"balanced", FG},
        {"balanced-performance", WARN}, {"performance", DANGER}, {"max-power", legible("#e070b0")}, {"custom", PURPLE}};
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

// Contrast-aware labels are necessary for light themes and high-chroma accent colours.
static QColor textOnAccent() {
    const QColor a(QString::fromLatin1(ACCENT));
    const auto linear = [](double x) {
        return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
    };
    const double luma = 0.2126 * linear(a.redF()) + 0.7152 * linear(a.greenF())
                      + 0.0722 * linear(a.blueF());
    return luma > 0.179 ? QColor(QStringLiteral("#15161a")) : QColor(QStringLiteral("#ffffff"));
}

static bool lightTheme() {
    return QColor(QString::fromLatin1(BG0)).lightnessF() > 0.62;
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
    p.setColor(QPalette::HighlightedText, textOnAccent()); p.setColor(QPalette::PlaceholderText, c(MUTED));
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
    return BRAND_X + BRAND_ICON + BRAND_GAP + QFontMetrics(f).horizontalAdvance(QStringLiteral("Centurion"))
           + (experimentalEnabled() ? 118 : 22);
}

// Selectors (objectName / dynamic properties) are the contract with the tabs:
// btnAccent, btnDanger, btnMini, terminal, miniSlider, box_<colour>,
// role=muted/title/value, tone=muted/dim/warn/danger, mainTabs. Only the look behind them changes here.
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
/* Text tones (setProperty("tone", …)) instead of per-widget color stylesheets: follow theme switches. */
*[tone="muted"] { color: @MUTED; }
*[tone="dim"] { color: @FG_DIM; }
*[tone="warn"] { color: @WARN; }
*[tone="danger"] { color: @DANGER; }
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
    // Experimental: flatter surfaces and stronger hierarchy without hiding controls.
    // Appended after the classic selectors, so the original styling is selectable.
    // Uses the same palette tokens: every existing colour theme keeps working.
    if (experimentalEnabled()) s += QStringLiteral(R"QSS(
/* Experimental / Studio: content-first, tight hierarchy, no faux-metal shading. */
QScrollArea { border: 0; }
QSplitter::handle { background: @BORDER_SOFT; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }
QWidget#expShell QGroupBox { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 @CARD_TOP, stop:1 @CARD_BOT);
    border: 1px solid @BORDER; border-top-color: @EDGE; }
QWidget#expShell QGroupBox QGroupBox { background: transparent; border: 0;
    border-top: 1px solid @BORDER_SOFT; }
QToolButton#expDisclosure { background: transparent; color: @FG_DIM; text-align: left;
    border: 0; border-top: 1px solid @BORDER_SOFT; border-radius: 0;
    font-weight: 700; font-size: 8pt; padding: 8px 6px; }
QToolButton#expDisclosure:hover { color: @FG; background: @BG2; }
QTabWidget#intelWorkflows::pane { border-top: 1px solid @BORDER_SOFT; }
QTabWidget#intelWorkflows QTabBar::tab { padding: 7px 14px; }

QWidget#expShell, QWidget#expContent { background: @BG0; }
/* The rail sits a step below the page: content is the lit surface, navigation recedes. */
QFrame#expRail { background: @SIDEBAR; border-right: 1px solid @BORDER_SOFT; }
QWidget#expNavHost, QScrollArea#expNavScroll,
QScrollArea#expNavScroll > QWidget > QWidget { background: transparent; }
QLabel#expBrandLabel { color: @FG; font-size: 10pt; font-weight: 800; letter-spacing: 1.6px; background: transparent; }
QLabel#expBrandVersion { color: @MUTED; font-size: 7pt; font-weight: 600; background: transparent; }
QLabel#expBrandIcon { background: transparent; }
QFrame#expRailDivider { background: @BORDER_SOFT; border: 0; }
QLabel#expNavHeading { color: @MUTED; background: transparent;
    padding: 12px 6px 4px 10px; font-size: 7pt; font-weight: 700; letter-spacing: 1.4px; }
QPushButton#expNavButton { text-align: left; color: @FG_DIM; background: transparent;
    border: 1px solid transparent; border-left: 3px solid transparent; border-radius: 7px;
    font-weight: 500; padding: 3px 10px; min-height: 0; }
QPushButton#expNavButton:hover { background: @HILITE; color: @FG; }
QPushButton#expNavButton:pressed { background: @HILITE2; }
/* Current page: a framed accent wash that fades into the rail, lit from its marker. */
QPushButton#expNavButton:checked { color: @FG;
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 @ACCENT_TINT, stop:1 @ACCENT_FADE);
    border: 1px solid @ACCENT_EDGE; border-left: 3px solid @ACCENT; font-weight: 700; }
QPushButton#expNavButton:disabled { color: @BG4; }
QPushButton#expNavButton[collapsed="true"] { text-align: center; padding: 3px 1px; font-size: 8pt; }
QPushButton#expRailToggle { text-align: left; color: @MUTED; font-size: 8pt;
    background: transparent; border: 0; border-top: 1px solid @BORDER_SOFT;
    border-radius: 0; padding: 7px 10px; min-height: 21px; }
QPushButton#expRailToggle:hover { color: @FG; background: @HILITE; }
QFrame#expTopbar { background: @BG0; border-bottom: 1px solid @BORDER_SOFT; }
QLabel#expTopPath { color: @MUTED; font-size: 7pt; font-weight: 700; letter-spacing: 1.4px; }
QLabel#expTopTitle { color: @FG; font-size: 14pt; font-weight: 700; }
/* Search field look-alike: opens the navigator (Ctrl+K). */
QPushButton#expSearch { background: @WELL; border: 1px solid @BORDER; border-radius: 9px;
    padding: 0; min-height: 30px; }
QPushButton#expSearch:hover { border-color: @ACCENT_LINE; background: @FOCUS_WELL; }
QPushButton#expSearch:pressed, QPushButton#expSearch:focus { border-color: @ACCENT_SOFT; }
QLabel#expSearchText { color: @MUTED; font-size: 8pt; }
/* Key legend: a small raised cap. */
QLabel#expKeycap { color: @FG_DIM; background: @BG3; border: 1px solid @BORDER; border-bottom: 2px solid @BORDER;
    border-radius: 4px; padding: 0 5px; font-size: 7pt; font-weight: 700; }
/* Status chips: the power profile and the active scene, on every page. */
QPushButton#expChip { background: @BG2; color: @FG_DIM; border: 1px solid @BORDER; border-radius: 12px;
    padding: 0 11px 0 9px; min-height: 22px; font-size: 8pt; font-weight: 600; }
QPushButton#expChip:hover { color: @FG; background: @BG3; border-color: @ACCENT_LINE; }
QPushButton#expChip:pressed { background: @BG2; }
/* Command palette (Ctrl+K). */
QDialog#expPalette { background: @BG1; border: 1px solid @EDGE; }
QLineEdit#experimentalQuickSearch { background: transparent; border: 0; border-radius: 0;
    padding: 9px 0; font-size: 11pt; }
QLineEdit#experimentalQuickSearch:focus { background: transparent; border: 0; }
QListWidget#experimentalQuickResults { background: transparent; border: 0; border-top: 1px solid @BORDER_SOFT;
    border-radius: 0; padding: 6px; }
QListWidget#experimentalQuickResults::item { padding: 6px 9px; border: 0; border-left: 3px solid transparent;
    border-radius: 6px; color: @FG_DIM; }
QListWidget#experimentalQuickResults::item:hover:!selected { background: @HILITE; }
QListWidget#experimentalQuickResults::item:selected { background: @ACCENT_TINT; color: @FG; border-left: 3px solid @ACCENT; }
QLabel#expPaletteHint { color: @MUTED; background: @SIDEBAR; border-top: 1px solid @BORDER_SOFT;
    padding: 7px 16px; font-size: 8pt; }
QTabWidget#expPageStack::pane { border: 0; background: @BG0; }
QTabWidget#expPageStack > QTabBar { max-height: 0px; }
/* App-wide visual tokens. Compact, restrained, consistent with all palettes. */
/* Cards stand clear of the page: a lifted surface with a full hairline and a lit top edge. */
QGroupBox { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 @CARD_TOP, stop:1 @CARD_BOT);
    border: 1px solid @BORDER; border-top-color: @EDGE; border-radius: 10px;
    margin-top: 0; padding: 31px 10px 8px 10px; color: @FG; font-weight: 600; }
/* Classic's colour-coded title bar, at Studio density: sections stay scannable. */
QGroupBox::title { subcontrol-origin: border; subcontrol-position: top left;
    top: 10px; left: 12px; color: @FG; font-size: 10pt;
    border: 0; border-left: 3px solid @ACCENT; padding: 0 0 0 8px; background: transparent; font-weight: 700; }
/* The title reads in the text colour; the bar carries the section colour. Only caution/danger tint the text. */
QGroupBox#box_red::title { color: @DANGER; border-left-color: @DANGER; }
QGroupBox#box_green::title { color: @FG; border-left-color: @OK; }
QGroupBox#box_yellow::title { color: @WARN; border-left-color: @WARN; }
QGroupBox#box_blue::title { color: @FG; border-left-color: @INFO; }
QGroupBox#box_purple::title { color: @FG; border-left-color: @PURPLE; }
QGroupBox#box_grey::title { color: @FG_DIM; border-left-color: @MUTED; }
QGroupBox QGroupBox { background: transparent; border: none; border-top: 1px solid @BORDER_SOFT;
    padding: 22px 0 0; border-radius: 0; }
QGroupBox QGroupBox::title { left: 0; top: 6px; color: @FG_DIM; border-left: 0; padding: 0; font-size: 8pt; }
QLabel[role="title"] { color: @FG; font-size: 14pt; font-weight: 700; }
QLabel[role="muted"] { color: @MUTED; font-size: 8pt; }
QPushButton { background: @BG3; border: 1px solid @BORDER;
    color: @FG; border-radius: 7px; padding: 3px 9px; min-height: 17px;
    font-weight: 500; }
/* One flat ring per state: Classic's lit/shaded edges must not leak through. */
QPushButton:focus { border-color: @ACCENT_SOFT; }
QPushButton:hover { background: @BG4; border-color: @ACCENT_LINE; }
QPushButton:pressed { background: @BG2; border-color: @ACCENT_SOFT; }
QPushButton:checked { background: @ACCENT_TINT; border-color: @ACCENT_SOFT; color: @FG; }
QPushButton#btnAccent { background: @ACCENT; color: @ON_ACCENT; font-weight: 700;
    border: 1px solid @ACCENT; }
QPushButton#btnAccent:hover { background: @ACCENT_HI; border-color: @ACCENT_TOP; }
QPushButton#btnAccent:pressed { background: @ACCENT_SOFT; border-color: @ACCENT_SOFT; }
QPushButton#btnAccent:focus { border-color: @FG; }
QPushButton#btnDanger { color: @DANGER; border-color: @DANGER_SOFT; background: @DANGER_WASH; }
QPushButton#btnDanger:hover { background: @DANGER_TINT; border-color: @DANGER; }
QPushButton:disabled, QPushButton#btnAccent:disabled, QPushButton#btnDanger:disabled {
    color: @MUTED; background: transparent; border-color: @BORDER_SOFT; }
QPushButton#btnMini { font-size: 8pt; padding: 1px 7px; min-height: 15px; }
QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, Int64SpinBox {
    background: @WELL; border: 1px solid @BORDER; color: @FG;
    border-radius: 7px; padding: 2px 7px; min-height: 18px; }
QLineEdit:hover, QComboBox:hover, QSpinBox:hover, QDoubleSpinBox:hover, Int64SpinBox:hover {
    border-color: @EDGE; }
QLineEdit:focus, QComboBox:focus, QComboBox:on, QSpinBox:focus, QDoubleSpinBox:focus, Int64SpinBox:focus {
    border-color: @ACCENT; background: @FOCUS_WELL; }
QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, Int64SpinBox:disabled {
    border-color: @BORDER_SOFT; }
QCheckBox::indicator, QRadioButton::indicator { border: 1px solid @BORDER; }
QCheckBox::indicator:hover, QRadioButton::indicator:hover { border-color: @ACCENT; }
QCheckBox::indicator:checked { background: @ACCENT; border-color: @ACCENT; }
QCheckBox::indicator:disabled, QRadioButton::indicator:disabled { border-color: @BORDER_SOFT; }
QCheckBox::indicator:checked:disabled { background: @BG4; border-color: @BG4; }
QPlainTextEdit, QTextEdit, QListView, QTreeView, QTableView {
    background: @WELL; border: 1px solid @BORDER; border-radius: 6px; }
QTabWidget::pane { border-top: 1px solid @BORDER; background: transparent; }
/* Sub-tabs: the current one is a soft accent tab standing on the pane rule. */
QTabBar::tab { padding: 6px 13px; color: @MUTED; border-top-left-radius: 6px; border-top-right-radius: 6px; }
QTabBar::tab:hover:!selected { background: @HILITE; }
QTabBar::tab:selected { color: @ACCENT_HI; background: @ACCENT_TINT; border-bottom: 2px solid @ACCENT; }
QHeaderView::section { background: @BG1; border: 0; border-bottom: 1px solid @BORDER;
    color: @FG_DIM; font-weight: 600; padding: 4px 7px; }
QMenu { background: @BG1; border: 1px solid @BORDER; border-top-color: @EDGE; border-radius: 8px; padding: 5px; }
QMenu::item { border-radius: 5px; }
QFrame#expInlineDivider { background: @BORDER; border: 0; max-height: 1px; }
QSplitter::handle:horizontal { background: @BORDER_SOFT; width: 1px; }
QSplitter::handle:vertical { background: @BORDER_SOFT; height: 1px; }
QSplitter::handle:hover { background: @ACCENT_SOFT; }
QFrame#homeDetailCard { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 @CARD_TOP, stop:1 @CARD_BOT);
    border: 1px solid @BORDER; border-top-color: @EDGE; border-radius: 10px; }
QTreeView::item:selected, QListView::item:selected { border-radius: 5px; }
QTabWidget#homeDetailTabs::pane { border: 0; border-top: 1px solid @BORDER_SOFT; background: transparent; }
QTabWidget#homeDetailTabs > QTabBar::tab { padding: 5px 10px; }
QTabWidget#homeDetailTabs > QStackedWidget { background: transparent; }
QTabWidget#homeDetailTabs > QStackedWidget > QGroupBox { border: 0; background: transparent; padding: 6px 6px 4px 6px; }

/* ── Studio polish: depth from light, not from frames ── */
/* Top bar lit from above; its lower edge is the profile-coloured signal line (MainWindow::syncStatusChips). */
QFrame#expTopbar { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 @BEZEL_TOP, stop:1 @BG0); border-bottom: 0; }
QLabel#expTopHint { color: @MUTED; font-size: 8pt; padding-bottom: 3px; }
/* The rail darkens towards its foot; the current page is a marker and a wash, no box. */
QFrame#expRail { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 @SIDEBAR, stop:1 @BEZEL_BOT); }
QPushButton#expNavButton { border-radius: 8px; border-top-left-radius: 3px; border-bottom-left-radius: 3px; }
QPushButton#expNavButton:checked { color: @FG; border: 1px solid transparent; border-left: 3px solid @ACCENT;
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 @ACCENT_TINT, stop:0.75 @ACCENT_FADE, stop:1 transparent); }
/* Cards: a softer corner and a shaded foot, so they sit on the page rather than being drawn on it. */
QGroupBox { border-radius: 12px; border-bottom-color: @BTN_SHADE;
    background: qradialgradient(cx:0, cy:0, radius:1, fx:0, fy:0, stop:0 @GLOW_ACCENT, stop:0.5 @CARD_TOP, stop:1 @CARD_BOT); }
/* ...lit from the corner its title sits in, in the section's own colour. */
QGroupBox#box_red { background: qradialgradient(cx:0, cy:0, radius:1, fx:0, fy:0, stop:0 @GLOW_DANGER, stop:0.5 @CARD_TOP, stop:1 @CARD_BOT); }
QGroupBox#box_green { background: qradialgradient(cx:0, cy:0, radius:1, fx:0, fy:0, stop:0 @GLOW_OK, stop:0.5 @CARD_TOP, stop:1 @CARD_BOT); }
QGroupBox#box_yellow { background: qradialgradient(cx:0, cy:0, radius:1, fx:0, fy:0, stop:0 @GLOW_WARN, stop:0.5 @CARD_TOP, stop:1 @CARD_BOT); }
QGroupBox#box_blue { background: qradialgradient(cx:0, cy:0, radius:1, fx:0, fy:0, stop:0 @GLOW_INFO, stop:0.5 @CARD_TOP, stop:1 @CARD_BOT); }
QGroupBox#box_purple { background: qradialgradient(cx:0, cy:0, radius:1, fx:0, fy:0, stop:0 @GLOW_PURPLE, stop:0.5 @CARD_TOP, stop:1 @CARD_BOT); }
QGroupBox#box_grey { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 @CARD_TOP, stop:1 @CARD_BOT); }
QFrame#homeDetailCard { border-radius: 12px; border-bottom-color: @BTN_SHADE; }
/* Sub-tabs: segmented pills on the pane's hairline. */
QTabBar::tab { margin: 4px 3px 5px 0; padding: 5px 13px; border: 1px solid transparent; border-radius: 7px; background: transparent; }
QTabBar::tab:hover:!selected { color: @FG; background: @HILITE; }
QTabBar::tab:selected { color: @ACCENT_HI; background: @ACCENT_TINT; border: 1px solid @ACCENT_EDGE; }
/* Health's page list speaks the main rail's language. */
QTreeWidget#healthNav::item { border-left: 3px solid transparent; border-radius: 7px; border-top-left-radius: 3px;
    border-bottom-left-radius: 3px; margin: 1px 0; }
QTreeWidget#healthNav::item:hover:!selected { background: @HILITE; color: @FG; }
QTreeWidget#healthNav::item:selected { color: @FG; border-left: 3px solid @ACCENT;
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 @ACCENT_TINT, stop:1 @ACCENT_FADE); }
/* Tables: a quieter header, a clearer current row. */
QHeaderView::section { font-size: 8pt; padding: 5px 8px; }
QTreeView::item:hover:!selected, QTableView::item:hover:!selected { background: @HILITE; }
/* Menus and the tooltip: one surface, a hairline, the accent only where it points. */
QMenu::item { padding: 6px 24px 6px 10px; }
QMenu::item:selected { background: @ACCENT_TINT; color: @FG; }
QToolTip { background: @BG2; border: 1px solid @EDGE; border-left: 3px solid @ACCENT; border-radius: 6px; padding: 6px 10px; }
/* Scrollbars: slim, out of the way until hovered. */
QScrollBar:vertical { width: 9px; margin: 4px 2px 4px 0; }
QScrollBar:horizontal { height: 9px; margin: 0 4px 2px 4px; }
QScrollBar::handle:vertical, QScrollBar::handle:horizontal { background: @BORDER; border-radius: 3px; margin: 0; }
QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover { background: @ACCENT_SOFT; }
)QSS");
    const QColor accent(QString::fromLatin1(ACCENT));
    // Dark foreground on pale accent, white foreground on deep accent.
    const QString onAccent = textOnAccent().name();
    const bool isLight = lightTheme();
    // Longest names first so @BG doesn't eat @BG0 etc.
    // Card glow: the card's top surface with a trace of a signal colour (opaque, so it stays a card colour).
    const QByteArray cardTop = mix(BG1, BG2, 0.55).toLatin1();
    const auto glow = [&cardTop](const char *c) { return mix(cardTop.constData(), c, 0.10); };
    const std::pair<const char *, QString> toks[] = {
        {"@GLOW_ACCENT", glow(ACCENT)}, {"@GLOW_DANGER", glow(DANGER)}, {"@GLOW_OK", glow(OK)}, {"@GLOW_WARN", glow(WARN)},
        {"@GLOW_INFO", glow(INFO)}, {"@GLOW_PURPLE", glow(PURPLE)},
        {"@BORDER_SOFT", BORDER_SOFT}, {"@ACCENT_SOFT", ACCENT_SOFT}, {"@DANGER_SOFT", DANGER_SOFT},
        {"@DANGER_TINT", rgba(DANGER, 0.14)}, {"@DANGER_WASH", rgba(DANGER, 0.05)}, {"@ACCENT_TINT2", rgba(ACCENT, 0.10)},
        {"@ACCENT_LINE", rgba(ACCENT, 0.32)}, {"@ACCENT_EDGE", rgba(ACCENT, 0.22)}, {"@ACCENT_FADE", rgba(ACCENT, 0.02)}, {"@FOCUS_WELL", mix(WELL, ACCENT, 0.05)}, {"@ACCENT_TINT", rgba(ACCENT, 0.16)}, {"@ACCENT_DEEP", accent.darker(135).name()},
        {"@HILITE2", isLight ? QStringLiteral("rgba(0, 0, 0, 0.08)") : QStringLiteral("rgba(255, 255, 255, 0.09)")},
        {"@HILITE", isLight ? QStringLiteral("rgba(0, 0, 0, 0.04)") : QStringLiteral("rgba(255, 255, 255, 0.05)" )}, {"@ACCENT_HALO", rgba(ACCENT, 0.28)},
        {"@ACCENT_TOP", accent.lighter(125).name()}, {"@ACCENT_HI", isLight ? accent.darker(115).name() : accent.lighter(112).name()}, {"@ON_ACCENT", onAccent},
        // Derived surfaces (each palette's own hue): bezel and page floor below the window colour,
        // the rail and well shadows below that; edges are the border lifted towards the text colour.
        {"@BEZEL_TOP", mix(BG1, BG0, 0.35)}, {"@BEZEL_BOT", mix(BG0, WELL, 0.55)}, {"@PAGE_BOT", mix(BG0, WELL, 0.65)},
        {"@RAIL_SHADE", isLight ? mix(WELL, BORDER, 0.6) : mix(WELL, "#000000", 0.35)}, {"@RAIL", mix(WELL, BG0, 0.25)},
        {"@BTN_SHADE", isLight ? mix(BORDER, FG, 0.08) : mix(BORDER, "#000000", 0.35)}, {"@CARD_TOP", mix(BG1, BG2, 0.55)}, {"@CARD_BOT", mix(BG1, BG2, 0.15)}, {"@SIDEBAR", mix(BG0, WELL, 0.6)}, {"@ROW_ALT", mix(WELL, BG1, 0.6)},
        {"@KEY_TOP", mix(BG3, BG4, 0.45)}, {"@EDGE", mix(BORDER, FG, 0.13)},
        {"@BRANDW", QString::number(brandInset())}, {"@BRANDPT", QString::number(FONT_PT + 2)},
        {"@FG_DIM", FG_DIM}, {"@BORDER", BORDER}, {"@ACCENT", ACCENT}, {"@DANGER", DANGER},
        {"@MUTED", MUTED}, {"@PURPLE", PURPLE}, {"@INFO", INFO}, {"@WARN", WARN}, {"@WELL", WELL}, {"@OK", OK},
        {"@BG0", BG0}, {"@BG1", BG1}, {"@BG2", BG2}, {"@BG3", BG3}, {"@BG4", BG4}, {"@FG", FG},
        {"@FONT", QString::number(FONT_PT)}, {"@RAD", QString::number(RADIUS)}};
    for (const auto &[k, v] : toks) s.replace(QString::fromLatin1(k), v);
    if (g_compact) s += QStringLiteral(R"QSS(
QPushButton { padding: 2px 6px; }
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox { padding-top: 1px; padding-bottom: 1px; }
QTabBar::tab { padding-top: 3px; padding-bottom: 3px; }
QTreeView::item, QTableView::item { padding: 1px 3px; }
QHeaderView::section { padding: 3px 5px; }
)QSS");
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
        if (m == PM_LayoutHorizontalSpacing || m == PM_LayoutVerticalSpacing) return g_compact ? 2 : 4;
        return QProxyStyle::pixelMetric(m, o, w);
    }
    // Used per widget pair when the layout has no spacing at all (the nested case).
    int layoutSpacing(QSizePolicy::ControlType, QSizePolicy::ControlType, Qt::Orientation,
                      const QStyleOption * = nullptr, const QWidget * = nullptr) const override {
        return g_compact ? 2 : 4;
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
