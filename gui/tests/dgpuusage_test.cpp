// Unit test for the long-awake warning of DgpuUsage (dgpuusage.cpp). Plain asserts, like the other tests.
// A fake sysfs device and a temporary XDG_STATE_HOME; time is driven by the caller.
#include "../src/dgpuusage.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QThreadPool>
#include <QTemporaryDir>
#include <cstdio>
#include <vector>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir state, dev;
    qputenv("XDG_STATE_HOME", state.path().toUtf8());
    if (!dev.isValid() || !QDir().mkpath(dev.path() + QStringLiteral("/power"))) return 1;
    auto setFile = [&](const char *name, const QByteArray &v) {
        QFile f(dev.path() + QLatin1String("/power/") + QLatin1String(name));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(v);
    };

    std::vector<qint64> minutes;  // one entry per longAwake
    QString holders;
    DgpuUsage usage(dev.path(), [](const QString &) { return QStringLiteral("ffmpeg"); });
    QObject::connect(&usage, &DgpuUsage::longAwake, [&](qint64 m, const QString &h) { minutes.push_back(m); holders = h; });

    // Seconds on a clock that moves only when the test says so: wall and monotonic time advance together,
    // so no system suspend is seen between samples.
    const qint64 base = 1700000000000;
    auto at = [&](qint64 sec, bool battery, const char *status) {
        setFile("runtime_status", status);
        setFile("runtime_active_time", QByteArray::number(sec * 1000));
        setFile("runtime_suspended_time", QByteArray::number(sec * 1000));
        usage.sample(battery, -1, base + sec * 1000, sec * 1000);
    };

    // Awake on battery from 60 s: nine minutes is not yet a warning, ten is, once, with the wake's holder.
    at(0, true, "suspended");
    for (qint64 s = 60; s <= 600; s += 60) at(s, true, "active");
    CHECK(minutes.empty());
    QThreadPool::globalInstance()->waitForDone();  // the holder scan of the wake finishes on a worker thread
    app.processEvents();
    at(660, true, "active");
    CHECK(minutes.size() == 1 && minutes[0] == 10);
    CHECK(holders == QLatin1String("ffmpeg"));
    for (qint64 s = 720; s <= 1200; s += 60) at(s, true, "active");  // the same stretch: no second warning
    CHECK(minutes.size() == 1);

    // A suspend ends the stretch; the next awake stretch is warned about on its own.
    at(1260, true, "suspended");
    for (qint64 s = 1320; s <= 1920; s += 60) at(s, true, "active");
    CHECK(minutes.size() == 2);

    // Plugged in ends the stretch too: a battery stretch counts from its own start.
    at(1980, false, "active");
    for (qint64 s = 2040; s <= 2580; s += 60) at(s, true, "active");
    CHECK(minutes.size() == 2);
    at(2640, true, "active");
    CHECK(minutes.size() == 3);

    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
