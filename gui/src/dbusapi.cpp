#include "dbusapi.h"
#include "healthtab.h"
#include "hometab.h"
#include "mainwindow.h"
#include "platformprofile.h"
#include "scenes.h"
#include "sysinfo.h"
#include "telemetryservice.h"
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMetaType>
#include <QCoreApplication>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>
#include <utility>
#include <cstdio>

static const char *SERVICE = "com.centurion.Centurion";
static const char *PATH = "/com/centurion/Centurion";

DbusApi::DbusApi(MainWindow *win) : QObject(win), win_(win) {
    qDBusRegisterMetaType<QMap<QString, QString>>();
    qDBusRegisterMetaType<QMap<QString, double>>();
    qDBusRegisterMetaType<QList<QVariantMap>>();
    connect(win_->scenes(), &SceneEngine::finished, this, [this](const QString &n, bool ok, const QStringList &) {
        if (!n.isEmpty()) Q_EMIT SceneApplied(n, ok);  // empty name: an informational note, not an apply
    });
    connect(&pp::Watcher::instance(), &pp::Watcher::changed, this, &DbusApi::PowerProfileChanged);
    connect(win_, &MainWindow::safeModeFinished, this, &DbusApi::SafeModeFinished);
    connect(win_->health(), &HealthTab::fault, this, [this](const QJsonObject &e) {
        Q_EMIT HealthEvent(e.value(QStringLiteral("kind")).toString(), e.value(QStringLiteral("level")).toString(), e.value(QStringLiteral("title")).toString());
    });
}

bool DbusApi::registerOnBus() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || !bus.registerService(QLatin1String(SERVICE))
        || !bus.registerObject(QLatin1String(PATH), this, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals)) {
        std::fprintf(stderr, "centurion: D-Bus API not available (%s)\n", qPrintable(bus.lastError().message()));
        return false;
    }
    return true;
}

void DbusApi::SafeMode() { win_->runSafeMode(); }

QStringList DbusApi::ListScenes() { return scenes::names(); }

QString DbusApi::ActiveScene() { return win_->scenes()->active(); }

void DbusApi::ApplyScene(const QString &name) {
    if (!scenes::names().contains(name)) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("no scene named '%1'").arg(name));
        return;
    }
    win_->scenes()->apply(name);
}

QStringList DbusApi::PowerProfiles() { return win_->home()->offeredProfiles(); }

QString DbusApi::PowerProfile() { return pp::Watcher::instance().current().value_or(QString()); }

void DbusApi::SetPowerProfile(const QString &profile) {
    if (!win_->home()->offeredProfiles().contains(profile)) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("profile '%1' is not offered (see PowerProfiles)").arg(profile));
        return;
    }
    win_->home()->applyProfile(profile);
}

void DbusApi::enqueueSensors(bool numeric) {
    const auto msg = message();
    const auto bus = connection();
    setDelayedReply(true);
    if (sensorExpired_ || sensorRequests_.size() >= 32) {
        bus.send(msg.createErrorReply(QDBusError::LimitsExceeded, QStringLiteral("sensor acquisition busy")));
        return;
    }
    sensorRequests_.append(SensorRequest{bus, msg, numeric});
    if (sensorPending_) return;
    sensorPending_ = true;
    const auto generation = ++sensorGeneration_;
    QTimer::singleShot(5000, this, [this, generation] {
        if (!sensorPending_ || generation != sensorGeneration_) return;
        sensorExpired_ = true;
        const auto requests = std::exchange(sensorRequests_, QList<SensorRequest>{});
        for (const auto &req : requests)
            req.connection.send(req.message.createErrorReply(QDBusError::Timeout, QStringLiteral("sensor acquisition timed out")));
        // Do not start another worker while the slow sysfs read still runs.
    });
    using M = sysinfo::PresentationMetric;
    TelemetryService::instance().request({M::CpuTemp,M::Fans,M::Storage,M::Power,M::Igpu,M::Battery}, this,
        [this,generation](TelemetryService::Results results) {
            if (generation != sensorGeneration_) return;
            sensorPending_ = false; sensorExpired_ = false;
            const auto requests = std::exchange(sensorRequests_, QList<SensorRequest>{});
            sysinfo::PresentationSnapshot snapshot;
            const QStringList names{QStringLiteral("cpu"),QStringLiteral("fans"),QStringLiteral("storage"),QStringLiteral("power"),QStringLiteral("igpu"),QStringLiteral("battery")};
            bool timedOut = false;
            for (int i=0;i<names.size();++i) {
                auto reading = results.value(i);
                timedOut |= reading.quality == telemetry::Quality::TimedOut || reading.quality == telemetry::Quality::Pending;
                snapshot.readings.append({names.at(i),reading});
            }
            for (const auto &req : requests) {
                if (timedOut) { req.connection.send(req.message.createErrorReply(QDBusError::Timeout,QStringLiteral("sensor acquisition incomplete"))); continue; }
                const QVariant result = req.numeric ? QVariant::fromValue(snapshot.values()) : QVariant::fromValue(snapshot.text());
                req.connection.send(req.message.createReply(QVariantList{result}));
            }
        });
}
QMap<QString, QString> DbusApi::Sensors() {
    if (calledFromDBus()) { enqueueSensors(false); return {}; }
    return sysinfo::presentationSnapshot().text();
}
QMap<QString, double> DbusApi::SensorValues() {
    if (calledFromDBus()) { enqueueSensors(true); return {}; }
    return sysinfo::sensorValues();
}

QVariantMap DbusApi::HealthSummary() {
    QVariantMap m;
    m.insert(QStringLiteral("worst"), win_->health()->worstLevel());
    m.insert(QStringLiteral("cards"), QVariant::fromValue(win_->health()->overviewCards()));
    return m;
}

QList<QVariantMap> DbusApi::HealthEvents(uint sinceSeq) { return win_->health()->eventsSince(sinceSeq); }
