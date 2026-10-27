#include "coolingpage.h"
#include "theme.h"
#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <cmath>

static QString toolPath() {
    const QString p = QStandardPaths::findExecutable(QStringLiteral("centurion-cooltest"));
    return p.isEmpty() ? QStringLiteral("/usr/bin/centurion-cooltest") : p;
}

static QLabel *muted(const QString &t) {
    auto *l = new QLabel(t);
    l->setWordWrap(true);
    l->setStyleSheet(QStringLiteral("color:%1").arg(theme::MUTED));
    return l;
}

/// The value of reason `code` in an assessment (NaN when the assessment does not list it).
static double reason(const QJsonObject &a, const char *code) {
    for (const QJsonValue &v : a.value(QStringLiteral("reasons")).toArray()) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("code")).toString() == QLatin1String(code)) return o.value(QStringLiteral("value")).toDouble();
    }
    return std::nan("");
}

QString CoolingPage::verdictText(const QJsonObject &a, double limitC) {
    const QString level = a.value(QStringLiteral("level")).toString();
    const double dTemp = a.value(QStringLiteral("d_temp")).toDouble();
    QString t;
    if (level == QLatin1String("inconclusive")) {
        if (!std::isnan(reason(a, "noisy")))
            return tr("Other programs were using the CPU during the test, so this run cannot be compared with the baseline. "
                      "Close them and run the test again.");
        return tr("The laptop started this test %1 °C warmer than it did at the baseline (a warmer room, or it had not cooled down). "
                  "Let it rest and run the test again.").arg(reason(a, "warm_start"), 0, 'f', 0);
    }
    if (level == QLatin1String("ok")) t = tr("Cooling is as good as the baseline: the same load runs at about the same temperature.");
    else if (level == QLatin1String("better"))
        t = tr("Cooling is better than the baseline (%1 °C). If you have just cleaned the laptop, press \"Use the latest run as baseline\".")
                .arg(dTemp, 0, 'f', 1);
    else if (level == QLatin1String("watch"))
        t = tr("Cooling is slightly weaker than the baseline. Repeat the test in a few weeks; if it keeps drifting, clean the vents and fans.");
    else
        t = tr("Cooling is clearly weaker than the baseline. Dust in the heat sink and fans, or dried-out thermal paste, are the usual causes: "
               "clean the vents and fans first, and re-paste only if the difference stays.");
    QStringList facts;
    if (level == QLatin1String("warn") || level == QLatin1String("watch")) {
        if (!std::isnan(reason(a, "temp_up"))) facts << tr("%1 °C hotter at the same load").arg(reason(a, "temp_up"), 0, 'f', 1);
        if (!std::isnan(reason(a, "clock_down"))) facts << tr("clock %1 % lower").arg(reason(a, "clock_down"), 0, 'f', 0);
        if (!std::isnan(reason(a, "fan_up"))) facts << tr("fans %1 % faster").arg(reason(a, "fan_up"), 0, 'f', 0);
        if (!facts.isEmpty()) t += QLatin1Char(' ') + facts.join(QStringLiteral(", ")) + QLatin1Char('.');
        if (!std::isnan(reason(a, "fan_up"))) t += QLatin1Char(' ') + tr("(If you changed the fan curve since the baseline, ignore the fan figure.)");
    }
    if (a.value(QStringLiteral("pinned")).toBool())
        t += QLatin1Char(' ') + tr("The firmware holds the CPU at its %1 °C temperature target, so the temperature cannot rise further: the clock is what shows weaker cooling.")
                                     .arg(limitC, 0, 'f', 0);
    return t;
}

CoolingPage::CoolingPage(QWidget *parent) : QWidget(parent) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(6);

    root->addWidget(muted(tr("Puts a fixed load on every CPU thread for a few minutes and records the temperature, fan speed and clock. "
                             "The first clean run becomes the baseline; later runs are compared with it, so dust in the fans or dried-out thermal paste "
                             "show up as a hotter die, louder fans or lower clocks. Results are only compared under the same firmware settings (below).")));
    context_ = new QLabel;
    context_->setTextFormat(Qt::RichText);
    context_->setWordWrap(true);
    root->addWidget(context_);

    auto *row = new QHBoxLayout;
    run_ = new QPushButton(tr("Run cooling test"));
    run_->setToolTip(tr("Loads every CPU thread for the chosen time (after a short cool-down). The fans will run loud."));
    cancel_ = new QPushButton(tr("Cancel"));
    cancel_->setVisible(false);
    length_ = new QComboBox;
    length_->addItem(tr("2 minutes (short)"), 120);
    length_->addItem(tr("4 minutes (recommended)"), 240);
    length_->addItem(tr("8 minutes (thorough)"), 480);
    length_->setCurrentIndex(1);
    length_->setToolTip(tr("Use the same length every time you compare."));
    row->addWidget(run_);
    row->addWidget(cancel_);
    row->addWidget(length_);
    row->addStretch(1);
    root->addLayout(row);

    bar_ = new QProgressBar;
    bar_->setVisible(false);
    root->addWidget(bar_);
    live_ = new QLabel;
    live_->setTextFormat(Qt::RichText);
    root->addWidget(live_);

    verdict_ = new QLabel;
    verdict_->setObjectName(QStringLiteral("coolBanner"));
    verdict_->setWordWrap(true);
    verdict_->setVisible(false);
    root->addWidget(verdict_);

    table_ = new QTreeWidget;
    table_->setColumnCount(6);
    table_->setHeaderLabels({tr("Date"), tr("CPU temperature"), tr("Fan"), tr("Clock"), tr("Started at"), tr("Compared with the baseline")});
    table_->setRootIsDecorated(false);
    table_->setAlternatingRowColors(true);
    for (int i = 0; i < 5; ++i) table_->header()->setSectionResizeMode(i, QHeaderView::ResizeToContents);
    table_->header()->setStretchLastSection(true);
    root->addWidget(table_, 1);

    auto *foot = new QHBoxLayout;
    baseline_ = new QPushButton(tr("Use the latest run as baseline"));
    baseline_->setToolTip(tr("Make the newest clean run the reference for these firmware settings, e.g. after cleaning the fans."));
    forget_ = new QPushButton(tr("Forget results for these settings"));
    foot->addStretch(1);
    foot->addWidget(baseline_);
    foot->addWidget(forget_);
    root->addLayout(foot);
    root->addWidget(muted(tr("For a fair comparison: plug in the charger, leave the fans on Auto, close other programs, use the same room and the same test length. "
                             "A changed power limit or temperature target starts a new baseline, since it changes how much heat the load makes.")));

    connect(run_, &QPushButton::clicked, this, &CoolingPage::start);
    connect(cancel_, &QPushButton::clicked, this, [this] { if (proc_) proc_->kill(); });
    connect(baseline_, &QPushButton::clicked, this, [this] {
        tool({QStringLiteral("baseline")}, [this](const QJsonObject &o) {
            if (!o.value(QStringLiteral("ok")).toBool()) QMessageBox::information(this, tr("Cooling"), o.value(QStringLiteral("error")).toString());
            refresh();
        });
    });
    connect(forget_, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Cooling"), tr("Forget every cooling result stored for the current firmware settings?")) != QMessageBox::Yes) return;
        tool({QStringLiteral("forget")}, [this](const QJsonObject &) { verdict_->setVisible(false); refresh(); });
    });
}

CoolingPage::~CoolingPage() {
    if (proc_) { proc_->disconnect(this); proc_->kill(); proc_->waitForFinished(2000); }
}

void CoolingPage::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    if (!proc_) refresh();
}

void CoolingPage::tool(const QStringList &args, const std::function<void(const QJsonObject &)> &done) {
    auto *p = new QProcess(this);
    connect(p, &QProcess::finished, this, [p, done](int, QProcess::ExitStatus) {
        const QList<QByteArray> lines = p->readAllStandardOutput().trimmed().split('\n');
        const QJsonObject o = QJsonDocument::fromJson(lines.isEmpty() ? QByteArray() : lines.last()).object();
        p->deleteLater();
        done(o);
    });
    connect(p, &QProcess::errorOccurred, this, [p, done](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        p->deleteLater();
        done(QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), tr("centurion-cooltest not found")}});
    });
    p->start(toolPath(), args);
}

void CoolingPage::refresh() {
    tool({QStringLiteral("status")}, [this](const QJsonObject &o) { fillStatus(o); });
}

static QString fw(const QJsonObject &limits, const char *k) {
    return limits.contains(QLatin1String(k)) ? QString::number(limits.value(QLatin1String(k)).toInt()) : QStringLiteral("–");
}

void CoolingPage::fillStatus(const QJsonObject &s) {
    table_->clear();
    if (!s.value(QStringLiteral("ok")).toBool()) {
        context_->setText(tr("Could not read the cooling status: %1").arg(s.value(QStringLiteral("error")).toString(tr("centurion-cooltest failed"))));
        run_->setEnabled(false);
        return;
    }
    limitC_ = s.value(QStringLiteral("limit_c")).toDouble(95);
    const QJsonObject c = s.value(QStringLiteral("context")).toObject();
    const QJsonObject lim = c.value(QStringLiteral("limits")).toObject();
    const QString power = c.value(QStringLiteral("power")).toString();
    const QString powerText = power == QLatin1String("ac") ? tr("on AC") : power == QLatin1String("battery") ? tr("on battery") : tr("power source unknown");
    QString line = tr("<b>Compared under:</b> power profile %1 · %2 · PL1 / PL2 / PL3 %3 / %4 / %5 W")
                       .arg(c.value(QStringLiteral("profile")).toString(QStringLiteral("?")).toHtmlEscaped(), powerText, fw(lim, "ppt_pl1_spl"),
                            fw(lim, "ppt_pl2_sppt"), fw(lim, "ppt_pl3_fppt"));
    line += lim.contains(QStringLiteral("cpu_temp")) ? tr(" · temperature target %1 °C").arg(fw(lim, "cpu_temp"))
                                                      : tr(" · no firmware temperature target (assuming %1 °C)").arg(limitC_, 0, 'f', 0);
    context_->setText(line);

    const bool sensor = s.value(QStringLiteral("has_sensor")).toBool();
    run_->setEnabled(sensor && !proc_);
    if (!sensor) live_->setText(tr("<span style='color:%1'>No CPU temperature sensor found (k10temp / coretemp), so the test cannot run.</span>").arg(theme::WARN));

    const QJsonObject base = s.value(QStringLiteral("baseline")).toObject();
    const QJsonArray runs = s.value(QStringLiteral("runs")).toArray();
    bool anyClean = false;
    for (int i = runs.size() - 1; i >= 0; --i) {
        const QJsonObject r = runs.at(i).toObject();
        auto *it = new QTreeWidgetItem(table_);
        it->setText(0, QDateTime::fromSecsSinceEpoch(qint64(r.value(QStringLiteral("t")).toDouble())).toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        it->setText(1, tr("%1 °C").arg(r.value(QStringLiteral("steady_temp")).toDouble(), 0, 'f', 1));
        it->setText(2, r.value(QStringLiteral("fan_rpm")).isNull() ? QStringLiteral("–") : tr("%1 rpm").arg(qRound(r.value(QStringLiteral("fan_rpm")).toDouble())));
        it->setText(3, r.value(QStringLiteral("mhz")).isNull() ? QStringLiteral("–") : tr("%1 GHz").arg(r.value(QStringLiteral("mhz")).toDouble() / 1000.0, 0, 'f', 2));
        it->setText(4, tr("%1 °C").arg(r.value(QStringLiteral("start_temp")).toDouble(), 0, 'f', 0));
        const bool noisy = r.value(QStringLiteral("noisy")).toBool();
        if (!noisy) anyClean = true;
        if (r.value(QStringLiteral("baseline")).toBool()) it->setText(5, tr("baseline"));
        else if (noisy) it->setText(5, tr("other programs were busy: not compared"));
        else if (!base.isEmpty())
            it->setText(5, tr("%1 °C").arg(QString::asprintf("%+.1f", r.value(QStringLiteral("steady_temp")).toDouble() - base.value(QStringLiteral("steady_temp")).toDouble())));
    }
    baseline_->setEnabled(anyClean);
    forget_->setEnabled(!runs.isEmpty());

    if (s.contains(QStringLiteral("assessment"))) {
        const QJsonObject a = s.value(QStringLiteral("assessment")).toObject();
        const QString lv = a.value(QStringLiteral("level")).toString();
        showVerdict(verdictText(a, limitC_), lv == QLatin1String("warn") ? theme::DANGER : lv == QLatin1String("watch") || lv == QLatin1String("inconclusive") ? theme::WARN
                                                                                       : theme::OK);
    } else if (!runs.isEmpty() && !base.isEmpty()) {
        showVerdict(tr("This is the baseline for these firmware settings. Run the test again in a few weeks, and after cleaning, to compare."), theme::INFO);
    } else {
        verdict_->setVisible(false);
    }
}

void CoolingPage::showVerdict(const QString &text, const char *color) {
    verdict_->setText(text);
    verdict_->setStyleSheet(theme::banner(color, QStringLiteral("#coolBanner")));
    verdict_->setVisible(true);
}

void CoolingPage::setRunning(bool on) {
    run_->setEnabled(!on);
    length_->setEnabled(!on);
    baseline_->setEnabled(!on && baseline_->isEnabled());
    forget_->setEnabled(!on && forget_->isEnabled());
    cancel_->setVisible(on);
    bar_->setVisible(on);
}

void CoolingPage::start() {
    if (proc_) return;
    verdict_->setVisible(false);
    live_->clear();
    buf_.clear();
    proc_ = new QProcess(this);
    proc_->setProcessChannelMode(QProcess::SeparateChannels);
    connect(proc_, &QProcess::readyReadStandardOutput, this, &CoolingPage::onOutput);
    connect(proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        finished(QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), tr("centurion-cooltest not found")}});
    });
    connect(proc_, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        onOutput();
        if (!proc_) return;  // the result line was handled
        // A killed or crashed tool never printed its result line.
        finished(QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), tr("The test was stopped.")}});
    });
    setRunning(true);
    bar_->setRange(0, 0);  // busy until the tool reports its first progress
    live_->setText(tr("Cooling down before the test…"));
    proc_->start(toolPath(), {QStringLiteral("run"), QStringLiteral("--seconds"), QString::number(length_->currentData().toInt())});
}

void CoolingPage::onOutput() {
    if (!proc_) return;
    buf_ += proc_->readAllStandardOutput();
    int nl;
    while ((nl = buf_.indexOf('\n')) >= 0) {
        const QJsonObject o = QJsonDocument::fromJson(buf_.left(nl)).object();
        buf_.remove(0, nl + 1);
        if (o.contains(QStringLiteral("phase"))) {
            if (o.value(QStringLiteral("phase")).toString() == QLatin1String("load")) {
                const int total = o.value(QStringLiteral("total")).toInt(), el = qRound(o.value(QStringLiteral("elapsed")).toDouble());
                bar_->setRange(0, total);
                bar_->setValue(el);
                bar_->setFormat(tr("%1 / %2 s").arg(el).arg(total));
                QStringList p{tr("CPU %1 °C").arg(o.value(QStringLiteral("temp")).toDouble(), 0, 'f', 0)};
                if (!o.value(QStringLiteral("fan")).isNull()) p << tr("fan %1 rpm").arg(qRound(o.value(QStringLiteral("fan")).toDouble()));
                if (!o.value(QStringLiteral("mhz")).isNull()) p << tr("%1 GHz").arg(o.value(QStringLiteral("mhz")).toDouble() / 1000.0, 0, 'f', 2);
                live_->setText(tr("Testing: %1").arg(p.join(QStringLiteral(" · "))));
            }
        } else if (o.contains(QStringLiteral("ok"))) {
            const QJsonObject r = o;
            buf_.clear();
            finished(r);
            return;
        }
    }
}

void CoolingPage::finished(const QJsonObject &o) {
    QProcess *p = proc_;
    proc_ = nullptr;
    if (p) {
        p->disconnect(this);
        // The result line can arrive a moment before the process has exited.
        if (p->state() == QProcess::NotRunning) p->deleteLater();
        else connect(p, &QProcess::finished, p, &QObject::deleteLater);
    }
    setRunning(false);
    live_->clear();
    if (!o.value(QStringLiteral("ok")).toBool()) {
        // Show the reason after the status refresh, which would otherwise hide the banner again.
        const QString err = o.value(QStringLiteral("error")).toString(tr("The test failed."));
        tool({QStringLiteral("status")}, [this, err](const QJsonObject &s) { fillStatus(s); showVerdict(err, theme::WARN); });
        return;
    }
    // Table and verdict come from the stored runs (the tool's own answer carries the same assessment).
    refresh();
}
