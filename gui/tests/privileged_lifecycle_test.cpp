#include "../src/privileged.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>
#include <cstring>

// Fixture compiled with CENTURION_PRIVILEGED_FIXTURE. Never invokes pkexec.
namespace systools { QString installCommand(const QString &) { return {}; } }
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc == 2) {
        // A root-like process still alive after the GUI timeout. The runner
        // intentionally cannot kill this child in the fixture-only build.
        const int delay = !std::strcmp(argv[1], "ryzen-co-helper") ? 60000 : 100;
        QTimer::singleShot(delay, &app, [&app] {
            std::puts("{\"ok\":true,\"helper_version\":\"fixture\"}"); std::fflush(stdout); app.quit();
        });
        return app.exec();
    }
    QObject owner;
    QEventLoop loop;
    QTimer heartbeat;
    int ticks=0, notices=0, callbacks=0;
    bool bad=false, watchdog=false;
    const auto lease=privileged::acquireCpuCurveLease(&owner);
    if (!lease) return 1;
    QObject::connect(&heartbeat, &QTimer::timeout, &loop, [&] { ++ticks; });
    heartbeat.start(100);
    QTimer::singleShot(70000, &loop, [&] { watchdog=true; loop.quit(); });
    privileged::runUntilExit(QStringLiteral("ryzen-co-helper"), QJsonObject{{"op","set_coall"}}, &owner,
        [&](const privileged::Result &r) {
            ++callbacks;
            bad |= r.ok() || notices!=1 || ticks<400 || privileged::resourceActive(QStringLiteral("ryzen-co-helper"));
            bad |= !privileged::releaseCpuCurveLease(lease);
            loop.quit();
        }, 50, [&] {
            ++notices;
            bad |= callbacks!=0 || !privileged::resourceActive(QStringLiteral("ryzen-co-helper"));
            bad |= privileged::releaseCpuCurveLease(lease);
        }, lease);
    loop.exec();
    if (bad || watchdog || callbacks!=1 || !privileged::activeOperations().isEmpty()) return 1;
    // Owner loss suppresses callback, but the child is still tracked to exit.
    QEventLoop ownerLoop;
    auto *gone=new QObject;
    bool delivered=false;
    privileged::runUntilExit(QStringLiteral("fixture-short"), QJsonObject{}, gone,
        [&](const privileged::Result &) { delivered=true; }, 20);
    delete gone;
    QTimer::singleShot(1000, &ownerLoop, &QEventLoop::quit); ownerLoop.exec();
    if (delivered || !privileged::activeOperations().isEmpty()) return 1;
    // Firmware batch uses the raw-array payload overload.
    QEventLoop batchLoop;
    int batchNotices=0, batchCallbacks=0;
    bool batchBad=false, batchWatchdog=false;
    QTimer::singleShot(3000, &batchLoop, [&] { batchWatchdog=true; batchLoop.quit(); });
    privileged::runUntilExit(QStringLiteral("fixture-batch"), QByteArrayLiteral("[{\"path\":\"fixture\",\"value\":1}]"), &owner,
        [&](const privileged::Result &r) {
            ++batchCallbacks;
            batchBad |= r.ok() || batchNotices!=1 || privileged::resourceActive(QStringLiteral("fixture-batch"));
            batchLoop.quit();
        }, 20, [&] {
            ++batchNotices;
            batchBad |= batchCallbacks!=0 || !privileged::resourceActive(QStringLiteral("fixture-batch"));
        });
    batchLoop.exec();
    return batchBad || batchWatchdog || batchCallbacks!=1 || !privileged::activeOperations().isEmpty() ? 1 : 0;
}
