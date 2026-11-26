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
    CHECK(stem(QStringLiteral("nvme nvme0: I/O 12 QID 3 timeout, aborting")) == QLatin1String("timeout, aborting"));
    CHECK(stem(QStringLiteral("usb 1-2: device descriptor read/64, error -71")) == QLatin1String("device descriptor"));
    CHECK(stem(QStringLiteral("mce: |CE| 0x12")).isEmpty());
    CHECK(stem(QStringLiteral("ACPI BIOS Error (bug): Could not resolve symbol")) == QLatin1String("ACPI BIOS Error (bug): Could not resolve symbol"));

    // Repeats differing only in numbers form one row; the other message stays its own row.
    const std::deque<Line> lines{
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

    const auto full=at("2026-10-29 01:00:00  boot=aaaaaaaa-1111-2222-3333-444444444444 seq=7  ERR [1.0] nvme nvme1: timeout");
    const auto collision=at("2026-10-29 01:00:00  boot=aaaaaaaa-aaaa-bbbb-cccc-dddddddddddd seq=7  ERR [1.0] nvme nvme10: timeout");
    CHECK(fullBootId(full.boot)); CHECK(!fullBootId(QStringLiteral("aaaaaaaa")));
    CHECK(!parse(QStringLiteral("2026-10-29 01:00:00 boot=aaaaaaaa seq=18446744073709551616 ERR [1.0] x")));
    CHECK(!parse(QStringLiteral("2026-10-29 01:00:00 boot=not-a-boot seq=1 ERR [1.0] x")));
    Filter f{full.boot,QStringLiteral("nvme"),QStringLiteral("nvme1"),QStringLiteral("2026-10-29"),QStringLiteral("2026-10-29"),5};
    CHECK(f.valid()); CHECK(f.matches(full,QStringLiteral("nvme"),5,f.devicePattern()));
    CHECK(!f.matches(collision,QStringLiteral("nvme"),5,f.devicePattern()));
    CHECK(!f.matches(full,QStringLiteral("nvme"),6,f.devicePattern()));
    f.boot.clear(); CHECK(!f.matches(collision,QStringLiteral("nvme"),5,f.devicePattern()));
    f.fromDate=QStringLiteral("2026-02-30"); CHECK(!f.valid());
    ContextWindow exact(full.boot,7,2,3); exact.append(collision); exact.append(*a); exact.append(full);
    CHECK(exact.rows().size()==1 && exact.rows()[0].first.boot==full.boot);
    CHECK(group({full,collision},true).size()==2);

    ContextWindow context(QStringLiteral("aaaaaaaa"), 10, 2, 3);
    Line sample = lines.front();
    for (quint64 i = 1; i <= 8000; ++i) {
        sample.seq = i;
        context.append(sample);
        Line other = sample;
        other.boot = QStringLiteral("bbbbbbbb");
        context.append(other);
    }
    const auto window = context.rows();
    CHECK(context.mark() == 2 && window.size() == 6);
    CHECK(window.size() == 6 && window.front().first.seq == 8 && window[2].first.seq == 10 && window.back().first.seq == 13);
    ContextWindow missing(QStringLiteral("cccccccc"), 10, 2, 3);
    missing.append(sample);
    CHECK(missing.mark() == -1 && missing.rows().empty());
    ContextWindow boundary(QStringLiteral("aaaaaaaa"), 8000, 0, 0);
    boundary.append(sample);
    CHECK(boundary.mark() == 0 && boundary.rows().size() == 1);

    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("klogview: all checks passed\n");
    return 0;
}
