#include "privileged.h"
#include "operationregistry.h"
#include "systools.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QTimeZone>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <memory>

namespace privileged {

static operations::Registry registry;
static QString describe(const operations::Entry &e) {
    const char *phase = QT_TRANSLATE_NOOP("privileged", "Running");
    switch (e.phase) {
    case operations::Phase::Running: break;
    case operations::Phase::TimedOut: phase = QT_TRANSLATE_NOOP("privileged", "Timed out; process still tracked"); break;
    case operations::Phase::OwnerGone: phase = QT_TRANSLATE_NOOP("privileged", "Owner closed; process still tracked"); break;
    case operations::Phase::Exited:
        switch (e.outcome) {
        case operations::Outcome::Unknown: phase = QT_TRANSLATE_NOOP("privileged", "Exited; outcome unknown"); break;
        case operations::Outcome::ReportedOk: phase = QT_TRANSLATE_NOOP("privileged", "Exited; helper reported OK (not independent verification)"); break;
        case operations::Outcome::ReportedFailure: phase = QT_TRANSLATE_NOOP("privileged", "Exited; helper reported failure"); break;
        case operations::Outcome::NotStarted: phase = QT_TRANSLATE_NOOP("privileged", "Failed to start"); break;
        }
        break;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(e.time.time_since_epoch()).count();
    return QStringLiteral("%1 · #%2 · %3 · %4").arg(QDateTime::fromMSecsSinceEpoch(qint64(ms),QTimeZone::utc()).toString(Qt::ISODateWithMs))
        .arg(qulonglong(e.id)).arg(QString::fromStdString(e.resource), QCoreApplication::translate("privileged", phase));
}
QStringList activeOperations() {
    QStringList out;
    for (const auto &[id, e] : registry.active()) { Q_UNUSED(id); out << describe(e); }
    return out;
}
bool resourceActive(const QString &helperName) {
    for (const auto &item : registry.active()) if (item.second.resource == helperName.toStdString()) return true;
    return false;
}
QStringList operationHistory() {
    QStringList out; for (const auto &e : registry.history()) out << describe(e); return out;
}

static constexpr int EXIT_DISMISSED = 126, EXIT_NOT_AUTHORIZED = 127;

QString helperPath(const QString &relative) {
    return QStringLiteral(CENTURION_HELPER_DIR "/") + relative;
}

QString Result::message() const {
    if (!reached) return error;
    const QString e = json.value(QStringLiteral("error")).toString();
    return e.isEmpty() ? json.value(QStringLiteral("message")).toString() : e;
}

void run(const QString &helper, const QJsonObject &payload, QObject *ctx, Callback cb, int timeoutMs) {
    run(helper, QJsonDocument(payload).toJson(QJsonDocument::Compact), ctx, std::move(cb), timeoutMs);
}

void run(const QString &helper, const QByteArray &payload, QObject *ctx, Callback cb, int timeoutMs) {
    QPointer<QObject> guard(ctx);
    auto fail = [&](const QString &msg) {
        Result r; r.error = msg;
        QTimer::singleShot(0, ctx, [cb, r] { cb(r); });
    };
    // install.sh's PGO training run walks every tab of a real GUI build: it
    // must never reach a root helper (hardware writes during a compile).
    if (qEnvironmentVariableIsSet("CENTURION_PGO_TRAIN")) return fail(QStringLiteral("disabled during the PGO training run"));
    // Fixed locations, like the Rust side: never resolve pkexec through $PATH.
    QString pkexec;
    for (const char *c : {"/usr/bin/pkexec", "/bin/pkexec"})
        if (QFileInfo(QString::fromLatin1(c)).isFile()) { pkexec = QString::fromLatin1(c); break; }
    if (pkexec.isEmpty()) return fail(QCoreApplication::translate("privileged", "pkexec was not found. Install polkit (%1).").arg(systools::installCommand(QStringLiteral("pkexec"))));
    if (!QFileInfo(helper).isFile()) return fail(QCoreApplication::translate("privileged", "helper not found: %1").arg(helper));

    // Not parented to `ctx`: a QProcess destructor kills *and waits* for its
    // child. Once pkexec has exec'd the root helper, our kill() is refused
    // (EPERM), so destroying a still-running QProcess — on timeout, or when
    // the owning tab goes away — blocked the GUI thread until the helper
    // exited on its own. The process now lives until it has really finished
    // and only then deletes itself; `guard` decides whether the callback runs.
    auto *proc = new QProcess(QCoreApplication::instance());
    const auto operation = registry.begin(QFileInfo(helper).fileName().toStdString());
    if (ctx) QObject::connect(ctx, &QObject::destroyed, proc, [operation] {
        registry.transition(operation, operations::Phase::OwnerGone);
    });
    auto *timer = new QTimer(proc);
    timer->setSingleShot(true);
    auto done = std::make_shared<bool>(false);
    auto timedOut = std::make_shared<bool>(false);
    auto oversized = std::make_shared<bool>(false);
    auto stdoutBuf = std::make_shared<QByteArray>();
    auto stderrBuf = std::make_shared<QByteArray>();
    constexpr qsizetype outputLimit = 4 * 1024 * 1024, errorLimit = 4096;
    auto drainOut = [=] {
        const auto chunk = proc->readAllStandardOutput();
        if (*oversized) return; // continue draining even when root ignores kill()
        if (chunk.size() > outputLimit - stdoutBuf->size()) {
            *oversized = true; stdoutBuf->clear(); proc->kill();
        } else stdoutBuf->append(chunk);
    };
    auto drainErr = [=] {
        const auto chunk = proc->readAllStandardError();
        if (stderrBuf->size() < errorLimit) stderrBuf->append(chunk.left(errorLimit - stderrBuf->size()));
    };

    auto finish = [=](const Result &r) {
        if (*done) return;
        *done = true;
        timer->stop();
        if (guard) cb(r);
    };
    auto reap = [proc] {
        if (proc->state() == QProcess::NotRunning) proc->deleteLater();
        // else: deleted from the finished() handler once it really exits.
    };

    QObject::connect(timer, &QTimer::timeout, proc, [=] {
        *timedOut = true;
        registry.transition(operation, operations::Phase::TimedOut);
        proc->kill();  // works while pkexec still waits for authorization
        Result r;
        r.error = QCoreApplication::translate("privileged", "the helper did not finish in time. If an authorization dialog is still open, close it and try again. Otherwise the helper may still be finishing in the background (it stops on its own within a few minutes): check the current values before retrying.");
        finish(r);
        reap();
    });
    QObject::connect(proc, &QProcess::errorOccurred, proc, [=](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        registry.finish(operation, operations::Outcome::NotStarted);
        Result r; r.error = QCoreApplication::translate("privileged", "could not start pkexec: %1").arg(proc->errorString());
        finish(r);
        reap();
    });
    QObject::connect(proc, &QProcess::finished, proc, [=](int code, QProcess::ExitStatus exitStatus) {
        proc->deleteLater();
        if (*done) { registry.finish(operation, operations::Outcome::Unknown); return; }  // timeout: process has actually exited
        drainOut(); drainErr();
        Result r;
        const QByteArray &rawOut = *stdoutBuf;
        const QByteArray out = rawOut.trimmed();
        const QString err = QString::fromUtf8(*stderrBuf).trimmed();
        // The helper prints exactly one JSON line; take the last line in case
        // anything else slipped onto stdout first.
        const QByteArray last = out.mid(out.lastIndexOf('\n') + 1);
        const QJsonDocument doc = QJsonDocument::fromJson(last);
        if (*timedOut || *oversized || exitStatus != QProcess::NormalExit) {
            r.error = QCoreApplication::translate("privileged", "The helper crashed or exceeded the reply size limit; its result is unknown.");
        } else if (doc.isObject() && !doc.object().isEmpty() && code != 0 && doc.object().value(QStringLiteral("ok")).toBool()) {
            r.error = QCoreApplication::translate("privileged", "The helper reported success but exited with code %1; its result is unknown.").arg(code);
        } else if (doc.isObject() && !doc.object().isEmpty()) {
            r.reached = true;
            r.json = doc.object();
            // Version handshake: an older install's helpers send no or another helper_version.
            static bool warned = false;
            const QString hv = r.json.value(QStringLiteral("helper_version")).toString();
            if (!warned && hv != QStringLiteral(CENTURION_VERSION)) {
                warned = true;
                qWarning("centurion: %s reports version '%s', the GUI is %s; reinstall so they match",
                         qPrintable(helper), qPrintable(hv.isEmpty() ? QStringLiteral("unknown") : hv), CENTURION_VERSION);
            }
        } else if (code == EXIT_DISMISSED) {
            r.error = QCoreApplication::translate("privileged", "the authorization request was dismissed.");
        } else if (code == EXIT_NOT_AUTHORIZED) {
            r.error = err.isEmpty() ? QCoreApplication::translate("privileged", "polkit did not authorize this action.")
                                    : QCoreApplication::translate("privileged", "polkit did not authorize this action: %1").arg(err);
            if (!helper.startsWith(QStringLiteral(CENTURION_HELPER_DIR "/")))
                r.error += QStringLiteral("\n\n") + QCoreApplication::translate("privileged", "This helper is outside %1, so the pinned polkit actions do not apply to it.")
                                                        .arg(QStringLiteral(CENTURION_HELPER_DIR));
        } else {
            r.error = err.isEmpty() ? QCoreApplication::translate("privileged", "helper exited with code %1.").arg(code) : err;
        }
        registry.finish(operation, !r.reached ? operations::Outcome::Unknown :
            r.ok() ? operations::Outcome::ReportedOk : operations::Outcome::ReportedFailure);
        finish(r);
    });
    // A runaway helper must not grow the GUI's memory without bound: helpers
    // answer with one short JSON line, so anything past 4 MiB is garbage.
    QObject::connect(proc, &QProcess::readyReadStandardOutput, proc, drainOut);
    QObject::connect(proc, &QProcess::readyReadStandardError, proc, drainErr);

    proc->setProgram(pkexec);
    proc->setArguments({helper});
    proc->setProcessChannelMode(QProcess::SeparateChannels);
    proc->start();
    if (proc->state() == QProcess::NotRunning) return;  // FailedToStart already handled
    proc->write(payload);
    proc->closeWriteChannel();
    timer->start(timeoutMs);
}

} // namespace privileged
