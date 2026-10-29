#include "memtestbox.h"

#include "systools.h"
#include "theme.h"
#include "userhelper.h"

#include <QComboBox>
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

#include <sys/resource.h>

namespace {

constexpr int STUCK_GRACE_S = 120;  // stressapptest ends itself; one still running this long after is killed
constexpr int SETTLE_MS = 3000;     // kernel events reach onFault ~1.5 s after they are logged (HealthTab debounce): wait for them

QString findTool(const QString &name) {
    QString p = QStandardPaths::findExecutable(name);
    if (p.isEmpty()) p = QStandardPaths::findExecutable(name, {QStringLiteral("/usr/sbin"), QStringLiteral("/usr/local/bin"), QStringLiteral("/usr/bin")});
    return p;
}

qint64 memlockBytes() {
    rlimit r{};
    if (getrlimit(RLIMIT_MEMLOCK, &r) != 0 || r.rlim_cur == RLIM_INFINITY) return -1;
    return static_cast<qint64>(r.rlim_cur);
}

} // namespace

MemTestBox::MemTestBox(QWidget *parent) : QWidget(parent) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(new QLabel(QStringLiteral("<b>%1</b>").arg(tr("Memory test"))));

    for (const auto t : {memtest::Tool::Stressapptest, memtest::Tool::Memtester}) {
        program_ = findTool(memtest::toolName(t));
        if (!program_.isEmpty()) { tool_ = t; break; }
    }
    if (tool_ == memtest::Tool::None) {
        auto *hint = new QLabel(QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::MUTED,
            tr("Checks memory timings by writing and reading back most of the free memory. Needs stressapptest or "
               "memtester: %1").arg(systools::installCommand(QStringLiteral("memtester"))).toHtmlEscaped()));
        hint->setWordWrap(true);
        hint->setTextInteractionFlags(Qt::TextSelectableByMouse);
        root->addWidget(hint);
        return;
    }

    auto *intro = new QLabel(QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::MUTED,
        tr("Writes and reads back a share of the free memory with %1 and watches for machine checks meanwhile. "
           "The machine is slow while it runs. Each result is kept with the BIOS memory timings in effect.")
            .arg(memtest::toolName(tool_)).toHtmlEscaped()));
    intro->setWordWrap(true);
    root->addWidget(intro);
    klogNote_ = new QLabel(QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::MUTED,
        tr("The kernel log is not readable without a password here, so machine checks are not counted during the test; "
           "only the test's own results are.").toHtmlEscaped()));
    klogNote_->setWordWrap(true);
    klogNote_->setVisible(!kernelLive_);
    root->addWidget(klogNote_);

    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Free memory to test")));
    percent_ = new QSpinBox;
    percent_->setRange(10, 90);
    percent_->setSingleStep(10);
    percent_->setValue(70);
    percent_->setSuffix(QStringLiteral(" %"));
    row->addWidget(percent_);
    row->addWidget(new QLabel(tr("Duration")));
    minutes_ = new QComboBox;
    for (int m : {5, 15, 30, 60}) minutes_->addItem(tr("%n min", nullptr, m), m);
    minutes_->setCurrentIndex(1);
    row->addWidget(minutes_);
    run_ = new QPushButton(tr("Start"));
    connect(run_, &QPushButton::clicked, this, [this] {
        if (!proc_) { start(); return; }
        cancelled_ = true;
        proc_->terminate();
    });
    row->addWidget(run_);
    row->addStretch(1);
    root->addLayout(row);

    status_ = new QLabel;
    status_->setWordWrap(true);
    status_->setVisible(false);
    root->addWidget(status_);

    results_ = new QTreeWidget;
    results_->setRootIsDecorated(false);
    results_->setHeaderLabels({tr("Date"), tr("Tool"), tr("Memory"), tr("Duration"), tr("Result"), tr("BIOS memory timings")});
    results_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    results_->setMaximumHeight(160);
    root->addWidget(results_);

    ticker_ = new QTimer(this);  // only while a test runs
    ticker_->setInterval(1000);
    connect(ticker_, &QTimer::timeout, this, &MemTestBox::tick);
    fill();
}

void MemTestBox::setKernelLog(bool live) {
    kernelLive_ = live;
    if (klogNote_) klogNote_->setVisible(!live);
}

void MemTestBox::onFault(const QJsonObject &e) {
    if (proc_ && e.value(QStringLiteral("kind")).toString() == QLatin1String("mce")) ++mce_;
}

void MemTestBox::start() {
    QFile f(QStringLiteral("/proc/meminfo"));
    const qint64 avail = f.open(QIODevice::ReadOnly) ? memtest::availableKib(f.readAll()) : -1;
    const int seconds = minutes_->currentData().toInt() * 60;
    const memtest::Plan p = memtest::plan(tool_, avail, percent_->value(), seconds, memlockBytes());
    status_->setVisible(true);
    if (!p.error.isEmpty()) {
        const QString text = p.error == QLatin1String("memlock")
            ? tr("memtester must lock the memory it tests, and this session may lock only %1 MiB (RLIMIT_MEMLOCK). "
                 "Raise the limit (for example \"@wheel - memlock unlimited\" in /etc/security/limits.conf, then log in "
                 "again) or install stressapptest, which needs no lock.").arg(memlockBytes() / (1024 * 1024))
            : tr("Too little free memory to test (under %1 MiB).").arg(memtest::MIN_MIB);
        status_->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::WARN, text.toHtmlEscaped()));
        return;
    }
    run_->setEnabled(false);
    status_->setText(tr("Reading the BIOS memory timings…"));
    // The snapshot is read before the test: what the result is about.
    userhelper::run(QJsonObject{{QStringLiteral("op"), QStringLiteral("mem_snapshot")}}, this, [this, p](const QJsonObject &r) {
        QJsonObject bios = r;
        if (r.contains(QStringLiteral("error")) && !r.contains(QStringLiteral("available"))) bios[QStringLiteral("available")] = false;
        launch(p, bios);
    });
}

void MemTestBox::launch(const memtest::Plan &p, const QJsonObject &bios) {
    plan_ = p;
    bios_ = bios;
    seconds_ = minutes_->currentData().toInt() * 60;
    run_state_ = {};
    partial_.clear();
    mce_ = 0;
    stopped_ = cancelled_ = false;
    proc_ = new QProcess(this);
    proc_->setProcessChannelMode(QProcess::MergedChannels);
    connect(proc_, &QProcess::readyRead, this, [this] {
        partial_ += proc_->readAll();
        qsizetype nl;
        while ((nl = partial_.indexOf('\n')) >= 0) {
            memtest::feed(run_state_, tool_, QString::fromLocal8Bit(partial_.left(nl)));
            partial_.remove(0, nl + 1);
        }
        if (partial_.size() > 64 * 1024) partial_.clear();  // memtester redraws progress without newlines
    });
    connect(proc_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) { finished(code, st == QProcess::CrashExit); });
    connect(proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) finished(-1, true);
    });
    proc_->start(program_, p.args);
    elapsed_.start();
    ticker_->start();
    run_->setText(tr("Stop"));
    run_->setEnabled(true);
    percent_->setEnabled(false);
    minutes_->setEnabled(false);
    tick();
}

void MemTestBox::tick() {
    if (!proc_) return;
    const qint64 s = elapsed_.elapsed() / 1000;
    if (tool_ == memtest::Tool::Memtester && s >= seconds_ && !stopped_) {
        stopped_ = true;  // memtester has no time limit: the deadline is the normal end
        proc_->terminate();
    } else if (s >= seconds_ + STUCK_GRACE_S) {
        proc_->kill();
    }
    QString text = tr("Testing %1 MiB with %2: %3 of %4 min, %n error(s)", nullptr, run_state_.errors)
                       .arg(plan_.mib).arg(memtest::toolName(tool_)).arg(s / 60).arg(seconds_ / 60);
    if (mce_ > 0) text += QStringLiteral(" · ") + tr("%n machine check(s)", nullptr, mce_);
    if (plan_.clamped) text += QStringLiteral(" · ") + tr("held to the memlock limit");
    status_->setText(text);
}

void MemTestBox::finished(int exitCode, bool crashed) {
    if (!proc_ || settling_) return;
    ticker_->stop();
    if (!partial_.isEmpty()) memtest::feed(run_state_, tool_, QString::fromLocal8Bit(partial_));
    // The machine check that the failure caused is logged after the process is gone: keep counting them a
    // moment (proc_ stays set, so onFault counts), with Start disabled, before the verdict is decided.
    settling_ = true;
    run_->setEnabled(false);
    const qint64 seconds = elapsed_.elapsed() / 1000;
    QTimer::singleShot(SETTLE_MS, this, [this, exitCode, crashed, seconds] { conclude(exitCode, crashed, seconds); });
}

void MemTestBox::conclude(int exitCode, bool crashed, qint64 seconds) {
    settling_ = false;
    const memtest::Verdict v = memtest::verdict(run_state_, tool_, exitCode, crashed, stopped_, cancelled_, mce_);
    QJsonObject result{
        {QStringLiteral("t"), QDateTime::currentSecsSinceEpoch()}, {QStringLiteral("tool"), memtest::toolName(tool_)},
        {QStringLiteral("mib"), plan_.mib}, {QStringLiteral("seconds"), seconds},
        {QStringLiteral("verdict"), memtest::verdictName(v)}, {QStringLiteral("errors"), run_state_.errors},
        {QStringLiteral("mce"), mce_}, {QStringLiteral("clamped"), plan_.clamped},
        {QStringLiteral("unlocked"), run_state_.unlocked}, {QStringLiteral("bios"), bios_}};
    if (!kernelLive_) result[QStringLiteral("kernel_log")] = false;  // a pass without kernel events is weaker evidence
    const bool saved = memtest::append(result);
    proc_->deleteLater();
    proc_ = nullptr;
    run_->setText(tr("Start"));
    percent_->setEnabled(true);
    minutes_->setEnabled(true);
    run_->setEnabled(true);

    QString text;
    if (v == memtest::Verdict::Pass) text = QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::OK, tr("No errors.").toHtmlEscaped());
    else if (v == memtest::Verdict::Fail)
        text = QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::DANGER,
            tr("Memory errors: %1 from the test, %2 machine check(s). The memory timings or speed are not stable.")
                .arg(run_state_.errors).arg(mce_).toHtmlEscaped());
    else text = tr("Not finished: the test was stopped or ended early (exit code %1).").arg(exitCode).toHtmlEscaped();
    if (run_state_.unlocked) text += QStringLiteral("<br>") + tr("memtester could not lock its memory: parts of it may have been swapped out and not tested.").toHtmlEscaped();
    if (!saved) text += QStringLiteral("<br>") + tr("The result could not be saved to %1.").arg(memtest::stateFile()).toHtmlEscaped();
    status_->setText(text);
    if (v == memtest::Verdict::Fail) Q_EMIT alert(tr("Memory test failed"), tr("%n error(s) during the memory test.", nullptr, run_state_.errors + mce_));
    fill();
}

QString MemTestBox::biosText(const QJsonObject &bios) {
    if (!bios.value(QStringLiteral("available")).toBool()) return tr("not readable on this machine");
    QStringList parts;
    const QJsonObject manual = bios.value(QStringLiteral("manual")).toObject();
    for (auto it = manual.begin(); it != manual.end(); ++it) parts << it.key() + QLatin1Char(' ') + QString::number(it.value().toInt());
    QString s = tr("%1 MT/s").arg(bios.value(QStringLiteral("speed_mts")).toInt()) + QStringLiteral(", ")
              + (parts.isEmpty() ? tr("all Auto") : parts.join(QStringLiteral(", ")));
    if (bios.value(QStringLiteral("pending")).toBool()) s += QStringLiteral(" ") + tr("(changed this boot: not in effect until the next boot)");
    return s;
}

void MemTestBox::fill() {
    results_->clear();
    const QJsonArray all = memtest::load();
    for (qsizetype i = all.size() - 1; i >= 0; --i) {
        const QJsonObject o = all.at(i).toObject();
        const QString v = o.value(QStringLiteral("verdict")).toString();
        const QString shown = v == QLatin1String("pass") ? tr("pass") : v == QLatin1String("fail") ? tr("fail") : tr("incomplete");
        auto *it = new QTreeWidgetItem({
            QDateTime::fromSecsSinceEpoch(o.value(QStringLiteral("t")).toInteger()).toString(QStringLiteral("yyyy-MM-dd HH:mm")),
            o.value(QStringLiteral("tool")).toString(), tr("%1 MiB").arg(o.value(QStringLiteral("mib")).toInteger()),
            tr("%n min", nullptr, int(o.value(QStringLiteral("seconds")).toInteger() / 60)), shown,
            biosText(o.value(QStringLiteral("bios")).toObject())});
        const char *color = v == QLatin1String("pass") ? theme::OK : v == QLatin1String("fail") ? theme::DANGER : theme::MUTED;
        it->setForeground(4, QColor(QLatin1String(color)));
        results_->addTopLevelItem(it);
    }
    results_->setVisible(!all.isEmpty());
}
