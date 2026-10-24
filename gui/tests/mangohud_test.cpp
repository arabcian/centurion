// Unit test for the MangoHud overlay block (mangohud.cpp). Plain asserts.
#include "../src/mangohud.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

using namespace mangohud;

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QString exec = QStringLiteral("exec=cat /run/user/1000/centurion-overlay");
    const QString cmd = execCommand(QStringLiteral("/run/user/1000/centurion-overlay"));
    CHECK(QStringLiteral("exec=") + cmd == exec);
    // A path MangoHud would split or a shell would read differently: the self-contained command instead.
    CHECK(execCommand(QStringLiteral("/home/a b/x")) == QLatin1String("centurion-gamemode OVERLAY"));
    CHECK(execCommand(QStringLiteral("/run/$(x)")) == QLatin1String("centurion-gamemode OVERLAY"));
    CHECK(execCommand(QString()) == QLatin1String("centurion-gamemode OVERLAY"));
    CHECK(overlayLine(QStringLiteral("Gaming"), QStringLiteral("Performance")) == QString::fromUtf8("Gaming · Performance"));
    CHECK(overlayLine(QString(), QStringLiteral("Quiet")) == QLatin1String("Quiet"));

    // No config yet: MangoHud's default elements come along, since legacy_layout=0 draws only what is listed.
    QString c = withOverlay(QString(), cmd);
    CHECK(hasOverlay(c));
    CHECK(c.contains(exec) && c.contains("custom_text=Centurion") && c.contains("legacy_layout=0"));
    for (const char *k : {"\ngpu_stats\n", "\ncpu_stats\n", "\nfps\n", "\nframe_timing\n"}) CHECK(c.contains(QLatin1String(k)));
    CHECK(withoutOverlay(c).isEmpty());
    CHECK(withOverlay(c, cmd) == c);  // idempotent

    // A non-legacy config keeps its own layout: only the two lines are added.
    const QString mine = QStringLiteral("legacy_layout=false\nfps\nram\nposition=top-right");
    c = withOverlay(mine, cmd);
    CHECK(!c.contains("legacy_layout=0") && !c.contains("\ngpu_stats"));
    CHECK(c.startsWith(mine + '\n'));
    CHECK(withoutOverlay(c) == mine + '\n');

    // Legacy config that names some defaults itself (or turns them off): those are not repeated.
    const QString legacy = QStringLiteral("# my hud\ncpu_stats=0\nfps\n");
    c = withOverlay(legacy, cmd);
    CHECK(c.contains("legacy_layout=0") && c.contains("\ngpu_stats\n") && c.contains("\nframe_timing\n"));
    CHECK(c.count(QStringLiteral("cpu_stats")) == 1 && c.count(QRegularExpression("^fps$", QRegularExpression::MultilineOption)) == 1);
    CHECK(withoutOverlay(c) == legacy);

    // Removing keeps whatever the user wrote after the block.
    CHECK(withoutOverlay(withOverlay(mine, cmd) + "vram\n") == mine + "\nvram\n");

    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::puts("mangohud: all checks passed");
    return failures ? 1 : 0;
}
