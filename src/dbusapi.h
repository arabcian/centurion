#pragma once
// Session-bus API of the running Centurion (tray) instance, for scripts, KDE
// Connect, Home Assistant, Stream Deck and the like:
//
//   service   com.centurion.Centurion
//   object    /com/centurion/Centurion
//   interface com.centurion.Centurion
//
//   ListScenes()            → as      saved scene names
//   ActiveScene()           → s       "" when none
//   ApplyScene(s name)                queued like a tray click; result in SceneApplied
//   PowerProfiles()         → as      profiles the firmware offers
//   PowerProfile()          → s
//   SetPowerProfile(s)                same path as the Home cards / tray menu
//   Sensors()               → a{ss}   the Home tab's Live rows (cpu, fans, storage, power, igpu, battery);
//                                     sysfs only, never wakes the dGPU
//   SensorValues()          → a{sd}   the same as numbers, unit in the key (Home Assistant & co.):
//                                     cpu_temp_c, fanN_rpm, nvmeN_temp_c, power_<hwmon>_w, igpu_temp_c,
//                                     battery_percent, battery_power_w, battery_health_percent
//   signal SceneApplied(s name, b ok)
//   signal PowerProfileChanged(s profile)
//
// Same trust as `centurion-gamemode SCENE`: any process of this user may call
// it; root work still goes through the helpers and their polkit rules.
#include <QDBusContext>
#include <QObject>
#include <QStringList>
#include <QMap>

class MainWindow;

class DbusApi : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.centurion.Centurion")
public:
    explicit DbusApi(MainWindow *win);
    /// Takes the bus name and exports the object; false (and a line on stderr) when the bus is unavailable.
    bool registerOnBus();

public Q_SLOTS:
    QStringList ListScenes();
    QString ActiveScene();
    void ApplyScene(const QString &name);
    QStringList PowerProfiles();
    QString PowerProfile();
    void SetPowerProfile(const QString &profile);
    QMap<QString, QString> Sensors();
    QMap<QString, double> SensorValues();

Q_SIGNALS:
    void SceneApplied(const QString &name, bool ok);
    void PowerProfileChanged(const QString &profile);

private:
    MainWindow *win_;
};
