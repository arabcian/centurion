#include "sysadmin.h"

#include "privileged.h"

#include <QCoreApplication>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

namespace sysadmin {

namespace {

QString tr(const char *s) { return QCoreApplication::translate("sysadmin", s); }

/// PipeWire, its Pulse server and the session manager, restarted the way this session started them:
/// gentoo-pipewire-launcher (OpenRC desktops), else the systemd user units.
void restartPipewire(QObject *ctx, Done cb) {
    QString program;
    QStringList args;
    if (const QString l = QStandardPaths::findExecutable(QStringLiteral("gentoo-pipewire-launcher")); !l.isEmpty()) {
        // It forks the servers and stays in the foreground while they run: started detached.
        const bool started = QProcess::startDetached(l, {QStringLiteral("restart")});
        QTimer::singleShot(0, ctx, [cb, started] {
            cb(started, started ? tr("PipeWire restarted (gentoo-pipewire-launcher restart).") : tr("gentoo-pipewire-launcher could not be started."));
        });
        return;
    }
    program = QStandardPaths::findExecutable(QStringLiteral("systemctl"));
    if (program.isEmpty()) {
        QTimer::singleShot(0, ctx, [cb] { cb(false, tr("Neither gentoo-pipewire-launcher nor systemctl was found: restart the sound servers with the session.")); });
        return;
    }
    args = {QStringLiteral("--user"), QStringLiteral("restart"), QStringLiteral("pipewire"), QStringLiteral("pipewire-pulse"), QStringLiteral("wireplumber")};
    auto *p = new QProcess(ctx);
    p->setProcessChannelMode(QProcess::MergedChannels);
    QObject::connect(p, &QProcess::finished, ctx, [p, cb](int code, QProcess::ExitStatus st) {
        const bool ok = st == QProcess::NormalExit && code == 0;
        cb(ok, ok ? tr("PipeWire, pipewire-pulse and WirePlumber restarted.") : QString::fromUtf8(p->readAll()).trimmed());
        p->deleteLater();
    });
    p->start(program, args);
}

}  // namespace

void run(const QJsonObject &req, QObject *ctx, Done cb, int timeoutMs) {
    if (req.value(QStringLiteral("op")).toString() == QLatin1String("pipewire_restart")) { restartPipewire(ctx, std::move(cb)); return; }
    privileged::run(privileged::helperPath(QStringLiteral("sysadmin-helper")), req, ctx,
                    [cb](const privileged::Result &r) { cb(r.ok(), r.message()); }, timeoutMs);
}

void act(QWidget *parent, const QString &title, const QString &confirm, const QJsonObject &req, std::function<void(bool)> after) {
    if (!confirm.isEmpty() &&
        QMessageBox::question(parent, title, confirm, QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    QPointer<QWidget> guard(parent);
    // fstrim walks every filesystem: give it the helper's own limit.
    const int timeout = req.value(QStringLiteral("op")).toString() == QLatin1String("fstrim") ? 960000 : 120000;
    run(req, parent, [guard, title, after](bool ok, const QString &msg) {
        if (!guard) return;
        const QString text = msg.isEmpty() ? (ok ? tr("Done.") : tr("Failed.")) : msg;
        QMessageBox box(ok ? QMessageBox::Information : QMessageBox::Warning, title, text.left(600), QMessageBox::Ok, guard);
        if (text.size() > 600) box.setDetailedText(text);
        box.exec();
        if (after) after(ok);
    }, timeout);
}

}  // namespace sysadmin
