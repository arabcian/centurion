#include "batterypage.h"
#include "scenes.h"
#include "sysinfo.h"
#include "theme.h"
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

QString BatteryPage::file() {
    QString dir = qEnvironmentVariable("XDG_STATE_HOME");
    if (dir.isEmpty()) dir = QDir::homePath() + QStringLiteral("/.local/state");
    return dir + QStringLiteral("/centurion/battery.json");
}

bool BatteryPage::present() { return sysinfo::batteryState().present; }

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
    root->addWidget(muted(QStringLiteral("Energy actually taken from the battery while each scene was active (energy counter differences, "
                                         "sleep excluded). \"Full charge lasts\" = today's full capacity ÷ the average draw.")));

    root->addWidget(new QLabel(QStringLiteral("<b>Capacity history</b>")));
    wear_ = new QLabel;
    wear_->setTextFormat(Qt::RichText);
    root->addWidget(wear_);
    capacity_ = table({QStringLiteral("Date"), QStringLiteral("Full charge"), QStringLiteral("Health"), QStringLiteral("Cycles")});
    root->addWidget(capacity_, 1);

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
    nowTimer_->start();
}

void BatteryPage::hideEvent(QHideEvent *e) {
    QWidget::hideEvent(e);
    nowTimer_->stop();
}

void BatteryPage::tick() {
    const sysinfo::BatteryState b = sysinfo::batteryState();
    // Once a day: capacity sample (also catches a GUI that runs for weeks).
    const QJsonArray cap = data_.value(QStringLiteral("capacity")).toArray();
    if (cap.isEmpty() || cap.last().toObject().value(QStringLiteral("d")).toString() != QDate::currentDate().toString(Qt::ISODate)) sampleCapacity();
    const bool dis = b.present && b.status == QLatin1String("Discharging") && b.energyWh >= 0;
    if (!dis) { prevValid_ = false; return; }
    const qint64 wall = QDateTime::currentMSecsSinceEpoch(), mono = mono_.elapsed();
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

void BatteryPage::refreshTables() {
    const sysinfo::BatteryState b = sysinfo::batteryState();
    scenes_->clear();
    const QJsonObject sc = data_.value(QStringLiteral("scenes")).toObject();
    QStringList order = sc.keys();
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
    }
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
}
