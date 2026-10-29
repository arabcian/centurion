// Unit test for the Health → Kernel log view logic (klogview.cpp). Plain asserts, like sleepadvice_test.cpp.
#include "../src/klogview.h"

#include <QCoreApplication>
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

using namespace klogview;

static Line at(const char *text) { return *parse(QString::fromUtf8(text)); }

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    // Format written by KlogPage::persist (level padded to five characters).
    const auto a = parse(QStringLiteral("2026-10-29 01:00:00  boot=aaaaaaaa seq=7  ERR    [12.000001] nvme nvme0: I/O 12 QID 3 timeout"));
    CHECK(a && a->level == levelIndex(QStringLiteral("ERR")) && a->level == 3);
    CHECK(a && a->boot == QLatin1String("aaaaaaaa") && a->seq == 7 && a->time == QLatin1String("2026-10-29 01:00:00"));
    CHECK(a && a->msg == QLatin1String("nvme nvme0: I/O 12 QID 3 timeout"));
    CHECK(a && a->key == QLatin1String("nvme nvme#: I/O # QID # timeout"));

    CHECK(!parse(QStringLiteral("garbage")).has_value());
    CHECK(!parse(QStringLiteral("2026-10-29 01:00:00  boot=aaaaaaaa seq=1  NOTICE  [1.0] x")).has_value());

    CHECK(levelIndex(QStringLiteral("EMERG")) == 0 && levelIndex(QStringLiteral("WARN")) == 4 && levelIndex(QStringLiteral("DEBUG")) == -1);
    CHECK(normalize(QStringLiteral("a 12 b 3")) == QLatin1String("a # b #"));
    CHECK(normalize(QStringLiteral("  pid 42 ")) == QLatin1String("pid #"));

    // Repeats differing only in numbers form one row; the other message stays its own row.
    const std::vector<Line> lines{
        at("2026-10-29 01:00:00  boot=aaaaaaaa seq=1  ERR    [1.0] nvme nvme0: timeout 5"),
        at("2026-10-29 01:00:01  boot=aaaaaaaa seq=2  ERR    [2.0] other event"),
        at("2026-10-29 01:00:02  boot=aaaaaaaa seq=3  ERR    [3.0] nvme nvme1: timeout 9"),
        at("2026-10-29 01:00:09  boot=aaaaaaaa seq=4  ERR    [4.0] nvme nvme1: timeout 11"),
    };
    const auto grouped = group(lines, true);
    CHECK(grouped.size() == 2);
    CHECK(grouped.size() == 2 && grouped[0].count == 3 && grouped[0].first.seq == 1);
    CHECK(grouped.size() == 2 && grouped[0].last == QLatin1String("2026-10-29 01:00:09"));
    CHECK(grouped.size() == 2 && grouped[1].count == 1 && grouped[1].first.seq == 2);

    const auto ungrouped = group(lines, false);
    CHECK(ungrouped.size() == 4 && ungrouped[3].count == 1);

    // "Only new": a message first seen in an earlier boot is not new in a later one.
    const std::vector<Line> saved{
        at("2026-10-28 09:00:00  boot=bbbbbbbb seq=1  WARN   [1.0] gpu fault 4"),
        at("2026-10-29 09:00:00  boot=aaaaaaaa seq=1  WARN   [1.0] gpu fault 8"),
        at("2026-10-29 09:00:01  boot=aaaaaaaa seq=2  WARN   [2.0] fresh message"),
    };
    const QHash<QString, QString> first = firstBoots(saved);
    CHECK(first.value(QStringLiteral("gpu fault #")) == QLatin1String("bbbbbbbb"));
    CHECK(first.value(QStringLiteral("fresh message")) == QLatin1String("aaaaaaaa"));
    CHECK(!first.contains(QStringLiteral("never seen")));

    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("klogview: all checks passed\n");
    return 0;
}
