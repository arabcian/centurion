#include "gpustressbox.h"

#include "curvepage.h"
#include "theme.h"

#include <QComboBox>
#include <QDateTime>
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

// tr()'s plural count is an int; a broken VRAM run can have more wrong words than that.
int plural(qint64 n) { return int(qMin<qint64>(n, INT_MAX)); }

QString findTool(const QString &name) {
    QString p = QStandardPaths::findExecutable(name);
    if (p.isEmpty()) p = QStandardPaths::findExecutable(name, {QStringLiteral("/usr/local/bin"), QStringLiteral("/usr/bin")});
    return p;
}

} // namespace

GpuStressBox::GpuStressBox(QWidget *parent) : QWidget(parent) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(new QLabel(QStringLiteral("<b>%1</b>").arg(tr("GPU test"))));

    program_ = findTool(QStringLiteral("centurion-gpustress"));
    if (program_.isEmpty()) {
        auto *hint = new QLabel(QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::MUTED,
            tr("Checks a GPU curve or memory offset with Vulkan compute work. Needs centurion-gpustress, which is "
               "installed next to centurion-cooltest.").toHtmlEscaped()));
        hint->setWordWrap(true);
        root->addWidget(hint);
        return;
    }

    auto *intro = new QLabel(QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::MUTED,
        tr("Loads the GPU with Vulkan compute work whose correct result is known and counts every wrong result, "
           "and watches for GPU errors in the kernel log meanwhile. Run the VRAM test after changing a memory "
           "clock offset and the Core test after changing a core curve (undervolt / overclock). The screen may "
           "stutter while it runs. A pass is evidence, not proof. Each result is kept with the GPU curve in effect.")
            .toHtmlEscaped()));
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("GPU")));
    device_ = new QComboBox;
    row->addWidget(device_, 1);
    mode_ = new QComboBox;
    mode_->addItem(tr("VRAM"), static_cast<int>(gpustress::Mode::Vram));
    mode_->addItem(tr("Core"), static_cast<int>(gpustress::Mode::Core));
    row->addWidget(mode_);
    percentLabel_ = new QLabel(tr("VRAM to test"));
    row->addWidget(percentLabel_);
    percent_ = new QSpinBox;
    percent_->setRange(gpustress::MIN_PERCENT, gpustress::MAX_PERCENT);
    percent_->setSingleStep(5);
    percent_->setValue(gpustress::DEFAULT_PERCENT);
    percent_->setSuffix(QStringLiteral(" %"));
    row->addWidget(percent_);
    row->addWidget(new QLabel(tr("Duration")));
    minutes_ = new QComboBox;
    for (int m : {5, 10, 15, 30, 60}) minutes_->addItem(tr("%n min", nullptr, m), m);
    minutes_->setCurrentIndex(minutes_->findData(gpustress::DEFAULT_MINUTES));
    row->addWidget(minutes_);
    run_ = new QPushButton(tr("Start"));
    run_->setEnabled(false);  // until a GPU is listed
    connect(run_, &QPushButton::clicked, this, [this] {
        if (!proc_) { start(); return; }
        cancelled_ = true;
        proc_->terminate();  // the tool stops at its next submission and prints "stopped"
    });
    row->addWidget(run_);
    root->addLayout(row);
    connect(mode_, &QComboBox::currentIndexChanged, this, [this] {
        const bool vram = mode_->currentData().toInt() == static_cast<int>(gpustress::Mode::Vram);
        percentLabel_->setVisible(vram);
        percent_->setVisible(vram);
    });

    status_ = new QLabel;
    status_->setWordWrap(true);
    status_->setVisible(false);
    root->addWidget(status_);

    results_ = new QTreeWidget;
    results_->setRootIsDecorated(false);
    results_->setHeaderLabels({tr("Date"), tr("Test"), tr("GPU"), tr("Size"), tr("Duration"), tr("Passes"), tr("Errors"), tr("Result"), tr("GPU curve")});
    results_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    results_->setMaximumHeight(160);
    root->addWidget(results_);

    ticker_ = new QTimer(this);  // only while a test runs
    ticker_->setInterval(1000);
    connect(ticker_, &QTimer::timeout, this, &GpuStressBox::tick);
    fill();
}

void GpuStressBox::note(const QString &text, const char *color) {
    status_->setVisible(true);
    status_->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(QLatin1String(color), text.toHtmlEscaped()));
}

void GpuStressBox::setIdle(bool idle) {
    device_->setEnabled(idle);
    mode_->setEnabled(idle);
    percent_->setEnabled(idle);
    minutes_->setEnabled(idle);
}

void GpuStressBox::refresh(bool blocked) {
    if (!run_ || proc_) return;  // no tool, or a test is running
    if (blocked) {
        blocked_ = true;
        run_->setEnabled(false);
        note(tr("The NVIDIA GPU is off or not initialised, so it is not probed (listing its Vulkan device could wake "
                "it or hang). Switch it on and open this page again."), theme::WARN);
        return;
    }
    if (blocked_) { blocked_ = false; status_->setVisible(false); run_->setEnabled(!devices_.isEmpty()); }
    // Listing initialises the Vulkan drivers, so it happens when the page is shown, never at start-up.
    if (devices_.isEmpty() && !listProc_) loadDevices();
}

void GpuStressBox::loadDevices() {
    listProc_ = new QProcess(this);
    listProc_->setStandardErrorFile(QProcess::nullDevice());
    auto done = [this] {
        if (!listProc_) return;
        devices_ = gpustress::parseDevices(listProc_->readAllStandardOutput());
        listProc_->deleteLater();
        listProc_ = nullptr;
        showDevices();
    };
    connect(listProc_, &QProcess::finished, this, done);
    connect(listProc_, &QProcess::errorOccurred, this, [done](QProcess::ProcessError e) { if (e == QProcess::FailedToStart) done(); });
    listProc_->start(program_, {QStringLiteral("list")});
}

void GpuStressBox::showDevices() {
    device_->clear();
    for (const gpustress::Device &d : std::as_const(devices_))
        device_->addItem(QStringLiteral("%1 (%2)").arg(d.name, d.pci), d.pci);
    device_->setCurrentIndex(gpustress::preselect(devices_));
    run_->setEnabled(!devices_.isEmpty() && !proc_);
    if (devices_.isEmpty())
        note(tr("No Vulkan GPU found. The test needs a Vulkan driver (Mesa, or the NVIDIA driver's Vulkan support)."), theme::WARN);
    else if (!proc_)
        status_->setVisible(false);
}

void GpuStressBox::onFault(const QJsonObject &e) {
    if (proc_ && gpustress::isGpuFault(e.value(QStringLiteral("kind")).toString(), e.value(QStringLiteral("code")).toInt())) ++faults_;
}

void GpuStressBox::start() {
    const auto m = static_cast<gpustress::Mode>(mode_->currentData().toInt());
    const int seconds = minutes_->currentData().toInt() * 60;
    const gpustress::Plan p = gpustress::plan(m, device_->currentData().toString(), percent_->value(), seconds);
    if (!p.error.isEmpty()) {
        note(p.error == QLatin1String("device") ? tr("Choose a GPU to test.") : tr("The test settings are not valid."), theme::WARN);
        return;
    }
    launch(p, m, seconds);
}

void GpuStressBox::launch(const gpustress::Plan &p, gpustress::Mode m, int seconds) {
    runMode_ = m;
    seconds_ = seconds;
    pci_ = device_->currentData().toString();
    deviceName_ = device_->currentText();
    curve_ = CurvePage::currentState(QStringLiteral("gpu"));  // what the result is about: the curve in effect at the start
    run_state_ = {};
    partial_.clear();
    faults_ = 0;
    cancelled_ = false;
    proc_ = new QProcess(this);
    proc_->setStandardErrorFile(QProcess::nullDevice());  // diagnostics only; the verdict is on stdout
    connect(proc_, &QProcess::readyReadStandardOutput, this, [this] {
        partial_ += proc_->readAllStandardOutput();
        qsizetype nl;
        while ((nl = partial_.indexOf('\n')) >= 0) {
            gpustress::feed(run_state_, QString::fromUtf8(partial_.left(nl)));
            partial_.remove(0, nl + 1);
        }
        if (partial_.size() > 64 * 1024) partial_.clear();  // never a line that long
    });
    connect(proc_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) { finished(code, st == QProcess::CrashExit); });
    connect(proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) finished(-1, true);
    });
    proc_->start(program_, p.args);
    elapsed_.start();
    ticker_->start();
    run_->setText(tr("Stop"));
    setIdle(false);
    tick();
}

void GpuStressBox::tick() {
    if (!proc_) return;
    const qint64 s = elapsed_.elapsed() / 1000;
    if (s >= seconds_ + STUCK_GRACE_S) proc_->kill();  // recorded as not finished
    const QString what = runMode_ == gpustress::Mode::Vram ? tr("VRAM") : tr("Core");
    QString text = tr("%1 test on %2: %3 of %4 min, %n pass(es)", nullptr, plural(run_state_.passes))
                       .arg(what, deviceName_).arg(s / 60).arg(seconds_ / 60);
    text += QStringLiteral(" · ") + tr("%n error(s)", nullptr, plural(run_state_.errors));
    if (faults_ > 0) text += QStringLiteral(" · ") + tr("%n GPU error(s) in the kernel log", nullptr, faults_);
    status_->setVisible(true);
    status_->setText(text);
}

void GpuStressBox::finished(int exitCode, bool crashed) {
    if (!proc_ || settling_) return;
    ticker_->stop();
    if (!partial_.isEmpty()) gpustress::feed(run_state_, QString::fromUtf8(partial_));
    // The Xid that the failure caused is logged after the process is gone: keep counting faults a moment
    // (proc_ stays set, so onFault counts), with Start disabled, before the verdict is decided.
    settling_ = true;
    run_->setEnabled(false);
    const qint64 seconds = elapsed_.elapsed() / 1000;
    QTimer::singleShot(SETTLE_MS, this, [this, exitCode, crashed, seconds] { conclude(exitCode, crashed, seconds); });
}

void GpuStressBox::conclude(int exitCode, bool crashed, qint64 seconds) {
    settling_ = false;
    const gpustress::Verdict v = gpustress::verdict(run_state_, exitCode, crashed, cancelled_, faults_);
    const QJsonObject result{
        {QStringLiteral("t"), QDateTime::currentSecsSinceEpoch()}, {QStringLiteral("mode"), gpustress::modeName(runMode_)},
        {QStringLiteral("pci"), pci_}, {QStringLiteral("device"), deviceName_},
        {QStringLiteral("mib"), run_state_.bytes / (1024 * 1024)}, {QStringLiteral("seconds"), seconds},
        {QStringLiteral("passes"), run_state_.passes}, {QStringLiteral("errors"), run_state_.errors},
        {QStringLiteral("faults"), faults_}, {QStringLiteral("verdict"), gpustress::verdictName(v)},
        {QStringLiteral("curve"), curve_}};
    const bool saved = gpustress::append(result);
    proc_->deleteLater();
    proc_ = nullptr;
    run_->setText(tr("Start"));
    setIdle(true);
    run_->setEnabled(!devices_.isEmpty() && !blocked_);

    QString text;
    if (v == gpustress::Verdict::Pass)
        text = QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::OK,
            tr("No errors. A pass is evidence, not proof: the curve may still fail under another load.").toHtmlEscaped());
    else if (v == gpustress::Verdict::Fail)
        text = QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::DANGER,
            (run_state_.deviceLost
                 ? tr("The GPU was lost during the test. The curve or offset in effect is not stable.")
                 : tr("GPU errors: %1 wrong result(s) from the test, %2 GPU error(s) in the kernel log. The curve or offset in effect is not stable.")
                       .arg(run_state_.errors).arg(faults_)).toHtmlEscaped());
    else {
        text = tr("Not finished: the test was stopped or ended early (exit code %1).").arg(exitCode).toHtmlEscaped();
        if (!run_state_.msg.isEmpty() && run_state_.status == QLatin1String("init_failed")) text += QStringLiteral("<br>") + run_state_.msg.toHtmlEscaped();
    }
    if (!saved) text += QStringLiteral("<br>") + tr("The result could not be saved to %1.").arg(gpustress::stateFile()).toHtmlEscaped();
    status_->setVisible(true);
    status_->setText(text);
    if (v == gpustress::Verdict::Fail)
        Q_EMIT alert(tr("GPU test failed"), tr("%n error(s) during the GPU test.", nullptr, plural(run_state_.errors + faults_ + (run_state_.deviceLost ? 1 : 0))));
    fill();
}

void GpuStressBox::fill() {
    results_->clear();
    const QJsonArray all = gpustress::load();
    for (qsizetype i = all.size() - 1; i >= 0; --i) {
        const QJsonObject o = all.at(i).toObject();
        const QString v = o.value(QStringLiteral("verdict")).toString();
        const QString shown = v == QLatin1String("pass") ? tr("pass") : v == QLatin1String("fail") ? tr("fail") : tr("incomplete");
        const bool vram = o.value(QStringLiteral("mode")).toString() == QLatin1String("vram");
        const int faults = o.value(QStringLiteral("faults")).toInt();
        QString errors = QString::number(o.value(QStringLiteral("errors")).toInteger());
        if (faults > 0) errors += QStringLiteral(" + ") + tr("%n kernel", nullptr, faults);
        auto *it = new QTreeWidgetItem({
            QDateTime::fromSecsSinceEpoch(o.value(QStringLiteral("t")).toInteger()).toString(QStringLiteral("yyyy-MM-dd HH:mm")),
            vram ? tr("VRAM") : tr("Core"), o.value(QStringLiteral("device")).toString(),
            vram ? tr("%1 MiB").arg(o.value(QStringLiteral("mib")).toInteger()) : QStringLiteral("-"),
            tr("%n min", nullptr, int(o.value(QStringLiteral("seconds")).toInteger() / 60)),
            QString::number(o.value(QStringLiteral("passes")).toInteger()), errors, shown, o.value(QStringLiteral("curve")).toString()});
        const char *color = v == QLatin1String("pass") ? theme::OK : v == QLatin1String("fail") ? theme::DANGER : theme::MUTED;
        it->setForeground(7, QColor(QLatin1String(color)));
        results_->addTopLevelItem(it);
    }
    results_->setVisible(!all.isEmpty());
}
