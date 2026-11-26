// Unit test for the Boot history flight-recorder text (flightrec.cpp).
// Plain asserts, like sleepadvice_test.cpp.
#include "../src/flightrec.h"
#include "../src/kernelwalltime.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <cstdio>
#include <ctime>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main(int argc, char **argv) {
    qputenv("TZ", "UTC");
    tzset();
    QCoreApplication app(argc, argv);

    const auto slot = [](const QByteArray &json) {
        return json.leftJustified(flightrec::SLOT_BYTES - 1, ' ') + '\n';
    };
    const auto header = slot(R"({"boot":"00000000-0000-0000-0000-00000000000A","interval":5})");
    const auto other = slot(R"({"boot":"00000000-0000-0000-0000-00000000000b","interval":30,"s":0,"t":1})");
    const auto valid = slot(R"({"s":1,"t":2})");
    const auto partial = slot(R"({"s":2,"t":3})").chopped(1);
    const auto ring = flightrec::parseRing(header + other + valid + partial);
    CHECK(ring.value("boot").toString() == QStringLiteral("00000000-0000-0000-0000-00000000000a"));
    CHECK(ring.value("interval").toInteger() == 5);
    const auto parsed = ring.value("samples").toArray();
    CHECK(parsed.size() == 1 && parsed.at(0).toObject().value("s").toInteger() == 1);
    CHECK(flightrec::parseRing(slot("{bad") + header).isEmpty());
    CHECK(flightrec::parseRing(slot(R"({"boot":"12345678","interval":5})") + header).isEmpty());
    CHECK(flightrec::parseRing(slot(R"({"boot":"00000000-0000-0000-0000-00000000000a","interval":0})") + header).isEmpty());
    CHECK(flightrec::parseRing(header.chopped(1)).isEmpty());

    // Across suspend, wall time advances while the monotonic clock is stopped.
    const kernelwalltime::Anchor before{10000000, 1700000000000};
    const kernelwalltime::Anchor after{12000000, 1700003602000};
    CHECK(before.toWallMs(9000000) == 1699999999000);
    CHECK(after.toWallMs(11000000) == 1700003601000);
    CHECK(after.toWallMs(12000001) == 0);
    CHECK(after.toWallMs(-1) == 0);

    // 30 samples, 5 s apart; the CPU heats up, the GPU reading appears only while the dGPU is awake.
    QJsonArray s;
    for (int i = 0; i < 30; ++i) {
        QJsonObject o{{"s", i}, {"t", 3600 + 5 * i}, {"cpu_c", 60.0 + i}, {"mhz", 4100.0}, {"pkg_w", 12.5},
                      {"dgpu", i < 20 ? "suspended" : "active"}, {"fan", QJsonArray{2400, 2600}}};
        if (i >= 20) o.insert(QStringLiteral("gpu_c"), 70 + i);
        s.append(o);
    }
    const QString t = flightrec::text(s);
    const QStringList lines = t.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    CHECK(lines.size() == 3 + flightrec::ROWS);                          // title, peaks, header, last rows only
    CHECK(lines.at(0).contains(QStringLiteral("30 sample(s), 01:00:00 – 01:02:25")));
    CHECK(lines.at(1) == QStringLiteral("Peaks: CPU 89 °C, GPU 99 °C, CPU package 13 W, load —"));  // 12.5 rounds up; no load readings
    CHECK(lines.at(3).startsWith(QStringLiteral("01:00:30")));           // the first 6 samples are not listed
    CHECK(lines.at(3).contains(QStringLiteral("—")) && lines.at(3).contains(QStringLiteral("suspended")));  // no GPU reading while asleep
    CHECK(lines.last().contains(QStringLiteral("89")) && lines.last().contains(QStringLiteral("99"))
          && lines.last().contains(QStringLiteral("12.5")) && lines.last().endsWith(QStringLiteral("2400/2600")));

    // A sample with nothing but its time: every column is "—", no crash.
    const QString bare = flightrec::text(QJsonArray{QJsonObject{{"s", 0}, {"t", 0}}});
    CHECK(bare.contains(QStringLiteral("Peaks: CPU — °C, GPU — °C, CPU package — W, load —")));
    CHECK(bare.trimmed().endsWith(QStringLiteral("—")));

    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::puts("flightrec: all checks passed");
    return failures ? 1 : 0;
}
