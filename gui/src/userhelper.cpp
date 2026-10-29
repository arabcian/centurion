#include "userhelper.h"
#include "privileged.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QTimer>

namespace userhelper {

static QJsonObject failure(const QString &why) { return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), why}}; }

void run(const QJsonObject &request, QObject *context, Callback cb, int timeoutMs) {
    const QString exe = privileged::helperPath(QStringLiteral("tune-profile-helper"));
    if (!QFileInfo(exe).isExecutable()) { cb(failure(QStringLiteral("tune-profile-helper is not installed"))); return; }
    // Not parented to `context`: destroying a running QProcess kills and waits for it.
    auto *p = new QProcess(QCoreApplication::instance());
    QPointer<QObject> alive(context);
    QObject::connect(p, &QProcess::finished, p, [p, alive, cb](int, QProcess::ExitStatus) {
        const QByteArray out = p->readAllStandardOutput().trimmed();
        p->deleteLater();
        if (!alive) return;
        const QJsonObject reply = QJsonDocument::fromJson(out.mid(out.lastIndexOf('\n') + 1)).object();
        cb(reply.isEmpty() ? failure(QStringLiteral("no reply from the helper")) : reply);
    });
    QObject::connect(p, &QProcess::errorOccurred, p, [p, alive, cb](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        p->deleteLater();
        if (alive) cb(failure(QStringLiteral("the helper could not be started")));
    });
    QTimer::singleShot(timeoutMs, p, [p] { if (p->state() != QProcess::NotRunning) p->kill(); });
    p->start(exe, {});
    p->write(QJsonDocument(request).toJson(QJsonDocument::Compact));
    p->closeWriteChannel();
}

} // namespace userhelper
