#include "lidwatch.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>

namespace {

// gui.ini, where the Sleep page keeps the limit.
QSettings guiSettings() {
    return QSettings(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/centurion/gui.ini"), QSettings::IniFormat);
}

constexpr char UPOWER[] = "org.freedesktop.UPower";
constexpr char UPOWER_PATH[] = "/org/freedesktop/UPower";
constexpr int DEFAULT_MINUTES = 10;

QString readTrim(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.readAll()).trimmed() : QString();
}

// A monitor plugged into a connector other than the panel's (eDP / LVDS / DSI): the lid is then a desk setup.
bool externalDisplay() {
    for (const QString &e : QDir(QStringLiteral("/sys/class/drm")).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const int dash = e.indexOf(QLatin1Char('-'));  // "card1-HDMI-A-1"
        if (dash < 0) continue;
        const QString conn = e.mid(dash + 1);
        if (conn.startsWith(QLatin1String("eDP")) || conn.startsWith(QLatin1String("LVDS")) || conn.startsWith(QLatin1String("DSI"))) continue;
        if (readTrim(QStringLiteral("/sys/class/drm/") + e + QStringLiteral("/status")) == QLatin1String("connected")) return true;
    }
    return false;
}

// The CPU die temperature (°C) and the fastest fan (rpm) from hwmon; 0 where there is no reading.
void cpuAndFans(double &cpuC, double &fanRpm) {
    cpuC = 0;
    fanRpm = 0;
    for (const QString &h : QDir(QStringLiteral("/sys/class/hwmon")).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString d = QStringLiteral("/sys/class/hwmon/") + h + QLatin1Char('/');
        const QString name = readTrim(d + QStringLiteral("name"));
        if (cpuC == 0 && (name == QLatin1String("k10temp") || name == QLatin1String("coretemp")))
            cpuC = readTrim(d + QStringLiteral("temp1_input")).toDouble() / 1000.0;
        for (int i = 1; i <= 8; ++i) fanRpm = std::max(fanRpm, readTrim(d + QStringLiteral("fan%1_input").arg(i)).toDouble());
    }
}

} // namespace

LidWatch::LidWatch(QObject *parent) : QObject(parent), timer_(new QTimer(this)) {
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, &LidWatch::fire);
    // The signal only reports changes: read the state once. Without UPower the property is invalid (= open) and nothing happens.
    QDBusInterface upower(QLatin1String(UPOWER), QLatin1String(UPOWER_PATH), QStringLiteral("org.freedesktop.UPower"), QDBusConnection::systemBus());
    setClosed(upower.property("LidIsClosed").toBool());
    QDBusConnection::systemBus().connect(QLatin1String(UPOWER), QLatin1String(UPOWER_PATH), QStringLiteral("org.freedesktop.DBus.Properties"),
                                         QStringLiteral("PropertiesChanged"), this, SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
}

void LidWatch::onPropertiesChanged(const QString &iface, const QVariantMap &changed, const QStringList &) {
    if (iface == QLatin1String(UPOWER) && changed.contains(QStringLiteral("LidIsClosed")))
        setClosed(changed.value(QStringLiteral("LidIsClosed")).toBool());
}

void LidWatch::setClosed(bool closed) {
    if (closed == closed_) return;
    closed_ = closed;
    timer_->stop();  // the lid opening ends the count; closing starts it again
    if (!closed) return;
    minutes_ = guiSettings().value(QStringLiteral("sleep/bag_warn_min"), DEFAULT_MINUTES).toInt();
    if (minutes_ > 0) timer_->start(minutes_ * 60 * 1000);
}

void LidWatch::fire() {
    // Opened in the meantime, or a screen is used with the lid shut: nothing to warn about.
    if (!closed_ || externalDisplay()) return;
    double cpu = 0, fan = 0;
    cpuAndFans(cpu, fan);
    QStringList readings;
    if (cpu > 0) readings << tr("CPU %1 °C").arg(qRound(cpu));
    if (fan > 0) readings << tr("fastest fan %1 rpm").arg(qRound(fan));
    Q_EMIT warm(tr("The lid has been closed for %1 min and the machine is still awake%2. Open it, or put it where it can breathe.")
                    .arg(minutes_)
                    .arg(readings.isEmpty() ? QString() : QStringLiteral(" (") + readings.join(QStringLiteral(", ")) + QLatin1Char(')')));
}
