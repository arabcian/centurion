#include "cosearchdialog.h"
#include "curvepage.h"
#include "privileged.h"
#include "scenes.h"
#include "sysinfo.h"
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFile>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QSaveFile>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <limits>

CoSearchDialog::CoSearchDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Ryzen CO Search Assistant (experimental)"));
    resize(710, 480);
    auto *v = new QVBoxLayout(this);
    auto *explainer = new QLabel(tr("Tests ALL-CORE negative offsets in steps of 5. Each candidate gets a short SSE CPU test with idle/load transitions. "
        "On failure, cancellation or completion, the assistant requests a reset to stock (0). "
        "A short pass is NOT proof of long-term stability. A hard freeze may still require rebooting."));
    explainer->setWordWrap(true);
    v->addWidget(explainer);
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Maximum negative offset:")));
    limit_ = new QSpinBox;
    limit_->setRange(5, 25);
    limit_->setSingleStep(5);
    limit_->setValue(15);
    row->addWidget(limit_);
    row->addStretch();
    go_ = new QPushButton(tr("Start conservative search"));
    stop_ = new QPushButton(tr("Stop and reset"));
    stop_->setEnabled(false);
    row->addWidget(go_); row->addWidget(stop_);
    v->addLayout(row);
    status_ = new QLabel(tr("Not started. The current CPU curve must be stock."));
    status_->setWordWrap(true);
    v->addWidget(status_);
    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    v->addWidget(log_, 1);
    guard_ = new QTimer(this);
    guard_->setInterval(1000);
    connect(guard_, &QTimer::timeout, this, [this] {
        if (!running_ || resetting_) return;
        if (scenes::gameSessions() > 0 || scenes::activeScene() != startScene_) {
            note(tr("Scene or game tuning ownership changed; cancelling search.")); cancel(); return;
        }
        if (!applying_ && candidate_ < 0 &&
            CurvePage::currentState("cpu") != QStringLiteral("all-core %1").arg(candidate_)) {
            note(tr("CPU curve was changed externally during this candidate; cancelling.")); cancel(); return;
        }
        const double t = temperature();
        if (!std::isfinite(t) || t >= 92.0) {
            note(tr("Temperature sensor unavailable or at/above 92°C; stopping.")); cancel(); return;
        }
        if (test_ && test_->state() != QProcess::NotRunning && QDateTime::currentSecsSinceEpoch() - testStart_ > 75) {
            note(tr("CPU test exceeded safety timeout; terminating.")); cancel();
        }
    });
    connect(go_, &QPushButton::clicked, this, &CoSearchDialog::start);
    connect(stop_, &QPushButton::clicked, this, &CoSearchDialog::cancel);
}

/// Live AMD MCA threshold counters (machinecheckN/<bank>/<block>/error_count, mode 0444): the kernel
/// reads the bank's MISC MSR on every read, so a corrected error shows here at once, while the kernel
/// itself may only log it at the next MCE poll (check_interval, 5 min by default).
static QHash<QString, QString> mceCounters() {
    QHash<QString, QString> out;
    QDirIterator it(QStringLiteral("/sys/devices/system/machinecheck"), {QStringLiteral("error_count")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext() && out.size() < 4096) {
        const QString p = it.next();
        QFile f(p);
        if (f.open(QIODevice::ReadOnly)) out.insert(p, QString::fromLatin1(f.read(32)).trimmed());
    }
    return out;
}

/// Blocking stock reset, for when the dialog is destroyed mid-search (application quit): the
/// asynchronous privileged::run could not report back any more.
static bool blockingReset() {
    QString pkexec;
    for (const char *c : {"/usr/bin/pkexec", "/bin/pkexec"})
        if (QFileInfo(QString::fromLatin1(c)).isFile()) { pkexec = QString::fromLatin1(c); break; }
    if (pkexec.isEmpty()) return false;
    QProcess p;
    p.start(pkexec, {privileged::helperPath("ryzen-co-helper")});
    if (!p.waitForStarted(3000)) return false;
    p.write(QJsonDocument(QJsonObject{{"op", "reset"}}).toJson(QJsonDocument::Compact) + '\n');
    p.closeWriteChannel();
    if (!p.waitForFinished(15000)) return false;
    const QByteArray out = p.readAllStandardOutput().trimmed();
    return QJsonDocument::fromJson(out.mid(out.lastIndexOf('\n') + 1)).object().value("ok").toBool();
}

CoSearchDialog::~CoSearchDialog() {
    if (!running_ || resetting_) return;  // idle, or the asynchronous reset is already on its way
    guard_->stop();
    if (test_) {
        test_->disconnect(this);
        test_->kill();
        test_->waitForFinished(2000);
    }
    const bool inFlight = applying_;
    bool ok = blockingReset();
    // An SMU write still in flight may land after that reset (ryzen-co-helper serialises on its lock):
    // reset once more after it has had time to finish.
    if (inFlight) { QThread::msleep(1500); ok = blockingReset() && ok; }
    quitting_ = true;
    done(ok, tr("Application closed during search"));
}

void CoSearchDialog::reject() {
    if (running_) { cancel(); return; }
    QDialog::reject();
}

void CoSearchDialog::closeEvent(QCloseEvent *e) {
    if (running_) {
        cancel();
        e->ignore();  // root writes and reset must finish before this controller can be destroyed
    } else QDialog::closeEvent(e);
}

double CoSearchDialog::temperature() const {
    // The same sensor the rest of the GUI shows: k10temp, zenpower (Tctl/Tdie) or coretemp.
    return sysinfo::cpuTempC().value_or(std::numeric_limits<double>::quiet_NaN());
}

void CoSearchDialog::note(const QString &s) {
    log_->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss") + "  " + s);
    status_->setText(s);
}

void CoSearchDialog::start() {
    if (running_) return;
    testProgram_ = QStandardPaths::findExecutable("centurion-cpustress");
    if (testProgram_.isEmpty() || !QFileInfo(privileged::helperPath("ryzen-co-helper")).isExecutable()) {
        QMessageBox::warning(this, tr("Missing tools"), tr("Install centurion-cpustress and ryzen-co-helper first.")); return;
    }
    if (CurvePage::currentState("cpu") != "stock" || scenes::gameSessions() > 0) {
        QMessageBox::warning(this, tr("Search locked"), tr("Reset the CPU curve to stock and end game sessions before searching. Existing offsets must not be overwritten.")); return;
    }
    // This helper's retained state is only *recorded* state, never an SMU read-back. Require
    // a known explicit stock write to avoid silently overwriting a saved CO configuration.
    QFile coState(QStringLiteral("/run/centurion/co-state.json"));
    const QJsonObject co = coState.open(QIODevice::ReadOnly) && coState.size() < (64LL << 10)
        ? QJsonDocument::fromJson(coState.readAll()).object() : QJsonObject{};
    const QJsonValue state = co.value("state");
    const QJsonObject recorded = state.toObject();
    const bool knownStock = state.toString() == QStringLiteral("reset") ||
        (recorded.size() == 1 && recorded.value("all").isDouble() && recorded.value("all").toInt(1) == 0);
    if (!knownStock) {
        QMessageBox::warning(this, tr("Cannot verify stock state"),
           tr("Use Ryzen Curve → Reset Curve Optimizer first. The SMU cannot read offsets back; "
              "the last helper-written state must explicitly be reset/0 before this search.")); return;
    }
    const double t = temperature();
    if (!std::isfinite(t) || t >= 80.0) {
        QMessageBox::warning(this, tr("Thermal guard"), tr("A readable CPU temperature sensor (k10temp, zenpower or coretemp) below 80°C is required before starting.")); return;
    }
    if (QMessageBox::warning(this, tr("Risk of a system crash"),
            tr("Undervolting may freeze or reboot the system. No applications can be protected against a hard lockup. "
               "The assistant resets the CPU curve when it can, but it cannot restore unsaved offsets. Continue?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    running_ = true;
    abort_ = false;
    exhausted_ = false;
    candidate_ = 0;
    best_ = 0;
    startScene_ = scenes::activeScene();
    QFile boot(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    bootId_ = boot.open(QIODevice::ReadOnly) ? QString::fromUtf8(boot.readAll()).trimmed() : QString();
    if (bootId_.isEmpty()) { running_ = false; QMessageBox::warning(this, tr("Boot identity"), tr("Cannot verify boot ID.")); return; }
    attempts_ = {};
    go_->setEnabled(false);
    stop_->setEnabled(true);
    limit_->setEnabled(false);
    guard_->start();
    note(tr("Beginning with stock state; every tested candidate will be cleared afterward."));
    // Baseline: only kernel events logged after this point count against a candidate.
    kmsgSeq_ = 0;
    scanKernel([this](bool ok, const QJsonArray &, quint64 last) {
        if (!running_ || resetting_) return;
        if (!ok) { reset(tr("Cannot read classified kernel events; not starting.")); return; }
        kmsgSeq_ = last;
        if (mceCounters().isEmpty())
            note(tr("MCA threshold counters are not readable here: corrected errors count only once the kernel logs them "
                    "(its MCE poll can take up to 5 minutes), so a short pass is even weaker evidence."));
        next();
    });
}

void CoSearchDialog::next() {
    if (!running_ || resetting_) return;
    if (abort_) { reset(tr("Cancelled")); return; }
    candidate_ -= 5;
    if (-candidate_ > limit_->value()) { exhausted_ = true; reset(tr("All requested candidates tested")); return; }
    if (CurvePage::currentState("cpu") == "stock" && candidate_ != -5) {
        // Unexpected external modification or helper journal reset: fail closed.
        reset(tr("Curve state changed unexpectedly; stopping")); return;
    }
    mceBefore_ = mceCounters();
    applying_ = true;
    note(tr("Applying trial offset %1 via the existing privileged helper…").arg(candidate_));
    privileged::run(privileged::helperPath("ryzen-co-helper"),
        QJsonObject{{"op", "set_coall"}, {"params", QJsonObject{{"value", candidate_}}}}, this,
        [this](const privileged::Result &r) {
            applying_ = false;
            if (!running_) return;
            if (!r.ok()) { reset(tr("SMU write rejected: %1").arg(r.message())); return; }
            if (abort_) { reset(tr("Cancelled after in-flight write")); return; }
            cpuTest();
        }, 120000);
}

void CoSearchDialog::cpuTest() {
    test_ = new QProcess(this);
    testStart_ = QDateTime::currentSecsSinceEpoch();
    note(tr("Running a 20-second all-core SSE test at %1, with transitions.").arg(candidate_));
    connect(test_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (status != QProcess::NormalExit) code = -1;
        testFinished(code);
    });
    connect(test_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) { note(tr("CPU test failed to start.")); cancel(); }
    });
    test_->start(testProgram_, {"allcore", "--load", "sse", "--seconds", "20", "--transitions"});
}

void CoSearchDialog::testFinished(int code) {
    QProcess *p = test_;
    test_ = nullptr;
    if (!p) return;
    const QByteArray output = p->readAllStandardOutput();
    p->deleteLater();
    if (abort_) { reset(tr("Cancelled while testing")); return; }
    QJsonObject end;
    for (const QByteArray &line : output.split('\n')) {
        const auto o = QJsonDocument::fromJson(line).object();
        if (o.value("ev").toString() == "end") end = o;
    }
    const bool computedPass = code == 0 && end.value("status").toString() == "pass" && end.value("errors").toInt(-1) == 0;
    attempts_.append(QJsonObject{{"t", double(testStart_)}, {"coall", candidate_},
                                 {"computed_pass", computedPass}, {"exit_code", code},
                                 {"test", QStringLiteral("SSE all-core 20s with transitions")}});
    if (!computedPass) {
        reset(tr("Offset %1 FAILED / incomplete; %2").arg(candidate_).arg(end.value("msg").toString())); return;
    }
    // Allow the Health kernel-event debounce to flush before interpreting this result.
    QTimer::singleShot(2500, this, [this] {
        if (abort_) { reset(tr("Cancelled after CPU test")); return; }
        validateFaultLog();
    });
}

void CoSearchDialog::scanKernel(std::function<void(bool, const QJsonArray &, quint64)> then) {
    // health-reader (read-only, silent polkit) reads /dev/kmsg directly. health.log is not used: with
    // kernel.dmesg_restrict=1 the Health tab persists events only every 5 minutes, and the file does not
    // exist until the first event, so it can neither prove a clean trial nor be relied on to exist.
    privileged::run(privileged::helperPath("health-reader"), QJsonObject{{"since", double(kmsgSeq_)}}, this,
        [then](const privileged::Result &r) {
            then(r.ok(), r.json.value("events").toArray(), quint64(r.json.value("last_seq").toDouble()));
        }, 30000);
}

void CoSearchDialog::validateFaultLog() {
    scanKernel([this](bool ok, const QJsonArray &events, quint64 last) {
        if (!running_ || resetting_) return;
        if (abort_) { reset(tr("Cancelled after CPU test")); return; }
        // An unreadable kernel log is NOT evidence of a clean trial.
        if (!ok) { reset(tr("Cannot read classified kernel events; stopping without claiming a clean test.")); return; }
        for (const QJsonValue &v : events) {
            const QJsonObject e = v.toObject();
            const QString kind = e.value("kind").toString();
            if (kind == "mce" || kind == "lockup" || kind == "thermal") {
                reset(tr("Hardware fault reported during trial: %1").arg(e.value("title").toString())); return;
            }
        }
        // Corrected errors the kernel has not polled yet: any threshold counter that moved.
        const QHash<QString, QString> now = mceCounters();
        for (auto i = mceBefore_.cbegin(); i != mceBefore_.cend(); ++i) {
            const auto n = now.constFind(i.key());
            if (n != now.cend() && n.value() != i.value()) {
                const QString bank = QFileInfo(i.key()).dir().path().section('/', -3, -2);
                reset(tr("Corrected machine-check errors counted during trial (%1: %2 → %3)").arg(bank, i.value(), n.value()));
                return;
            }
        }
        kmsgSeq_ = std::max(kmsgSeq_, last);
        best_ = candidate_;
        note(tr("Short test passed at %1. This is provisional, not a stable offset recommendation.").arg(best_));
        QTimer::singleShot(0, this, &CoSearchDialog::next);
    });
}

void CoSearchDialog::cancel() {
    if (!running_ || resetting_) return;
    abort_ = true;
    stop_->setEnabled(false);
    if (test_ && test_->state() != QProcess::NotRunning) {
        QPointer<QProcess> pending(test_);
        test_->terminate();
        QTimer::singleShot(3000, this, [pending] {
            if (pending && pending->state() != QProcess::NotRunning) pending->kill();
        });
        return;
    }
    if (applying_) { note(tr("Waiting for in-flight SMU write before resetting.")); return; }
    reset(tr("Cancelled by user / guard"));
}

void CoSearchDialog::reset(const QString &reason) {
    if (!running_ || resetting_) return;
    resetting_ = true;
    note(reason + tr(" — requesting stock reset."));
    privileged::run(privileged::helperPath("ryzen-co-helper"), QJsonObject{{"op", "reset"}}, this,
        [this, reason](const privileged::Result &r) { done(r.ok(), reason + (r.ok() ? QString() : QStringLiteral("; reset FAILED: ") + r.message())); }, 120000);
}

void CoSearchDialog::done(bool resetOk, const QString &reason) {
    guard_->stop();
    // Persist a bounded evidence record. "passed" means only short compute tests, not curve stability.
    const QString dir = (qEnvironmentVariableIsEmpty("XDG_STATE_HOME") ? QDir::homePath() + "/.local/state"
                                 : qEnvironmentVariable("XDG_STATE_HOME")) + "/centurion";
    QDir().mkpath(dir);
    const QString path = dir + "/co-search-runs.json";
    QFile prev(path);
    QJsonArray history;
    if (prev.open(QIODevice::ReadOnly) && prev.size() < (1LL << 20))
        history = QJsonDocument::fromJson(prev.readAll()).object().value("history").toArray();
    history.append(QJsonObject{{"t", double(QDateTime::currentSecsSinceEpoch())}, {"boot", bootId_},
                              {"best_short_test", best_}, {"reset_ok", resetOk},
                              {"complete", exhausted_ && resetOk},
                              {"notes", reason}, {"attempts", attempts_}});
    while (history.size() > 40) history.removeAt(0);
    QSaveFile target(path);
    if (target.open(QIODevice::WriteOnly)) {
        target.write(QJsonDocument(QJsonObject{{"version", 1}, {"history", history}}).toJson());
        target.commit();
    }
    running_ = false;
    resetting_ = false;
    go_->setEnabled(resetOk);
    stop_->setEnabled(false);
    limit_->setEnabled(true);
    note(reason);
    if (resetOk) note(tr("SMU reset confirmed by helper; best short-test candidate was %1. Manually reapply only after extended testing.").arg(best_));
    else if (!quitting_) QMessageBox::critical(this, tr("Reset failed"), tr("CPU curve reset could not be confirmed. Use Ryzen Curve → Reset, or reboot before continuing."));
}
