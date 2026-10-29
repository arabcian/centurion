#include "dgpuusage.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QPointer>
#include <QSaveFile>
#include <QThreadPool>

#include <algorithm>
#include <cmath>

static constexpr int SAVE_EVERY = 10;            // samples (about ten minutes)
static constexpr int MAX_DAYS = 400;
static constexpr int MAX_WAKES = 200;
static constexpr qint64 MAX_GAP_MS = 10 * 60 * 1000;  // a longer gap between samples is not counted
static constexpr qint64 SUSPEND_SLACK_MS = 5000;      // wall clock ahead of the monotonic one = the machine slept
static constexpr double MAX_INTERVAL_WH = 50;         // a bigger energy step is a counter reset, not a draw
static constexpr qint64 LONG_AWAKE_MS = 10 * 60 * 1000;  // awake on battery this long without a break: warn once

static QString rd(const QString &p) {
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.read(64)).trimmed() : QString();
}

QString DgpuUsage::file() {
    QString dir = qEnvironmentVariable("XDG_STATE_HOME");
    if (dir.isEmpty()) dir = QDir::homePath() + QStringLiteral("/.local/state");
    return dir + QStringLiteral("/centurion/dgpu.json");
}

DgpuUsage::DgpuUsage(const QString &dev, std::function<QString(const QString &)> holders, QObject *parent)
    : QObject(parent), dev_(dev), holders_(std::move(holders)) {
    if (dev_.isEmpty()) return;
    QFile f(file());
    if (f.open(QIODevice::ReadOnly) && f.size() < (1 << 20)) data_ = QJsonDocument::fromJson(f.readAll()).object();
}

DgpuUsage::~DgpuUsage() { if (dirty_) save(); }

void DgpuUsage::sample(bool onBattery, double energyWh, qint64 wall, qint64 mono) {
    if (dev_.isEmpty()) return;
    bool okA = false, okS = false;
    const qint64 active = rd(dev_ + QStringLiteral("/power/runtime_active_time")).toLongLong(&okA);
    const qint64 suspended = rd(dev_ + QStringLiteral("/power/runtime_suspended_time")).toLongLong(&okS);
    const QString status = rd(dev_ + QStringLiteral("/power/runtime_status"));
    if (!okA || !okS) return;

    // The counters only grow. An interval that spans a system suspend is not counted (wall clock ran ahead).
    const qint64 dMono = mono - prevMono_, dWall = wall - prevWall_;
    if (havePrev_ && dMono > 0 && dMono <= MAX_GAP_MS && std::abs(dWall - dMono) < SUSPEND_SLACK_MS) {
        const qint64 dA = active - prevActive_, dS = suspended - prevSuspended_;
        if (dA >= 0 && dS >= 0) {
            const QString day = QDateTime::fromMSecsSinceEpoch(wall).date().toString(Qt::ISODate);
            QJsonObject days = data_[QStringLiteral("days")].toObject();
            QJsonObject d = days[day].toObject();
            QJsonObject side = d[onBattery ? QStringLiteral("bat") : QStringLiteral("ac")].toObject();
            side[QStringLiteral("awake_ms")] = side[QStringLiteral("awake_ms")].toDouble() + double(dA);
            side[QStringLiteral("asleep_ms")] = side[QStringLiteral("asleep_ms")].toDouble() + double(dS);
            d[onBattery ? QStringLiteral("bat") : QStringLiteral("ac")] = side;
            days[day] = d;
            while (days.size() > MAX_DAYS) days.remove(days.keys().first());
            data_[QStringLiteral("days")] = days;

            // Energy over the interval, when on battery at both ends: split by what the dGPU mostly did.
            const double dWh = prevWh_ - energyWh;
            if (prevBattery_ && onBattery && prevWh_ >= 0 && energyWh >= 0 && dWh >= 0 && dWh < MAX_INTERVAL_WH) {
                QJsonObject power = data_[QStringLiteral("power")].toObject();
                const QString key = dA > dS ? QStringLiteral("awake") : QStringLiteral("asleep");
                QJsonObject bucket = power[key].toObject();
                bucket[QStringLiteral("sec")] = bucket[QStringLiteral("sec")].toDouble() + double(dMono) / 1000.0;
                bucket[QStringLiteral("wh")] = bucket[QStringLiteral("wh")].toDouble() + dWh;
                power[key] = bucket;
                data_[QStringLiteral("power")] = power;
            }
            ++dirty_;
        }
    }
    if (havePrev_ && status == QLatin1String("active") && prevStatus_ != QLatin1String("active")) onWake(wall / 1000);

    // A stretch awake on battery without a break; one warning per stretch once it has lasted LONG_AWAKE_MS.
    if (status != QLatin1String("active") || !onBattery) { streakStart_ = 0; warned_ = false; }
    else if (streakStart_ == 0) streakStart_ = wall;
    if (streakStart_ && !warned_ && wall - streakStart_ >= LONG_AWAKE_MS) {
        warned_ = true;
        Q_EMIT longAwake((wall - streakStart_) / 60000, holdersOfWake(streakStart_ / 1000));
    }

    prevActive_ = active;
    prevSuspended_ = suspended;
    prevStatus_ = status;
    prevBattery_ = onBattery;
    prevWh_ = energyWh;
    prevWall_ = wall;
    prevMono_ = mono;
    havePrev_ = true;
    if (dirty_ >= SAVE_EVERY) save();
}

void DgpuUsage::onWake(qint64 unixSec) {
    QJsonArray wakes = data_[QStringLiteral("wakes")].toArray();
    wakes.append(QJsonObject{{QStringLiteral("t"), unixSec}, {QStringLiteral("holders"), QString()}});
    while (wakes.size() > MAX_WAKES) wakes.removeFirst();
    data_[QStringLiteral("wakes")] = wakes;
    save();

    // The scan reads /proc on a worker thread (the GUI never waits for it); the answer is stored when it is in.
    QPointer<DgpuUsage> self(this);
    const auto scan = holders_;
    const QString dev = dev_;
    QThreadPool::globalInstance()->start([self, scan, dev, unixSec] {
        const QString who = scan ? scan(dev) : QString();
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, unixSec, who] {
            if (self) self->setHolders(unixSec, who);
        }, Qt::QueuedConnection);
    });
}

QString DgpuUsage::holdersOfWake(qint64 unixSec) const {
    const QJsonArray wakes = data_[QStringLiteral("wakes")].toArray();
    if (wakes.isEmpty()) return {};
    const QJsonObject w = wakes.last().toObject();
    return w[QStringLiteral("t")].toInteger() == unixSec ? w[QStringLiteral("holders")].toString() : QString();
}

void DgpuUsage::setHolders(qint64 unixSec, const QString &who) {
    QJsonArray wakes = data_[QStringLiteral("wakes")].toArray();
    for (int i = wakes.size() - 1; i >= 0; --i) {
        QJsonObject w = wakes[i].toObject();
        if (w[QStringLiteral("t")].toInteger() != unixSec) continue;
        w[QStringLiteral("holders")] = who;
        wakes[i] = w;
        break;
    }
    data_[QStringLiteral("wakes")] = wakes;
    save();
}

void DgpuUsage::save() {
    dirty_ = 0;
    data_[QStringLiteral("version")] = 1;
    QDir().mkpath(QFileInfo(file()).absolutePath());
    QSaveFile f(file());
    if (f.open(QIODevice::WriteOnly)) { f.write(QJsonDocument(data_).toJson(QJsonDocument::Compact)); f.commit(); }
}
