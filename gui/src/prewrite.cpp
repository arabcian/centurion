#include "prewrite.h"

#include "notify.h"
#include "privileged.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

namespace {

constexpr int COPY_TIMEOUT_MS = 120000;

QString settingsPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/centurion/gui.ini");
}

// The write goes on whatever happens here; this only tells the user that the copy is missing.
void warn(const QString &text) {
    static auto *notifier = new Notifier(QCoreApplication::instance());
    notifier->notify(QStringLiteral("prewrite"), QCoreApplication::translate("prewrite", "Safety copy not saved"), text, {}, 15000, [text] {
        auto *box = new QMessageBox(QMessageBox::Warning, QCoreApplication::translate("prewrite", "Safety copy not saved"), text);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->show();  // not exec(): the firmware write must not wait for this box
    });
}

} // namespace

namespace prewrite {

void backupThen(const QString &what, QObject *context, std::function<void()> then) {
    const QPointer<QObject> alive(context);
    auto done = [alive, then] { if (alive) then(); };

    // The same folder and profile setting as Health → Backup (its defaults when the page was never opened).
    QSettings st(settingsPath(), QSettings::IniFormat);
    st.beginGroup(QStringLiteral("backup"));
    const QString dir = st.value(QStringLiteral("config_dir"), QDir::homePath() + QStringLiteral("/Backups/centurion")).toString().trimmed();
    const bool profile = st.value(QStringLiteral("config_profile"), true).toBool();
    st.endGroup();
    if (!QDir::isAbsolutePath(dir)) {
        warn(QCoreApplication::translate("prewrite", "The folder for configuration backups is not set (Health → Backup)."));
        done();
        return;
    }

    const QJsonObject req{{QStringLiteral("op"), QStringLiteral("config_backup")}, {QStringLiteral("dest_dir"), dir},
                          {QStringLiteral("include_profile"), profile}, {QStringLiteral("include_logs"), false},
                          {QStringLiteral("tag"), QStringLiteral("pre-") + what},
                          {QStringLiteral("centurion_version"), QStringLiteral(CENTURION_VERSION)}};

    // Owned by the application, not by the caller: it must finish even if the dialog that asked is closed.
    auto *p = new QProcess(QCoreApplication::instance());
    QObject::connect(p, &QProcess::finished, p, [p, done](int, QProcess::ExitStatus) {
        QJsonObject reply;
        for (const QByteArray &line : p->readAllStandardOutput().split('\n')) {
            const QJsonObject o = QJsonDocument::fromJson(line.trimmed()).object();
            if (o.contains(QLatin1String("ok"))) reply = o;  // progress lines carry no "ok"
        }
        p->deleteLater();
        const QString error = reply.value(QStringLiteral("error")).toString();
        // A fresh install has no configuration yet: nothing to copy is not a problem.
        if (!reply.value(QStringLiteral("ok")).toBool() && error != QLatin1String("nothing to back up yet"))
            warn(QCoreApplication::translate("prewrite", "The configuration was not copied first (%1). The change goes ahead anyway.")
                     .arg(error.isEmpty() ? QCoreApplication::translate("prewrite", "no reply from backup-helper") : error));
        done();
    });
    QObject::connect(p, &QProcess::errorOccurred, p, [p, done](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        p->deleteLater();
        warn(QCoreApplication::translate("prewrite", "backup-helper did not start. The change goes ahead anyway."));
        done();
    });
    QTimer::singleShot(COPY_TIMEOUT_MS, p, [p] { if (p->state() != QProcess::NotRunning) p->kill(); });

    p->start(privileged::helperPath(QStringLiteral("backup-helper")), {});
    // One request line. Stdin stays open until the copy is done: the helper cancels when it sees EOF.
    p->write(QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n');
}

} // namespace prewrite
