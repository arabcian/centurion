#include "batterypage.h"
#include "batteryadvice.h"
#include "charts.h"
#include "dgpupage.h"
#include "dgpuusage.h"
#include "privileged.h"
#include "scenes.h"
#include "sysinfo.h"
#include "theme.h"
#include "userhelper.h"
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

static constexpr int TICK_MS = 60000, NOW_MS = 5000;
static constexpr qint64 MAX_GAP_MS = 10 * 60 * 1000;  // a longer gap between samples is not counted
static constexpr qint64 SUSPEND_SLACK_MS = 5000;      // wall clock ahead of the monotonic one = the machine slept
static constexpr int MAX_DAYS = 730;
static constexpr int SAVE_EVERY = 10;                 // ticks
static constexpr int FULL_PERCENT = 98;               // "at full charge" for the charger history
static constexpr double MIN_DRAIN_SEC = 60;           // shorter drain stretches on the charger are noise
static constexpr int MAX_DRAINS = 100;

QString BatteryPage::file() {
    QString dir = qEnvironmentVariable("XDG_STATE_HOME");
    if (dir.isEmpty()) dir = QDir::homePath() + QStringLiteral("/.local/state");
    return dir + QStringLiteral("/centurion/battery.json");
}

bool BatteryPage::present() { return sysinfo::batteryPresent(); }

static QLabel *muted(const QString &t) {
    auto *l = new QLabel(t);
    l->setWordWrap(true);
    l->setStyleSheet(QStringLiteral("color:%1").arg(theme::MUTED));
    return l;
}

static QTreeWidget *table(const QStringList &head) {
    auto *t = new QTreeWidget;
    t->setColumnCount(head.size());
    t->setHeaderLabels(head);
    t->setRootIsDecorated(false);
    t->setAlternatingRowColors(true);
    for (int i = 0; i < head.size() - 1; ++i) t->header()->setSectionResizeMode(i, QHeaderView::ResizeToContents);
    t->header()->setStretchLastSection(true);
    return t;
}

BatteryPage::BatteryPage(QWidget *parent) : QWidget(parent) {
    mono_.start();
    chargeFile_ = chargeFile();
    if (const QString dev = DgpuPage::devicePath(); !dev.isEmpty()) {
        usage_ = new DgpuUsage(dev, &DgpuPage::holdersAt, this);
        connect(usage_, &DgpuUsage::longAwake, this, &BatteryPage::dgpuAwake);
    }
    {
        QFile f(file());
        if (f.open(QIODevice::ReadOnly) && f.size() < (1 << 20)) data_ = QJsonDocument::fromJson(f.readAll()).object();
    }
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(6);
    now_ = new QLabel;
    now_->setTextFormat(Qt::RichText);
    root->addWidget(now_);

    root->addWidget(new QLabel(QStringLiteral("<b>On battery, per scene</b>")));
    scenes_ = table({QStringLiteral("Scene"), QStringLiteral("Time on battery"), QStringLiteral("Average draw"), QStringLiteral("Full charge lasts ~")});
    root->addWidget(scenes_, 1);
    scenesChart_ = new BarChart;
    root->addWidget(scenesChart_);
    root->addWidget(muted(QStringLiteral("Energy actually taken from the battery while each scene was active (energy counter differences, "
                                         "sleep excluded). \"Full charge lasts\" = today's full capacity ÷ the average draw.")));

    root->addWidget(new QLabel(QStringLiteral("<b>Capacity history</b>")));
    batInfo_ = new QLabel;
    batInfo_->setTextFormat(Qt::RichText);
    batInfo_->setVisible(false);
    root->addWidget(batInfo_);
    wear_ = new QLabel;
    wear_->setTextFormat(Qt::RichText);
    root->addWidget(wear_);
    forecast_ = new QLabel;
    forecast_->setWordWrap(true);
    root->addWidget(forecast_);
    healthChart_ = new LineChart;
    root->addWidget(healthChart_);
    capacity_ = table({QStringLiteral("Date"), QStringLiteral("Full charge"), QStringLiteral("Health"), QStringLiteral("Cycles")});
    root->addWidget(capacity_, 1);

    root->addWidget(new QLabel(tr("<b>Charger use</b>")));
    advice_ = new QLabel;
    advice_->setWordWrap(true);
    root->addWidget(advice_);
    longLife_ = new QPushButton(tr("Set charge mode to Long_Life…"));
    longLife_->setVisible(false);
    connect(longLife_, &QPushButton::clicked, this, &BatteryPage::setLongLife);
    root->addWidget(longLife_, 0, Qt::AlignLeft);
    drains_ = table({tr("Started"), tr("Length"), tr("Battery lost"), tr("Scene")});
    root->addWidget(drains_, 1);
    root->addWidget(muted(tr("Stretches on the charger while the battery still drained: the adapter could not carry the load "
                             "(a heavy scene on a small brick). The advice counts the last %1 days.").arg(batteryadvice::WINDOW_DAYS)));

    auto *row = new QHBoxLayout;
    row->addStretch(1);
    auto *reset = new QPushButton(QStringLiteral("Reset scene statistics"));
    connect(reset, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, QStringLiteral("Battery"), QStringLiteral("Forget the per-scene battery statistics? The capacity history is kept.")) != QMessageBox::Yes) return;
        data_.remove(QStringLiteral("scenes"));
        prevValid_ = false;
        save();
        refreshTables();
    });
    row->addWidget(reset);
    root->addLayout(row);

    timer_ = new QTimer(this);
    timer_->setTimerType(Qt::VeryCoarseTimer);
    connect(timer_, &QTimer::timeout, this, &BatteryPage::tick);
    nowTimer_ = new QTimer(this);
    nowTimer_->setInterval(NOW_MS);
    connect(nowTimer_, &QTimer::timeout, this, &BatteryPage::refreshNow);
    if (!qEnvironmentVariableIsSet("CENTURION_PGO_TRAIN")) {
        timer_->start(TICK_MS);
        QTimer::singleShot(20000, this, [this] { sampleCapacity(); tick(); });
    }
}

BatteryPage::~BatteryPage() { if (dirty_) save(); }

void BatteryPage::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    refreshNow();
    refreshTables();
    refreshForecast();
    if (!infoAsked_) refreshInfo();
    nowTimer_->start();
}

// Maker and manufacture date from the firmware (health-reader "battery_info"); static, asked once.
// With the date: the battery's age and the capacity lost per year so far (today's full vs design).
void BatteryPage::refreshInfo() {
    infoAsked_ = true;
    const QString helper = privileged::helperPath(QStringLiteral("health-reader"));
    if (!QFileInfo(helper).isExecutable()) return;
    privileged::run(helper, QJsonObject{{QStringLiteral("op"), QStringLiteral("battery_info")}}, this, [this](const privileged::Result &r) {
        if (!r.reached || !r.json.value(QStringLiteral("ok")).toBool()) return;  // unsupported or too old a helper: no line
        QStringList p;
        if (const QString maker = r.json.value(QStringLiteral("maker")).toString(); !maker.isEmpty())
            p << tr("Battery by <b>%1</b>").arg(maker.toHtmlEscaped());
        const QDate made = QDate::fromString(r.json.value(QStringLiteral("mfg_date")).toString(), Qt::ISODate);
        const QDate today = QDate::currentDate();
        if (made.isValid() && made <= today) {
            const qint64 days = made.daysTo(today);
            p << tr("made %1 (%2 months ago)").arg(made.toString(Qt::ISODate)).arg(days / 30);
            const sysinfo::BatteryState b = sysinfo::batteryState();
            // Under three months the yearly figure is mostly the counter's noise.
            if (days >= 90 && b.fullWh > 0 && b.designWh > 0) {
                const double lost = std::max(0.0, 100.0 - b.fullWh / b.designWh * 100.0);
                p << tr("~%1 % of design capacity lost per year").arg(lost / (days / 365.25), 0, 'f', 1);
            }
        }
        if (p.isEmpty()) return;
        batInfo_->setText(p.join(QStringLiteral(" &nbsp;·&nbsp; ")));
        batInfo_->setVisible(true);
    });
}

void BatteryPage::hideEvent(QHideEvent *e) {
    QWidget::hideEvent(e);
    nowTimer_->stop();
}

void BatteryPage::tick() {
    const sysinfo::BatteryState b = sysinfo::batteryState();
    if (usage_) usage_->sample(b.present && b.status == QLatin1String("Discharging"), b.energyWh,
                               QDateTime::currentMSecsSinceEpoch(), mono_.elapsed());
    // Once a day: capacity sample (also catches a GUI that runs for weeks).
    const QJsonArray cap = data_.value(QStringLiteral("capacity")).toArray();
    if (cap.isEmpty() || cap.last().toObject().value(QStringLiteral("d")).toString() != QDate::currentDate().toString(Qt::ISODate)) sampleCapacity();

    // Charger history: time on the charger and at full charge, and the stretches where the charger was plugged in
    // but the battery still drained. Same gap rule as the scene statistics: a suspend or a long pause ends a stretch.
    const bool onAc = scenes::onAc(true).value_or(false);
    const qint64 wall = QDateTime::currentMSecsSinceEpoch(), mono = mono_.elapsed();
    const qint64 acDMono = mono - acMono_, acDWall = wall - acWall_;
    const bool steady = acValid_ && acDMono > 0 && acDMono <= MAX_GAP_MS && std::abs(acDWall - acDMono) < SUSPEND_SLACK_MS;
    if (onAc && steady) accountCharger(acDMono / 1000.0, b.capacity >= FULL_PERCENT);
    const bool draining = onAc && b.present && b.status == QLatin1String("Discharging") && b.energyWh >= 0;
    if (!draining || !steady) closeDrain();
    if (draining) {
        if (!drain_.open) {
            drain_ = Drain{true, wall, 0, 0, b.energyWh, scenes::activeScene()};
            if (drain_.scene.isEmpty()) drain_.scene = QStringLiteral("(no scene)");
        } else {
            const double dWh = drain_.prevWh - b.energyWh;
            if (dWh >= 0 && dWh < 50) drain_.wh += dWh;
            drain_.sec += acDMono / 1000.0;
            drain_.prevWh = b.energyWh;
        }
    }
    acValid_ = onAc;
    acWall_ = wall;
    acMono_ = mono;

    const bool dis = b.present && b.status == QLatin1String("Discharging") && b.energyWh >= 0;
    if (!dis) { prevValid_ = false; return; }
    QString scene = scenes::activeScene();
    if (scene.isEmpty()) scene = QStringLiteral("(no scene)");
    if (prevValid_) {
        const qint64 dMono = mono - prevMono_, dWall = wall - prevWall_;
        const double dWh = prevWh_ - b.energyWh;
        if (dMono > 0 && dMono <= MAX_GAP_MS && std::abs(dWall - dMono) < SUSPEND_SLACK_MS && dWh >= 0 && dWh < 50) {
            // Charged to the scene that was active over the interval (the earlier one on a switch).
            QJsonObject sc = data_.value(QStringLiteral("scenes")).toObject();
            QJsonObject e = sc.value(prevScene_).toObject();
            e[QStringLiteral("sec")] = e.value(QStringLiteral("sec")).toDouble() + dMono / 1000.0;
            e[QStringLiteral("wh")] = e.value(QStringLiteral("wh")).toDouble() + dWh;
            sc[prevScene_] = e;
            data_[QStringLiteral("scenes")] = sc;
            if (++dirty_ >= SAVE_EVERY) save();
        }
    }
    prevValid_ = true;
    prevWh_ = b.energyWh;
    prevWall_ = wall;
    prevMono_ = mono;
    prevScene_ = scene;
}

void BatteryPage::sampleCapacity() {
    const sysinfo::BatteryState b = sysinfo::batteryState();
    if (!b.present || b.fullWh <= 0) return;
    QJsonArray cap = data_.value(QStringLiteral("capacity")).toArray();
    const QString today = QDate::currentDate().toString(Qt::ISODate);
    QJsonObject e{{"d", today}, {"full", std::round(b.fullWh * 100) / 100}};
    if (b.designWh > 0) e["design"] = std::round(b.designWh * 100) / 100;
    if (b.cycles >= 0) e["cycles"] = b.cycles;
    if (!cap.isEmpty() && cap.last().toObject().value(QStringLiteral("d")).toString() == today) cap.removeLast();
    cap.append(e);
    while (cap.size() > MAX_DAYS) cap.removeFirst();
    data_[QStringLiteral("capacity")] = cap;
    save();
}

void BatteryPage::accountCharger(double sec, bool full) {
    const QDate today = QDate::currentDate();
    const QString key = today.toString(Qt::ISODate);
    QJsonObject days = data_.value(QStringLiteral("ac_days")).toObject();
    QJsonObject d = days.value(key).toObject();
    d[QStringLiteral("ac")] = d.value(QStringLiteral("ac")).toDouble() + sec;
    if (full) d[QStringLiteral("full")] = d.value(QStringLiteral("full")).toDouble() + sec;
    days[key] = d;
    data_[QStringLiteral("ac_days")] = batteryadvice::pruned(days, today, batteryadvice::KEEP_DAYS);
    ++dirty_;
}

void BatteryPage::closeDrain() {
    if (!drain_.open) return;
    drain_.open = false;
    if (drain_.sec < MIN_DRAIN_SEC) return;
    QJsonArray a = data_.value(QStringLiteral("ac_drain")).toArray();
    a.append(QJsonObject{{"t", double(drain_.start)}, {"sec", std::round(drain_.sec)},
                         {"wh", std::round(drain_.wh * 100) / 100}, {"scene", drain_.scene}});
    while (a.size() > MAX_DRAINS) a.removeFirst();
    data_[QStringLiteral("ac_drain")] = a;
    save();
}

static batteryadvice::ChargeModes readChargeModes(const QString &file) {
    QFile f(file);
    return f.open(QIODevice::ReadOnly) ? batteryadvice::parseChargeTypes(QString::fromLatin1(f.readAll())) : batteryadvice::ChargeModes{};
}

QString BatteryPage::chargeFile() {
    const QDir root(QStringLiteral("/sys/class/power_supply"));
    for (const QString &name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString dir = root.filePath(name);
        QFile type(dir + QStringLiteral("/type"));
        if (!type.open(QIODevice::ReadOnly) || QString::fromLatin1(type.readAll()).trimmed() != QLatin1String("Battery")) continue;
        if (QFileInfo(dir + QStringLiteral("/charge_types")).isFile()) return dir + QStringLiteral("/charge_types");
    }
    return {};
}

void BatteryPage::setLongLife() {
    if (chargeFile_.isEmpty()) return;
    if (QMessageBox::question(this, tr("Charge mode"),
                              tr("Set the battery charge mode to Long_Life? Charging then stops around 80 %. Asks for your password.")) != QMessageBox::Yes)
        return;
    privileged::run(privileged::helperPath(QStringLiteral("legion-profile-helper")),
                    QJsonObject{{"device", "charge_type"}, {"value", "Long_Life"}}, this, [this](const privileged::Result &r) {
                        if (!r.ok()) { advice_->setText(tr("Could not change the charge mode: %1").arg(r.message())); return; }
                        refreshTables();
                    });
}

void BatteryPage::save() {
    dirty_ = 0;
    QDir().mkpath(QFileInfo(file()).absolutePath());
    QSaveFile f(file());
    if (f.open(QIODevice::WriteOnly)) { f.write(QJsonDocument(data_).toJson(QJsonDocument::Compact)); f.commit(); }
}

void BatteryPage::refreshNow() {
    const sysinfo::BatteryState b = sysinfo::batteryState();
    if (!b.present) { now_->setText(QStringLiteral("No battery reported.")); return; }
    QStringList p;
    if (b.capacity >= 0) p << QStringLiteral("<b>%1%</b>").arg(b.capacity);
    if (!b.status.isEmpty()) p << b.status;
    if (b.powerW >= 0.1 && (b.status == QLatin1String("Discharging") || b.status == QLatin1String("Charging")))
        p << QStringLiteral("%1 W").arg(b.powerW, 0, 'f', 1);
    if (const int m = b.minutesLeft(); m >= 0 && m < 48 * 60)
        p << (b.status == QLatin1String("Charging") ? QStringLiteral("full in ~<b>%1</b>") : QStringLiteral("~<b>%1</b> left at this draw")).arg(sysinfo::formatMinutes(m));
    if (b.energyWh >= 0 && b.fullWh > 0) p << QStringLiteral("%1 / %2 Wh").arg(b.energyWh, 0, 'f', 1).arg(b.fullWh, 0, 'f', 1);
    now_->setText(QStringLiteral("Now: ") + p.join(QStringLiteral(" &nbsp;·&nbsp; ")));
}

void BatteryPage::refreshForecast() {
    userhelper::run(QJsonObject{{QStringLiteral("op"), QStringLiteral("battery_trend")}}, this, [this](const QJsonObject &r) {
        const QString status = r.value(QStringLiteral("status")).toString();
        const double health = r.value(QStringLiteral("health")).toDouble(), months = r.value(QStringLiteral("months_to_80")).toDouble();
        if (!r.value(QStringLiteral("ok")).toBool()) forecast_->clear();
        else if (status == QLatin1String("insufficient")) forecast_->setText(tr("Wear forecast after 30 daily samples within 180 days (%1 so far).").arg(r.value(QStringLiteral("samples")).toInt()));
        else if (status == QLatin1String("no_decline")) forecast_->setText(tr("Wear forecast: health is not falling yet, so there is no trend to project."));
        else if (status == QLatin1String("at_or_below")) forecast_->setText(tr("Health is already at or below 80 % of design (now %1 %).").arg(health, 0, 'f', 1));
        else forecast_->setText(tr("Wear forecast: at this rate health reaches 80 % of design in about %1 months (now %2 %).").arg(months, 0, 'f', 0).arg(health, 0, 'f', 1));
    });
}

void BatteryPage::refreshTables() {
    const sysinfo::BatteryState b = sysinfo::batteryState();
    scenes_->clear();
    const QJsonObject sc = data_.value(QStringLiteral("scenes")).toObject();
    QStringList order = sc.keys();
    QVector<BarChart::Bar> bars;
    std::sort(order.begin(), order.end(), [&sc](const QString &a, const QString &b) {
        return sc.value(a).toObject().value(QStringLiteral("sec")).toDouble() > sc.value(b).toObject().value(QStringLiteral("sec")).toDouble();
    });
    for (const QString &name : order) {
        const QJsonObject o = sc.value(name).toObject();
        const double sec = o.value(QStringLiteral("sec")).toDouble(), wh = o.value(QStringLiteral("wh")).toDouble();
        if (sec < 60) continue;
        const double w = wh / (sec / 3600.0);
        const QString lasts = (w > 0.5 && b.fullWh > 0) ? sysinfo::formatMinutes(int(b.fullWh / w * 60)) : QStringLiteral("—");
        // Under ten minutes the energy counter's steps dominate: shown, but marked as rough.
        const QString avg = QStringLiteral("%1 W").arg(w, 0, 'f', 1) + (sec < 600 ? QStringLiteral("  (rough)") : QString());
        scenes_->addTopLevelItem(new QTreeWidgetItem({name, sysinfo::formatMinutes(int(sec / 60)), avg, lasts}));
        bars << BarChart::Bar{name, w, avg};
    }
    scenesChart_->setBars(bars);
    if (!scenes_->topLevelItemCount())
        scenes_->addTopLevelItem(new QTreeWidgetItem({QStringLiteral("No time on battery recorded yet."), QString(), QString(), QString()}));

    capacity_->clear();
    const QJsonArray cap = data_.value(QStringLiteral("capacity")).toArray();
    for (int i = cap.size() - 1; i >= 0; --i) {
        const QJsonObject e = cap.at(i).toObject();
        const double full = e.value(QStringLiteral("full")).toDouble(), design = e.value(QStringLiteral("design")).toDouble();
        capacity_->addTopLevelItem(new QTreeWidgetItem({e.value(QStringLiteral("d")).toString(), QStringLiteral("%1 Wh").arg(full, 0, 'f', 1),
                                                        design > 0 ? QStringLiteral("%1%").arg(100 * full / design, 0, 'f', 1) : QStringLiteral("—"),
                                                        e.contains(QStringLiteral("cycles")) ? QString::number(e.value(QStringLiteral("cycles")).toInt()) : QStringLiteral("—")}));
    }
    if (cap.size() >= 2) {
        const QJsonObject a = cap.first().toObject(), z = cap.last().toObject();
        const double fa = a.value(QStringLiteral("full")).toDouble(), fz = z.value(QStringLiteral("full")).toDouble();
        const qint64 days = QDate::fromString(a.value(QStringLiteral("d")).toString(), Qt::ISODate).daysTo(QDate::fromString(z.value(QStringLiteral("d")).toString(), Qt::ISODate));
        const double lost = fa > 0 ? 100 * (fa - fz) / fa : 0;
        wear_->setText(QStringLiteral("Since %1 (%2 days): <b>%3%</b> of the full charge %4.")
                           .arg(a.value(QStringLiteral("d")).toString()).arg(days).arg(std::abs(lost), 0, 'f', 1)
                           .arg(lost >= 0 ? QStringLiteral("lost") : QStringLiteral("regained (recalibration)")));
    } else {
        wear_->setText(QStringLiteral("<span style='color:%1'>One sample per day the app runs; the trend shows after the second day.</span>").arg(theme::MUTED));
    }
    QVector<LineChart::Point> health;
    for (const QJsonValue &v : cap) {
        const QJsonObject e = v.toObject();
        const double design = e.value(QStringLiteral("design")).toDouble();
        const QDate d = QDate::fromString(e.value(QStringLiteral("d")).toString(), Qt::ISODate);
        if (design > 0 && d.isValid()) health << LineChart::Point{double(d.toJulianDay()), 100 * e.value(QStringLiteral("full")).toDouble() / design, d.toString(Qt::ISODate)};
    }
    healthChart_->setPoints(health, QStringLiteral(" %"));

    // Charger use: the advice (only where the battery has a Long_Life mode to switch to) and the drain stretches.
    const batteryadvice::ChargeModes cm = readChargeModes(chargeFile_);
    const QJsonObject days = data_.value(QStringLiteral("ac_days")).toObject();
    const batteryadvice::Advice adv = batteryadvice::evaluate(days, QDate::currentDate(), cm.modes, cm.current);
    longLife_->setVisible(adv.show);
    if (adv.show) {
        advice_->setText(tr("On the charger %1 h in the last %2 days, %3 % of it at full charge. Long_Life stops charging around 80 %, "
                            "which spares the battery while it is mostly plugged in.")
                             .arg(adv.acHours, 0, 'f', 0).arg(batteryadvice::WINDOW_DAYS).arg(adv.fullShare * 100, 0, 'f', 0));
    } else {
        advice_->setText(tr("On the charger %1 h in the last %2 days.").arg(adv.acHours, 0, 'f', 0).arg(batteryadvice::WINDOW_DAYS));
    }

    drains_->clear();
    const QJsonArray dr = data_.value(QStringLiteral("ac_drain")).toArray();
    for (int i = dr.size() - 1; i >= 0; --i) {
        const QJsonObject o = dr.at(i).toObject();
        drains_->addTopLevelItem(new QTreeWidgetItem({QDateTime::fromMSecsSinceEpoch(qint64(o.value(QStringLiteral("t")).toDouble())).toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                                                      sysinfo::formatMinutes(int(o.value(QStringLiteral("sec")).toDouble() / 60)),
                                                      QStringLiteral("%1 Wh").arg(o.value(QStringLiteral("wh")).toDouble(), 0, 'f', 2),
                                                      o.value(QStringLiteral("scene")).toString()}));
    }
    if (!drains_->topLevelItemCount())
        drains_->addTopLevelItem(new QTreeWidgetItem({tr("None: the battery never drained on the charger."), QString(), QString(), QString()}));
}
