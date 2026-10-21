#include "fanassist.h"
#include "platformprofile.h"
#include "privileged.h"
#include "scenes.h"
#include "theme.h"
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSaveFile>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <memory>
#include <cmath>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

static constexpr int WARMUP_S = 60, SETTLE_S = 60, SAMPLE_S = 30;  // per level: settle, then average
static constexpr double ABORT_C = 98.0;  // a level that cannot hold the CPU below this is recorded as such

struct Goal { const char *name; double start, target; int top; };
static const Goal GOALS[] = {{"Quiet", 62, 86, 8}, {"Balanced", 55, 80, 9}, {"Cool", 48, 72, 10}};

static QString backupFile() {
    QString dir = qEnvironmentVariable("XDG_STATE_HOME");
    if (dir.isEmpty()) dir = QDir::homePath() + QStringLiteral("/.local/state");
    return dir + QStringLiteral("/centurion/fan-assist-backup.json");
}

QVector<int> FanAssistDialog::pendingBackup() {
    QFile f(backupFile());
    if (!f.open(QIODevice::ReadOnly) || f.size() > 4096) return {};
    QVector<int> lv;
    for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).object().value("levels").toArray()) lv << v.toInt();
    return lv.size() == 10 ? lv : QVector<int>();
}

void FanAssistDialog::clearBackup() { QFile::remove(backupFile()); }

/// Non-decreasing, 1..10, hottest step at least 7 (legion-profile-helper's thermal floor).
static QVector<int> legal(QVector<int> lv) {
    for (int &x : lv) x = std::clamp(x, 1, 10);
    for (int i = 1; i < lv.size(); ++i) lv[i] = std::max(lv[i], lv[i - 1]);
    if (lv.size() == 10) lv[9] = std::max(lv[9], 7);
    return lv;
}

QVector<int> FanAssistDialog::shape(int goal, int hold, const QVector<int> &temps) {
    const Goal &g = GOALS[std::clamp(goal, 0, 2)];
    hold = std::clamp(hold, 2, 10);
    QVector<int> lv(10);
    for (int s = 0; s < 10; ++s) {
        const double t = temps.value(s, 40 + 6 * s);
        int l;
        if (t < g.start) l = goal == 2 ? 2 : 1;
        else if (t < g.target) l = int(std::lround(1 + (hold - 1) * (t - g.start) / (g.target - g.start)));
        else if (t < g.target + 8) l = hold;
        else l = 10;
        lv[s] = l;
    }
    return legal(lv);
}

/// The CPU fan's temperature step thresholds (sensor 1 on the Legion EC; else the first fan), 127 = never.
QVector<int> FanAssistDialog::stepTemps() const {
    const FanCurveFan *f = nullptr;
    for (const FanCurveFan &x : fans_) if (x.sensor == 1 || !f) f = &x;
    if (!f || f->temp.size() != 10) return {};
    QVector<int> t = f->temp;
    for (int s = 0; s < 10; ++s) if (t[s] >= 127) t[s] = s ? t[s - 1] + 5 : 95;
    return t;
}

double FanAssistDialog::cpuTemp() {
    const QDir d(QStringLiteral("/sys/class/hwmon"));
    for (const QString &h : d.entryList({"hwmon*"}, QDir::Dirs | QDir::System)) {
        const auto name = pp::readText(d.filePath(h) + "/name").value_or(QString());
        if (name != QLatin1String("k10temp") && name != QLatin1String("coretemp") && name != QLatin1String("zenpower")) continue;
        // k10temp temp1 = Tctl, coretemp temp1 = Package id 0, zenpower temp1 = Tdie.
        if (const auto v = pp::readText(d.filePath(h) + "/temp1_input"); v && !v->isEmpty()) return v->toDouble() / 1000.0;
    }
    return -1;
}

FanAssistDialog::FanAssistDialog(const QString &helper, const QVector<int> &current, const QVector<FanCurveFan> &fans, QWidget *parent)
    : QDialog(parent), helper_(helper), current_(current), fans_(fans) {
    setWindowTitle(QStringLiteral("Fan curve assistant"));
    auto *v = new QVBoxLayout(this);
    auto *intro = new QLabel(QStringLiteral(
        "Proposes a curve for a goal. <b>Quick</b> shapes it on the EC's own temperature steps. <b>Measure</b> runs a full CPU "
        "load for about 6 minutes with the fans held at three levels in turn, to find the level that holds the goal temperature "
        "on <i>this</i> machine (Custom profile, on AC; your table is written back afterwards). The result goes into the editor — "
        "press Apply there to keep it."));
    intro->setWordWrap(true);
    intro->setTextFormat(Qt::RichText);
    v->addWidget(intro);
    auto *h = new QHBoxLayout;
    h->addWidget(new QLabel(QStringLiteral("Goal")));
    goal_ = new QComboBox;
    for (int i = 0; i < 3; ++i)
        goal_->addItem(QStringLiteral("%1 — up to ~%2 °C under load").arg(QLatin1String(GOALS[i].name)).arg(GOALS[i].target, 0, 'f', 0), i);
    goal_->setCurrentIndex(1);
    h->addWidget(goal_, 1);
    v->addLayout(h);
    quick_ = new QRadioButton(QStringLiteral("Quick (instant, from the firmware's temperature steps)"));
    measure_ = new QRadioButton(QStringLiteral("Measure (~6 min full CPU load, Custom profile, on AC)"));
    quick_->setChecked(true);
    v->addWidget(quick_);
    v->addWidget(measure_);
    bar_ = new QProgressBar;
    bar_->hide();
    v->addWidget(bar_);
    status_ = new QLabel;
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::RichText);
    v->addWidget(status_);
    auto *b = new QHBoxLayout;
    b->addStretch(1);
    run_ = new QPushButton(QStringLiteral("Propose"));
    run_->setObjectName("btnAccent");
    use_ = new QPushButton(QStringLiteral("Put in the editor"));
    use_->setEnabled(false);
    auto *close = new QPushButton(QStringLiteral("Cancel"));
    b->addWidget(run_);
    b->addWidget(use_);
    b->addWidget(close);
    v->addLayout(b);
    connect(close, &QPushButton::clicked, this, &FanAssistDialog::reject);
    connect(use_, &QPushButton::clicked, this, &QDialog::accept);
    connect(run_, &QPushButton::clicked, this, [this] {
        if (measure_->isChecked()) { startMeasure(); return; }
        result_ = shape(goal_->currentData().toInt(), GOALS[goal_->currentData().toInt()].top - 1, stepTemps());
        QStringList l;
        for (int x : result_) l << QString::number(x);
        status_->setText(QStringLiteral("Proposed: <b>%1</b>%2").arg(l.join(QStringLiteral(" · ")),
                         stepTemps().isEmpty() ? QStringLiteral(" (no per-fan temperature steps from the EC: generic spacing)") : QString()));
        use_->setEnabled(true);
    });
    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, &FanAssistDialog::tick);
}

FanAssistDialog::~FanAssistDialog() { stopLoad(); }

void FanAssistDialog::reject() {
    if (measuring_) {
        if (QMessageBox::question(this, windowTitle(), QStringLiteral("Stop the measurement and write your table back?")) != QMessageBox::Yes) return;
        finishMeasure(false, QStringLiteral("cancelled"));
        return;  // closes once the original table is written back
    }
    QDialog::reject();
}

void FanAssistDialog::writeTable(const QVector<int> &lv, std::function<void(bool, const QString &)> done) {
    QJsonArray a;
    for (int x : lv) a << x;
    writing_ = true;
    privileged::run(helper_, QJsonObject{{"fan_table", "set"}, {"levels", a}}, this, [this, done](const privileged::Result &r) {
        writing_ = false;
        done(r.ok(), r.message());
    });
}

void FanAssistDialog::startLoad() {
    stop_ = false;
    const unsigned n = std::max(1u, std::thread::hardware_concurrency());
    for (unsigned i = 0; i < n; ++i)
        workers_.emplace_back([this, i] {
            // Lowest priority: the desktop (and this dialog) stay responsive; the CPU is still fully loaded.
            setpriority(PRIO_PROCESS, pid_t(::syscall(SYS_gettid)), 19);
            double a = 1.0 + i * 1e-6, b = 0.999999, c = 1e-9;
            while (!stop_.load(std::memory_order_relaxed))
                for (int k = 0; k < 200000; ++k) { a = a * b + c; b = b * a - c; if (a > 4 || a < 0.25) a = 1.0; if (b > 4 || b < 0.25) b = 0.999999; }
            volatile double sink = a + b;
            (void)sink;
        });
}

void FanAssistDialog::stopLoad() {
    stop_ = true;
    for (std::thread &t : workers_) if (t.joinable()) t.join();
    workers_.clear();
}

void FanAssistDialog::startMeasure() {
    if (pp::currentProfile(pp::primaryHandler()) != QStringLiteral("custom")) {
        status_->setText(QStringLiteral("<span style='color:%1'>Switch to the Custom power profile first: the EC follows the table only there.</span>").arg(theme::WARN));
        return;
    }
    if (scenes::onAc() != std::optional<bool>(true)) {
        status_->setText(QStringLiteral("<span style='color:%1'>Plug in the charger: on battery the power limits are lower and the result would not hold on AC.</span>").arg(theme::WARN));
        return;
    }
    if (cpuTemp() < 0) { status_->setText(QStringLiteral("No CPU temperature sensor (k10temp / coretemp) found.")); return; }
    if (current_.size() != 10) { status_->setText(QStringLiteral("The current table has not been read yet.")); return; }
    // Saved before the first write: an interrupted run is undone the next time the fan curve dialog opens.
    {
        QDir().mkpath(QFileInfo(backupFile()).absolutePath());
        QSaveFile f(backupFile());
        QJsonArray a;
        for (int x : current_) a << x;
        if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(QJsonObject{{"levels", a}}).toJson(QJsonDocument::Compact)) < 0 || !f.commit()) {
            status_->setText(QStringLiteral("Cannot save the current table to ") + backupFile());
            return;
        }
    }
    measuring_ = true;
    run_->setEnabled(false);
    use_->setEnabled(false);
    goal_->setEnabled(false);
    quick_->setEnabled(false);
    measure_->setEnabled(false);
    steady_.clear();
    bar_->setRange(0, WARMUP_S + int(levels_.size()) * (SETTLE_S + SAMPLE_S));
    bar_->setValue(0);
    bar_->show();
    idx_ = -1;
    elapsed_ = 0;
    startLoad();
    status_->setText(QStringLiteral("Warming up under full load…"));
    timer_->start();
}

void FanAssistDialog::nextLevel() {
    ++idx_;
    if (idx_ >= levels_.size()) { finishMeasure(true, {}); return; }
    QVector<int> flat(10, levels_[idx_]);
    timer_->stop();
    touched_ = true;
    writeTable(legal(flat), [this](bool ok, const QString &m) {
        if (!measuring_) return;
        if (!ok) { finishMeasure(false, QStringLiteral("could not write the fan table: ") + m); return; }
        samples_.clear();
        elapsed_ = 0;
        timer_->start();
    });
}

void FanAssistDialog::tick() {
    if (!measuring_ || writing_) return;
    const double t = cpuTemp();
    bar_->setValue(bar_->value() + 1);
    if (idx_ < 0) {
        status_->setText(QStringLiteral("Warming up under full load… CPU %1 °C").arg(t, 0, 'f', 0));
        if (++elapsed_ >= WARMUP_S) nextLevel();
        return;
    }
    ++elapsed_;
    if (elapsed_ > SETTLE_S) samples_ << t;
    status_->setText(QStringLiteral("Fans held at level %1 (%2 of %3): CPU %4 °C, %5 s left")
                         .arg(levels_[idx_]).arg(idx_ + 1).arg(levels_.size()).arg(t, 0, 'f', 0).arg(SETTLE_S + SAMPLE_S - elapsed_));
    // A level that lets the CPU run into its limit is recorded as such and the next (higher) one starts at once.
    const bool hot = t >= ABORT_C && elapsed_ > 10;
    if (elapsed_ >= SETTLE_S + SAMPLE_S || hot) {
        double avg = hot ? ABORT_C : 0;
        if (!hot) { for (double x : samples_) avg += x; avg /= std::max<qsizetype>(1, samples_.size()); }
        steady_ << avg;
        bar_->setValue(WARMUP_S + int(idx_ + 1) * (SETTLE_S + SAMPLE_S));
        nextLevel();
    }
}

void FanAssistDialog::finishMeasure(bool ok, const QString &why) {
    timer_->stop();
    stopLoad();
    const QVector<int> original = current_;
    const bool wasMeasuring = measuring_;
    measuring_ = false;
    auto after = [this, ok, why, wasMeasuring](bool restored, const QString &m) {
        if (restored) clearBackup();
        bar_->hide();
        run_->setEnabled(true);
        goal_->setEnabled(true);
        quick_->setEnabled(true);
        measure_->setEnabled(true);
        const QString back = restored ? QStringLiteral("Your table was written back.")
                                      : QStringLiteral("<span style='color:%1'>Could not write your table back (%2) — it is saved and offered again "
                                                       "when the fan curve dialog opens.</span>").arg(theme::DANGER, m.toHtmlEscaped());
        if (!ok) {
            status_->setText(QStringLiteral("Stopped: %1. %2").arg(why.toHtmlEscaped(), back));
            if (why == QLatin1String("cancelled") && wasMeasuring) QDialog::reject();
            return;
        }
        // Lowest level whose steady temperature stays at or below the goal (linear between measured levels).
        const int g = goal_->currentData().toInt();
        const double target = GOALS[g].target;
        int hold = 10;
        for (int i = 0; i < steady_.size(); ++i) {
            if (steady_[i] > target) continue;
            if (i == 0) { hold = levels_[0]; break; }
            const double t0 = steady_[i - 1], t1 = steady_[i];
            const double f = t0 > t1 ? (t0 - target) / (t0 - t1) : 1.0;
            hold = int(std::ceil(levels_[i - 1] + f * (levels_[i] - levels_[i - 1])));
            break;
        }
        hold = std::clamp(hold, 2, 10);
        result_ = shape(g, hold, stepTemps());
        QStringList meas, l;
        for (int i = 0; i < steady_.size(); ++i)
            meas << QStringLiteral("level %1 → %2%3 °C").arg(levels_[i]).arg(steady_[i] >= ABORT_C ? QStringLiteral("≥") : QString()).arg(steady_[i], 0, 'f', 0);
        for (int x : result_) l << QString::number(x);
        status_->setText(QStringLiteral("Measured under full CPU load: %1.<br>Level <b>%2</b> holds ~%3 °C. Proposed: <b>%4</b>. %5")
                             .arg(meas.join(QStringLiteral(", "))).arg(hold).arg(target, 0, 'f', 0).arg(l.join(QStringLiteral(" · ")), back));
        use_->setEnabled(true);
    };
    if (!touched_) { after(true, {}); return; }
    // Never two helper calls at once: wait for a write still in flight, then put the original back.
    auto restore = std::make_shared<std::function<void()>>();
    *restore = [this, original, after, w = std::weak_ptr<std::function<void()>>(restore)] {
        // The retry holds the chain strongly: the caller's shared_ptr is gone by then, and a weak-only
        // retry found nothing to call and never wrote the table back.
        if (writing_) { if (auto r = w.lock()) QTimer::singleShot(500, this, [r] { (*r)(); }); return; }
        writeTable(original, [this, after](bool okW, const QString &m) { if (okW) touched_ = false; after(okW, m); });
    };
    (*restore)();
}
