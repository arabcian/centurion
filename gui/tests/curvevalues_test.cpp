// curvevalues: per-core Curve Optimizer values from the curve journal as readable lines.
#include "../src/curvevalues.h"
#include <QJsonDocument>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAILED %s line %d\n", #x, __LINE__); return EXIT_FAILURE; } } while (0)
int main() {
    const QJsonObject co = QJsonDocument::fromJson(R"({"0.0.10":-15,"0.0.2":-25,"1.0.0":-30,"all":-5})").object();
    const QStringList l = curvevalues::lines(QStringLiteral("cpu_co"), co);
    CHECK(l.size() == 4 && l.at(0) == QStringLiteral("All cores: -5"));
    CHECK(l.at(1) == QStringLiteral("CCD0 core 2: -25") && l.at(2) == QStringLiteral("CCD0 core 10: -15") && l.at(3) == QStringLiteral("CCD1 core 0: -30"));
    CHECK(curvevalues::coOffset(co, 0, 2) == -25);
    CHECK(curvevalues::coOffset(co, 1, 7) == -5);   // no per-core entry: the all-core offset
    CHECK(!curvevalues::coOffset(QJsonObject{}, 0, 0));
    CHECK(curvevalues::lines(QStringLiteral("cpu_co"), QJsonValue()).isEmpty());
    CHECK(curvevalues::lines(QStringLiteral("cpu_uv"), QJsonObject{{"core", -80}}) == QStringList{QStringLiteral("core: -80")});
    CHECK(curvevalues::label(QStringLiteral("'Daily'"), QStringLiteral("0a1b2c3d")) == QStringLiteral("'Daily' #0a1b2c3d"));
    CHECK(curvevalues::label(QStringLiteral("stock"), QString()) == QStringLiteral("stock"));
    CHECK(!curvevalues::coKey(QStringLiteral("0.0")) && !curvevalues::coKey(QStringLiteral("a.b.c")) && !curvevalues::coKey(QStringLiteral("0.0.-1")));
    return EXIT_SUCCESS;
}
