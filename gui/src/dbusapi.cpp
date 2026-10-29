#include "dbusapi.h"
#include "healthtab.h"
#include "hometab.h"
#include "mainwindow.h"
#include "platformprofile.h"
#include "scenes.h"
#include "sysinfo.h"
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMetaType>
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

QMap<QString, QString> DbusApi::Sensors() {
    QMap<QString, QString> m;
    for (const auto &[key, value] : {std::pair{"cpu", sysinfo::cpuTemp()}, {"fans", sysinfo::fans()}, {"storage", sysinfo::storage()},
                                     {"power", sysinfo::power()}, {"igpu", sysinfo::igpu()}, {"battery", sysinfo::battery()}})
        if (value && !value->isEmpty()) m.insert(QLatin1String(key), *value);
    return m;
}

QMap<QString, double> DbusApi::SensorValues() { return sysinfo::sensorValues(); }

QVariantMap DbusApi::HealthSummary() {
    QVariantMap m;
    m.insert(QStringLiteral("worst"), win_->health()->worstLevel());
    m.insert(QStringLiteral("cards"), QVariant::fromValue(win_->health()->overviewCards()));
    return m;
}

QList<QVariantMap> DbusApi::HealthEvents(uint sinceSeq) { return win_->health()->eventsSince(sinceSeq); }
