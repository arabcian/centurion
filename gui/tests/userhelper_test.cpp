#include "../src/userhelper.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>

// Standalone fixture, never launches any installed hardware helper.
namespace privileged { QString helperPath(const QString &) { return {}; } }
static bool exercise(const QString &mode, bool expected, int timeout = 2000) {
    QObject owner;
    QEventLoop loop;
    int callbacks = 0;
    bool result = false;
    bool watchdog = false;
    QTimer::singleShot(5000, &loop, [&] { watchdog = true; loop.quit(); });
    userhelper::runProgram(QCoreApplication::applicationFilePath(), {mode}, {}, &owner,
        [&](const QJsonObject &r) {
            ++callbacks; result = r.value(QStringLiteral("ok")).toBool();
            // Let late finished/error notifications drain, detect duplicates.
            QTimer::singleShot(100, &loop, &QEventLoop::quit);
        }, timeout);
    loop.exec();
    return !watchdog && callbacks == 1 && result == expected;
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc == 2) {
        if (!std::strcmp(argv[1], "crash")) { std::puts("{\"ok\":true}"); std::fflush(stdout); std::raise(SIGABRT); return 2; }
        if (!std::strcmp(argv[1], "timeout")) { QTimer::singleShot(3000, &app, &QCoreApplication::quit); return app.exec(); }
        if (!std::strcmp(argv[1], "overflow")) { for (int i=0; i<5*1024*1024; ++i) std::putchar('x'); return 0; }
        if (!std::strcmp(argv[1], "malformed")) { std::puts("{} "); return 0; }
        if (!std::strcmp(argv[1], "failure")) { std::puts("{\"ok\":false,\"error\":\"fixture\"}"); return 2; }
        std::puts("{\"ok\":true}"); return std::strcmp(argv[1], "success") ? 1 : 0;
    }
    for (const auto &mode : {"success", "nonzero", "failure", "crash", "overflow", "malformed", "timeout"}) {
        const bool ok = exercise(QString::fromLatin1(mode), !std::strcmp(mode, "success"), !std::strcmp(mode, "timeout") ? 50 : 2000);
        if (!ok) { std::fprintf(stderr, "userhelper fixture failed: %s\n", mode); return 1; }
    }
    QEventLoop loop;
    bool called = false;
    auto *owner = new QObject;
    userhelper::runProgram(QCoreApplication::applicationFilePath(), {QStringLiteral("timeout")}, {}, owner,
        [&](const QJsonObject &) { called = true; }, 500);
    delete owner;
    QTimer::singleShot(700, &loop, &QEventLoop::quit); loop.exec();
    return called ? 1 : 0;
}
