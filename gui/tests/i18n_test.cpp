// Unit test for the interface-language choice (i18n::resolve). Plain asserts, like the other tests.
#include "../src/i18n.h"

#include <QCoreApplication>
#include <QStringList>
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QStringList avail{"en", "tr"};
    auto r = [&](const char *saved, const char *env, const char *sys) {
        return i18n::resolve(QString::fromLatin1(saved), QString::fromLatin1(env), QString::fromLatin1(sys), avail);
    };
    // Automatic: the desktop locale decides (any spelling), unknown languages fall back to English.
    CHECK(r("auto", "", "tr_TR") == "tr");
    CHECK(r("", "", "tr_TR.UTF-8") == "tr");
    CHECK(r("auto", "", "tr-TR") == "tr");
    CHECK(r("auto", "", "de_DE") == "en");
    CHECK(r("auto", "", "") == "en");
    // The environment overrides the desktop; a saved choice overrides both.
    CHECK(r("auto", "tr", "en_US") == "tr");
    CHECK(r("auto", "fr", "tr_TR") == "en");
    CHECK(r("tr", "en", "en_US") == "tr");
    CHECK(r("en", "tr", "tr_TR") == "en");
    CHECK(r("xx", "tr", "tr_TR") == "en");
    // Not offered -> English, even when the locale matches.
    CHECK(i18n::resolve("auto", "", "tr_TR", {"en"}) == "en");
    return failures ? 1 : 0;
}
