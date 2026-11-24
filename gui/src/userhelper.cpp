#include "userhelper.h"
#include "privileged.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <memory>
#include <utility>

namespace userhelper {

static QJsonObject failure(const QString &why) { return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), why}}; }

void run(const QJsonObject &request, QObject *context, Callback cb, int timeoutMs) {
    runProgram(privileged::helperPath(QStringLiteral("tune-profile-helper")), {}, request, context, std::move(cb), timeoutMs);
}

void runProgram(const QString &exe, const QStringList &args, const QJsonObject &request,
                QObject *context, Callback cb, int timeoutMs) {
    if (!context) return;
    if (!QFileInfo(exe).isExecutable()) { QTimer::singleShot(0, context, [cb] { cb(failure(QStringLiteral("read-only helper is not installed"))); }); return; }
    constexpr qsizetype limit = 4 * 1024 * 1024;
    const QByteArray input = QJsonDocument(request).toJson(QJsonDocument::Compact);
    if (input.size() > limit || timeoutMs <= 0) { QTimer::singleShot(0, context, [cb] { cb(failure(QStringLiteral("invalid helper request"))); }); return; }
    // The process is not owned by the widget, so closing it never waits for I/O.
    auto *p = new QProcess(QCoreApplication::instance());
    auto *timer = new QTimer(p); timer->setSingleShot(true);
    QPointer<QObject> alive(context);
    struct State { QByteArray out; qsizetype stderrBytes = 0; bool reported = false; };
    auto state = std::make_shared<State>();
    const auto finish = [=](const QJsonObject &reply) {
        if (state->reported) return;
        state->reported = true; timer->stop();
        if (alive) cb(reply);
    };
    const auto abort = [=](const QString &why) {
        if (state->reported) return;
        state->reported = true; timer->stop();
        if (p->state() != QProcess::NotRunning) p->kill();
        if (alive) cb(failure(why));
    };
    const auto drain = [=] {
        const auto out = p->readAllStandardOutput();
        const auto err = p->readAllStandardError();
        if (state->reported) return;
        if (out.size() > limit - state->out.size() || err.size() > limit - state->stderrBytes) {
            abort(QStringLiteral("read-only helper exceeded the output limit")); return;
        }
        state->out += out; state->stderrBytes += err.size();
    };
    QObject::connect(p, &QProcess::readyReadStandardOutput, p, drain);
    QObject::connect(p, &QProcess::readyReadStandardError, p, drain);
    QObject::connect(context, &QObject::destroyed, p, [=] {
        state->reported = true; timer->stop();
        if (p->state() != QProcess::NotRunning) p->kill();
    });
    QObject::connect(timer, &QTimer::timeout, p, [=] { abort(QStringLiteral("read-only helper timed out")); });
    QObject::connect(p, &QProcess::finished, p, [=](int code, QProcess::ExitStatus status) {
        drain(); p->deleteLater();
        if (state->reported) return;
        if (status != QProcess::NormalExit) { finish(failure(QStringLiteral("read-only helper crashed"))); return; }
        const auto out = state->out.trimmed();
        const auto reply = QJsonDocument::fromJson(out.mid(out.lastIndexOf('\n') + 1)).object();
        if (!reply.value(QStringLiteral("ok")).isBool()) { finish(failure(QStringLiteral("invalid reply from read-only helper"))); return; }
        if (code != 0 && reply.value(QStringLiteral("ok")).toBool()) { finish(failure(QStringLiteral("read-only helper reported success but exited unsuccessfully"))); return; }
        finish(reply);
    });
    QObject::connect(p, &QProcess::errorOccurred, p, [=](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        p->deleteLater(); finish(failure(QStringLiteral("the read-only helper could not be started")));
    });
    QObject::connect(p, &QProcess::started, p, [=] { if (state->reported) { p->kill(); return; } p->write(input); p->closeWriteChannel(); });
    timer->start(timeoutMs); p->start(exe, args);
}

} // namespace userhelper
