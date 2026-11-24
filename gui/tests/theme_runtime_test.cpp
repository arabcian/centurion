// Isolated settings; no widgets, hardware access or real user config writes.
#include "../src/theme.h"
#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QByteArray>
#include <cstdio>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "Failed: %s\n", #x); return 1; } } while (false)
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tmp.path());
    qunsetenv("CENTURION_UI");
    CHECK(theme::saveUiMode(true));
    theme::load();
    CHECK(theme::experimentalEnabled());
    CHECK(theme::saveUiMode(false));
    CHECK(theme::experimentalEnabled()); // pending choice isn't the running shell
    theme::load();
    CHECK(!theme::experimentalEnabled());
    qputenv("CENTURION_UI", QByteArray("experimental"));
    theme::load();
    CHECK(theme::experimentalEnabled());
    theme::setRestartGuard([] { return false; });
    theme::requestRestart();
    CHECK(!theme::restartRequested());
    theme::setRestartGuard({});
    CHECK(theme::saveCompactDensity(true));
    CHECK(!theme::compactDensity());
    theme::load(); CHECK(theme::compactDensity());
    CHECK(theme::saveCompactDensity(false)); theme::load(); CHECK(!theme::compactDensity());
    CHECK(theme::typographyPreference().points == theme::FONT_PT);
    CHECK(!theme::saveTypography({{}, {}, 100}));
    CHECK(!theme::saveTypography({QStringLiteral("bad;family"), {}, 9}));
    CHECK(theme::saveTypography({QStringLiteral("Missing font"), QStringLiteral("Missing fixed font"), 12}));
    CHECK(theme::typographyPreference().points == 12);
    CHECK(theme::typographyPreference().numericFamily == QStringLiteral("Missing fixed font"));
    CHECK(theme::saveTypography({}));
    CHECK(theme::saveTheme(QStringLiteral("oled-black"))); theme::load();
    CHECK(QByteArray(theme::BG0) == QByteArray("#000000"));
    const auto original = theme::paletteColors(QStringLiteral("graphite"));
    CHECK(original.size() == 19);
    auto custom = original;
    custom[8] = QStringLiteral("#aabbcc");
    CHECK(!theme::saveCustomPalette({QStringLiteral("#ffffff")}));
    CHECK(theme::saveCustomPalette(custom));
    CHECK(theme::saveTheme(QStringLiteral("custom")));
    theme::load();
    CHECK(theme::currentTheme() == QStringLiteral("custom"));
    CHECK(QByteArray(theme::FG) == QByteArray("#aabbcc"));
    CHECK(theme::paletteColors(QStringLiteral("custom")) == custom);
    CHECK(theme::saveTheme(QStringLiteral("graphite")));
    theme::load();
    CHECK(theme::paletteColors(theme::currentTheme()) == original);
    CHECK(theme::contrastRatio(QColor(Qt::black), QColor(Qt::white)) > 20.99);
    CHECK(theme::contrastRatio(QColor(Qt::black), QColor(Qt::black)) == 1.0);
    CHECK(theme::contrastChecks({}).isEmpty());
    for (const auto &entry : theme::themes()) {
        const auto checks = theme::contrastChecks(theme::paletteColors(entry.first));
        CHECK(!checks.isEmpty());
        bool selection = false, disabled = false;
        for (const auto &check : checks) {
            CHECK(check.ratio >= 1 && check.ratio <= 21.00001);
            if (check.role == QStringLiteral("Selection text")) { CHECK(check.ratio >= 4.5); selection = true; }
            if (check.exempt) disabled = true;
        }
        CHECK(selection && disabled);
    }
    return 0;
}
