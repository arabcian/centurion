#include "cpustressbox.h"

#include "curvepage.h"
#include "optimizetab.h"
#include "privileged.h"
#include "ryzentab.h"
#include "sysinfo.h"
#include "theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <climits>

namespace {

constexpr int STUCK_GRACE_S = 120;  // the tool ends itself; one still running this long after is killed
constexpr int SETTLE_MS = 3000;     // kernel events reach onFault ~1.5 s after they are logged (HealthTab debounce): wait for them
const char *const CO_PRESET = "CO validation";

int plural(qint64 n) { return int(qMin<qint64>(n, INT_MAX)); }

QString findTool(const QString &name) {
    QString p = QStandardPaths::findExecutable(name);
    if (p.isEmpty()) p = QStandardPaths::findExecutable(name, {QStringLiteral("/usr/local/bin"), QStringLiteral("/usr/bin")});
    return p;
}

QString bootId() {
    QFile f(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    return f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.readAll()).trimmed() : QString();
}

/// The Curve Optimizer profile file behind a "profile:NAME" curve state, or "" (Intel undervolt, all-core, stock).
QString profileFile(const QString &curve) {
    if (!sysinfo::isAmd() || !curve.startsWith(QLatin1String("profile:"))) return {};
    return ryzen::profilesDir() + QLatin1Char('/') + curve.mid(8) + QStringLiteral(".json");
}

} // namespace

CpuStressBox::CpuStressBox(QWidget *parent) : QWidget(parent) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(new QLabel(QStringLiteral("<b>%1</b>").arg(tr("CPU test"))));

    program_ = findTool(QStringLiteral("centurion-cpustress"));
    if (program_.isEmpty()) {
        auto *hint = new QLabel(QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::MUTED,
            tr("Checks a Curve Optimizer offset or an undervolt core by core. Needs centurion-cpustress, which is "
               "installed next to centurion-cooltest.").toHtmlEscaped()));
        hint->setWordWrap(true);
        root->addWidget(hint);
        return;
    }
    QProcess infoProc;
    infoProc.start(program_, {QStringLiteral("info")});
    if (infoProc.waitForFinished(3000)) info_ = cpustress::parseInfo(infoProc.readAllStandardOutput());

    auto *intro = new QLabel(QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::MUTED,
        tr("Runs work whose correct result is known and compares every result, and watches for machine checks and "
           "lockups in the kernel log meanwhile. Per core: one thread visits every core in turn at its highest boost, "
           "where a Curve Optimizer or undervolt offset fails first; use it after per-core offsets. All cores: every "
           "CPU at once; use it after an all-core offset. SSE keeps the clock highest; AVX2 / AVX-512 add heat and "
           "current. An unstable offset can also freeze or reset the machine: the core that was loaded is then shown "
           "here after the reboot. A pass is evidence, not proof. Each result is kept with the CPU curve in effect.")
            .toHtmlEscaped()));
    intro->setWordWrap(true);
    root->addWidget(intro);
    klogNote_ = new QLabel(QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::MUTED,
        tr("The kernel log is not readable without a password here, so machine checks are not counted during the test; "
           "only the test's own results are.").toHtmlEscaped()));
    klogNote_->setWordWrap(true);
    klogNote_->setVisible(!kernelLive_);
    root->addWidget(klogNote_);

    auto *row = new QHBoxLayout;
    mode_ = new QComboBox;
    mode_->addItem(tr("Per core"), static_cast<int>(cpustress::Mode::PerCore));
    mode_->addItem(tr("All cores"), static_cast<int>(cpustress::Mode::AllCore));
    row->addWidget(mode_);
    load_ = new QComboBox;
    if (info_.loads.contains(QLatin1String("sse"))) load_->addItem(tr("SSE (highest boost)"), QStringLiteral("sse"));
    if (info_.loads.contains(QLatin1String("avx2"))) load_->addItem(QStringLiteral("AVX2"), QStringLiteral("avx2"));
    if (info_.loads.contains(QLatin1String("avx512"))) load_->addItem(QStringLiteral("AVX-512"), QStringLiteral("avx512"));
    row->addWidget(load_);
    durationLabel_ = new QLabel;
    row->addWidget(durationLabel_);
    duration_ = new QComboBox;
    row->addWidget(duration_);
    cyclesLabel_ = new QLabel(tr("Rounds"));
    row->addWidget(cyclesLabel_);
    cycles_ = new QSpinBox;
    cycles_->setRange(1, cpustress::MAX_CYCLES);
    cycles_->setToolTip(tr("How many times every core is visited."));
    row->addWidget(cycles_);
    run_ = new QPushButton(tr("Start"));
    run_->setEnabled(!info_.cores.isEmpty() && load_->count() > 0);
    connect(run_, &QPushButton::clicked, this, [this] {
        if (!proc_) { start(); return; }
        cancelled_ = true;
        proc_->terminate();  // the tool stops after its current chunk and prints "stopped"
    });
    row->addWidget(run_);
    row->addStretch(1);
    root->addLayout(row);
    connect(mode_, &QComboBox::currentIndexChanged, this, &CpuStressBox::fillDurations);
    fillDurations();

    auto *row2 = new QHBoxLayout;
    transitions_ = new QCheckBox(tr("Idle ↔ load transitions"));
    transitions_->setChecked(true);
    transitions_->setToolTip(tr("The last third of each core's time (all cores: of the run) alternates short bursts with pauses of\n"
                                "1-200 ms, so the core keeps leaving idle states and boosting again. Curve Optimizer offsets often\n"
                                "fail there although a steady load passes."));
    row2->addWidget(transitions_);
    if (sysinfo::isAmd() && !OptimizeTab::builtinValues(QLatin1String(CO_PRESET)).isEmpty()) {
        preset_ = new QCheckBox(tr("Hold the \"CO validation\" preset during the test"));
        preset_->setChecked(true);
        preset_->setToolTip(tr("Optimizations preset: boost on, every idle state on, machine checks polled every 10 s.\n"
                               "Applied when the test starts and taken back when it ends, returning the tuning that was in\n"
                               "effect before (it is held like a game session, so it also comes back if Centurion quits).\n"
                               "Asks for the administrator password; not applied while a game holds the tuning."));
        row2->addWidget(preset_);
    }
    row2->addStretch(1);
    root->addLayout(row2);

    status_ = new QLabel;
    status_->setWordWrap(true);
    status_->setVisible(false);
    root->addWidget(status_);
    if (info_.cores.isEmpty()) note(tr("centurion-cpustress could not read the CPU topology."), theme::WARN);

    results_ = new QTreeWidget;
    results_->setRootIsDecorated(false);
    results_->setHeaderLabels({tr("Date"), tr("Test"), tr("Duration"), tr("Errors"), tr("Result"), tr("Failed"), tr("CPU curve")});
    results_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    results_->setMaximumHeight(160);
    root->addWidget(results_);

    ticker_ = new QTimer(this);  // only while a test runs
    ticker_->setInterval(1000);
    connect(ticker_, &QTimer::timeout, this, &CpuStressBox::tick);

    // A run of an earlier session that never ended: the machine went down under it (or Centurion did).
    const QJsonArray crashed = cpustress::settle(bootId());
    if (!crashed.isEmpty()) {
        const QJsonObject c = crashed.last().toObject();
        const int cpu = c.value(QStringLiteral("cpu")).toInt(-1);
        const QString where = cpu >= 0 ? tr("while %1 was loaded").arg(cpuName(cpu)) : tr("during the all-core test");
        const QString msg = tr("The machine went down %1 (CPU curve: %2). Back that offset off before trusting it.")
                                .arg(where, c.value(QStringLiteral("curve")).toString());
        note(msg, theme::DANGER);
        QTimer::singleShot(0, this, [this, msg] { Q_EMIT alert(tr("CPU test: the machine went down"), msg); });
    }
    fill();
}

void CpuStressBox::fillDurations() {
    const bool perCore = mode_->currentData().toInt() == static_cast<int>(cpustress::Mode::PerCore);
    durationLabel_->setText(perCore ? tr("Per core") : tr("Duration"));
    duration_->clear();
    for (int m : perCore ? QList<int>{1, 3, 6, 10} : QList<int>{5, 10, 15, 30, 60}) duration_->addItem(tr("%n min", nullptr, m), m * 60);
    duration_->setCurrentIndex(1);  // 3 min a core / 10 min
    cyclesLabel_->setVisible(perCore);
    cycles_->setVisible(perCore);
}

void CpuStressBox::note(const QString &text, const char *color) {
    status_->setVisible(true);
    status_->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(QLatin1String(color), text.toHtmlEscaped()));
}

void CpuStressBox::setIdle(bool idle) {
    for (QWidget *w : {static_cast<QWidget *>(mode_), static_cast<QWidget *>(load_), static_cast<QWidget *>(duration_),
                       static_cast<QWidget *>(cycles_), static_cast<QWidget *>(transitions_)})
        w->setEnabled(idle);
    if (preset_) preset_->setEnabled(idle);
}

void CpuStressBox::setKernelLog(bool live) {
    kernelLive_ = live;
    if (klogNote_) klogNote_->setVisible(!live);
}

void CpuStressBox::onFault(const QJsonObject &e) {
    if (!proc_ || !cpustress::isCpuFault(e.value(QStringLiteral("kind")).toString())) return;
    ++faults_;
    const int cpu = cpustress::eventCpu(e.value(QStringLiteral("text")).toString());
    if (cpu >= 0 && !faultCpus_.contains(cpu)) faultCpus_.append(cpu);
}

QString CpuStressBox::cpuName(int cpu) const {
    // On AMD: the CCD and the Curve Optimizer slot of the Ryzen tab (the n-th core of its CCD).
    if (sysinfo::isAmd()) {
        const ryzen::Layout l = ryzen::detect();
        for (auto it = l.cores.cbegin(); it != l.cores.cend(); ++it)
            for (qsizetype i = 0; i < it.value().size(); ++i)
                if (it.value().at(i).cpus.contains(cpu)) return tr("CPU %1 (CCD%2 slot %3)").arg(cpu).arg(it.key()).arg(i);
    }
    return tr("CPU %1").arg(cpu);
}

QString CpuStressBox::cpuList(const QList<int> &cpus) const {
    QStringList out;
    for (int c : cpus) out << cpuName(c);
    return out.join(QStringLiteral(", "));
}

int CpuStressBox::totalSeconds() const {
    return runMode_ == cpustress::Mode::PerCore ? seconds_ * int(info_.cores.size()) * cycles_n_ : seconds_;
}

void CpuStressBox::start() {
    runMode_ = static_cast<cpustress::Mode>(mode_->currentData().toInt());
    seconds_ = duration_->currentData().toInt();
    cycles_n_ = runMode_ == cpustress::Mode::PerCore ? cycles_->value() : 1;
    load_name_ = load_->currentData().toString();
    plan_ = cpustress::plan(runMode_, load_name_, seconds_, transitions_->isChecked(), cycles_n_);
    if (!plan_.error.isEmpty()) { note(tr("The test settings are not valid."), theme::WARN); return; }
    if (!preset_ || !preset_->isChecked()) { launch(); return; }

    // The preset first, as a game session owned by this process; the test starts once it is in effect.
    starting_ = true;
    run_->setEnabled(false);
    setIdle(false);
    note(tr("Applying the \"CO validation\" preset…"), theme::MUTED);
    const QJsonObject req{{"op", "apply"}, {"mode", "game"}, {"preset", QLatin1String(CO_PRESET)},
                          {"values", OptimizeTab::builtinValues(QLatin1String(CO_PRESET))},
                          {"owner_pid", QCoreApplication::applicationPid()}};
    privileged::run(privileged::helperPath(QStringLiteral("tune-helper")), req, this, [this](const privileged::Result &r) {
        starting_ = false;
        if (!r.reached) {
            note(tr("The preset was not applied (%1); the test did not start.").arg(r.message()), theme::WARN);
            setIdle(true);
            run_->setEnabled(true);
            return;
        }
        // applied:false with ok:true = a game already holds the tuning; our session was still opened.
        presetHeld_ = r.json.value(QStringLiteral("ok")).toBool() || r.json.value(QStringLiteral("applied")).toBool();
        launch();
        if (!r.json.value(QStringLiteral("applied")).toBool())
            note(tr("A game holds the tuning, so the \"CO validation\" preset is not in effect for this test."), theme::WARN);
        else if (!r.ok())
            note(tr("Some preset values could not be applied: %1").arg(r.message()), theme::WARN);
    });
}

QJsonObject CpuStressBox::record(const QString &verdict) const {
    QJsonObject o{{"id", id_}, {"t", id_}, {"boot", bootId()}, {"verdict", verdict},
                  {"mode", cpustress::modeName(runMode_)}, {"load", load_name_}, {"seconds", seconds_}, {"cycles", cycles_n_},
                  {"transitions", plan_.args.contains(QStringLiteral("--transitions"))}, {"preset", presetHeld_},
                  {"vendor", sysinfo::isAmd() ? "amd" : sysinfo::isIntel() ? "intel" : "other"},
                  {"curve", curve_}, {"cpu", run_state_.cpu}};
    if (!profileHash_.isEmpty()) o[QStringLiteral("profile_hash")] = profileHash_;
    return o;
}

void CpuStressBox::launch() {
    id_ = QDateTime::currentSecsSinceEpoch();
    curve_ = CurvePage::currentState(QStringLiteral("cpu"));  // what the result is about: the curve in effect at the start
    profileHash_.clear();
    if (const QString pf = profileFile(curve_); !pf.isEmpty()) {
        QFile f(pf);
        if (f.open(QIODevice::ReadOnly) && f.size() < 256 * 1024) profileHash_ = cpustress::profileHash(f.readAll());
    }
    run_state_ = {};
    partial_.clear();
    faults_ = 0;
    faultCpus_.clear();
    cancelled_ = false;
    cpustress::store(record(QStringLiteral("running")));  // left behind if the machine goes down
    proc_ = new QProcess(this);
    proc_->setStandardErrorFile(QProcess::nullDevice());  // diagnostics only; the verdict is on stdout
    connect(proc_, &QProcess::readyReadStandardOutput, this, [this] {
        partial_ += proc_->readAllStandardOutput();
        qsizetype nl;
        while ((nl = partial_.indexOf('\n')) >= 0) {
            if (cpustress::feed(run_state_, QString::fromUtf8(partial_.left(nl)))) cpustress::store(record(QStringLiteral("running")));
            partial_.remove(0, nl + 1);
        }
        if (partial_.size() > 64 * 1024) partial_.clear();  // never a line that long
    });
    connect(proc_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) { finished(code, st == QProcess::CrashExit); });
    connect(proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) finished(-1, true);
    });
    proc_->start(program_, plan_.args);
    elapsed_.start();
    ticker_->start();
    run_->setText(tr("Stop"));
    run_->setEnabled(true);
    setIdle(false);
    tick();
}

void CpuStressBox::tick() {
    if (!proc_) return;
    const qint64 s = elapsed_.elapsed() / 1000;
    if (s >= totalSeconds() + STUCK_GRACE_S) proc_->kill();  // recorded as not finished
    QString text = runMode_ == cpustress::Mode::PerCore
        ? tr("Per-core test: %1 of about %2 min, %3 of %4 cores done").arg(s / 60).arg(totalSeconds() / 60)
              .arg(run_state_.done.size()).arg(info_.cores.size() * cycles_n_)
        : tr("All-core test: %1 of %2 min").arg(s / 60).arg(totalSeconds() / 60);
    if (runMode_ == cpustress::Mode::PerCore && run_state_.cpu >= 0) text += QStringLiteral(" · ") + tr("now on %1").arg(cpuName(run_state_.cpu));
    text += QStringLiteral(" · ") + tr("%n error(s)", nullptr, plural(run_state_.errors));
    if (faults_ > 0) text += QStringLiteral(" · ") + tr("%n machine check(s) / lockup(s)", nullptr, faults_);
    status_->setVisible(true);
    status_->setText(text);
}

void CpuStressBox::finished(int exitCode, bool crashed) {
    if (!proc_ || settling_) return;
    ticker_->stop();
    if (!partial_.isEmpty()) cpustress::feed(run_state_, QString::fromUtf8(partial_));
    // The machine check that a failure caused may be logged after the process is gone: keep counting a moment
    // (proc_ stays set, so onFault counts), with Start disabled, before the verdict is decided.
    settling_ = true;
    run_->setEnabled(false);
    const qint64 seconds = elapsed_.elapsed() / 1000;
    QTimer::singleShot(SETTLE_MS, this, [this, exitCode, crashed, seconds] { conclude(exitCode, crashed, seconds); });
}

void CpuStressBox::releasePreset() {
    if (!presetHeld_) return;
    presetHeld_ = false;
    privileged::run(privileged::helperPath(QStringLiteral("tune-helper")),
                    QJsonObject{{"op", "release"}, {"owner_pid", QCoreApplication::applicationPid()}}, this,
                    [this](const privileged::Result &r) {
                        if (!r.ok()) note(tr("The tuning from before the test could not be restored: %1. Use Optimizations → Restore originals.")
                                              .arg(r.message()), theme::WARN);
                    });
}

void CpuStressBox::conclude(int exitCode, bool crashed, qint64 seconds) {
    settling_ = false;
    const cpustress::Verdict v = cpustress::verdict(run_state_, exitCode, crashed, cancelled_, faults_);
    const bool heldPreset = presetHeld_;
    QJsonObject result = record(cpustress::verdictName(v));
    result[QStringLiteral("seconds")] = seconds;
    result[QStringLiteral("preset")] = heldPreset;
    result[QStringLiteral("chunks")] = run_state_.chunks;
    result[QStringLiteral("errors")] = run_state_.errors;
    result[QStringLiteral("faults")] = faults_;
    QJsonArray failed;
    for (int c : run_state_.failed) failed.append(c);
    result[QStringLiteral("failed")] = failed;
    QJsonArray fc;
    for (int c : std::as_const(faultCpus_)) fc.append(c);
    result[QStringLiteral("fault_cpus")] = fc;
    result.remove(QStringLiteral("cpu"));
    if (!kernelLive_) result[QStringLiteral("kernel_log")] = false;  // a pass without kernel events is weaker evidence
    const bool saved = cpustress::store(result);
    proc_->deleteLater();
    proc_ = nullptr;
    releasePreset();
    run_->setText(tr("Start"));
    setIdle(true);
    run_->setEnabled(true);

    QString text;
    if (v == cpustress::Verdict::Pass) {
        text = QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::OK,
            tr("No errors. A pass is evidence, not proof: the offset may still fail under another load or at idle.").toHtmlEscaped());
    } else if (v == cpustress::Verdict::Fail) {
        QString body = tr("%1 wrong result(s), %2 machine check(s) / lockup(s). The CPU curve in effect is not stable.")
                           .arg(run_state_.errors).arg(faults_);
        if (!run_state_.failed.isEmpty()) body += QLatin1Char(' ') + tr("Wrong results on: %1.").arg(cpuList(run_state_.failed));
        if (!faultCpus_.isEmpty()) body += QLatin1Char(' ') + tr("Kernel errors named: %1.").arg(cpuList(faultCpus_));
        if (sysinfo::isAmd()) body += QLatin1Char(' ') + tr("Make the offset of those slots less negative (for example by 5) in Ryzen Curve and test again.");
        text = QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::DANGER, body.toHtmlEscaped());
    } else {
        text = tr("Not finished: the test was stopped or ended early (exit code %1).").arg(exitCode).toHtmlEscaped();
        if (!run_state_.msg.isEmpty() && run_state_.status == QLatin1String("init_failed")) text += QStringLiteral("<br>") + run_state_.msg.toHtmlEscaped();
    }
    if (!saved) text += QStringLiteral("<br>") + tr("The result could not be saved to %1.").arg(cpustress::stateFile()).toHtmlEscaped();
    status_->setVisible(true);
    status_->setText(text);
    if (v == cpustress::Verdict::Fail)
        Q_EMIT alert(tr("CPU test failed"), tr("%n error(s) during the CPU test.", nullptr, plural(run_state_.errors + faults_)));
    fill();
}

void CpuStressBox::fill() {
    results_->clear();
    const QJsonArray all = cpustress::load();
    for (qsizetype i = all.size() - 1; i >= 0; --i) {
        const QJsonObject o = all.at(i).toObject();
        const QString v = o.value(QStringLiteral("verdict")).toString();
        const QString shown = v == QLatin1String("pass") ? tr("pass") : v == QLatin1String("fail") ? tr("fail")
                            : v == QLatin1String("crashed") ? tr("machine went down") : v == QLatin1String("running") ? tr("running")
                            : tr("incomplete");
        const bool perCore = o.value(QStringLiteral("mode")).toString() == QLatin1String("percore");
        QString test = (perCore ? tr("Per core") : tr("All cores")) + QStringLiteral(" · ") + o.value(QStringLiteral("load")).toString().toUpper();
        if (o.value(QStringLiteral("transitions")).toBool()) test += QStringLiteral(" · ") + tr("transitions");
        const int faults = o.value(QStringLiteral("faults")).toInt();
        QString errors = QString::number(o.value(QStringLiteral("errors")).toInteger());
        if (faults > 0) errors += QStringLiteral(" + ") + tr("%n kernel", nullptr, faults);
        QList<int> bad;
        for (const char *k : {"failed", "fault_cpus"})
            for (const QJsonValue &c : o.value(QLatin1String(k)).toArray()) if (!bad.contains(c.toInt())) bad.append(c.toInt());
        if (v == QLatin1String("crashed") && o.value(QStringLiteral("cpu")).toInt(-1) >= 0) bad.append(o.value(QStringLiteral("cpu")).toInt());
        QStringList badNames;
        for (int c : bad) badNames << QString::number(c);
        auto *it = new QTreeWidgetItem({
            QDateTime::fromSecsSinceEpoch(o.value(QStringLiteral("t")).toInteger()).toString(QStringLiteral("yyyy-MM-dd HH:mm")),
            test, v == QLatin1String("crashed") ? QStringLiteral("-") : tr("%n min", nullptr, int(o.value(QStringLiteral("seconds")).toInteger() / 60)),
            v == QLatin1String("crashed") ? QStringLiteral("-") : errors, shown,
            badNames.isEmpty() ? QStringLiteral("-") : tr("CPU %1").arg(badNames.join(QStringLiteral(", "))),
            o.value(QStringLiteral("curve")).toString()});
        if (!bad.isEmpty()) it->setToolTip(5, cpuList(bad));
        const char *color = v == QLatin1String("pass") ? theme::OK : (v == QLatin1String("fail") || v == QLatin1String("crashed")) ? theme::DANGER : theme::MUTED;
        it->setForeground(4, QColor(QLatin1String(color)));
        results_->addTopLevelItem(it);
    }
    results_->setVisible(!all.isEmpty());
}
