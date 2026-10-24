// Unit test for the portal trigger strings of scene shortcuts (shortcuts.h). Plain asserts.
#include "../src/shortcuts.h"

#include <QGuiApplication>
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static QString t(const char *portable) { return portalTrigger(QKeySequence::fromString(QLatin1String(portable), QKeySequence::PortableText)); }

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    CHECK(t("Ctrl+Alt+1") == QLatin1String("CTRL+ALT+1"));
    CHECK(t("Meta+F5") == QLatin1String("LOGO+F5"));
    CHECK(t("Ctrl+Shift+G") == QLatin1String("CTRL+SHIFT+g"));          // letters are lower-case keysyms
    CHECK(t("Alt+PgUp") == QLatin1String("ALT+Prior"));
    CHECK(t("Ctrl+Space") == QLatin1String("CTRL+space"));
    CHECK(t("Ctrl+,") == QLatin1String("CTRL+comma"));
    CHECK(t("Ctrl+Alt+1, Ctrl+Alt+2") == QLatin1String("CTRL+ALT+1"));  // only the first combination
    CHECK(t("").isEmpty());
    CHECK(GlobalShortcuts::idFor("Gaming X3D") == QLatin1String("scene:Gaming X3D"));

    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::puts("shortcuts: all checks passed");
    return failures ? 1 : 0;
}
