#pragma once
// Health → Battery: time left at the current draw, battery drain per scene, and the capacity
// (wear) history with its chart and the wear forecast (tune-profile-helper "battery_trend"), and the
// battery's maker / manufacture date / age where the firmware reports them (health-reader "battery_info").
//
// The record is kept by the GUI itself, as the user, in ~/.local/state/centurion/battery.json:
//   "scenes":   {name: {"sec", "wh"}}   time spent on battery and energy taken from it, per active scene
//                                        (from energy_now differences, so it is what the battery really gave)
//   "capacity": [{"d", "full", "design", "cycles"}]   one entry per day the app ran
//   "ac_days":  {"YYYY-MM-DD": {"ac", "full"}}       seconds on the charger, and of those at full charge
//                                                    (kept for batteryadvice::KEEP_DAYS days)
//   "ac_drain": [{"t", "sec", "wh", "scene"}]        stretches on the charger where the battery still drained:
//                                                    the adapter could not carry the load (newest 100)
// One sysfs read per minute (status); on battery a few more. Intervals that span a suspend are not
// counted here (sleep drain is on the Sleep page). The advice rules are in batteryadvice.h.
#include <QElapsedTimer>
#include <QJsonObject>
#include <QWidget>

class BarChart;
class DgpuUsage;
class LineChart;
class QLabel;
class QPushButton;
class QTimer;
class QTreeWidget;

class BatteryPage : public QWidget {
    Q_OBJECT
public:
    explicit BatteryPage(QWidget *parent = nullptr);
    ~BatteryPage() override;
    static bool present();

Q_SIGNALS:
    /// The dGPU stayed awake on battery for `minutes` (DgpuUsage::longAwake, passed on).
    void dgpuAwake(qint64 minutes, const QString &holders);

protected:
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;

private:
    // One stretch on the charger while the battery drained (ac_drain).
    struct Drain {
        bool open = false;
        qint64 start = 0;      // wall clock, ms
        double sec = 0, wh = 0, prevWh = 0;
        QString scene;
    };

    void tick();
    void sampleCapacity();
    void accountCharger(double sec, bool full);
    void closeDrain();
    void setLongLife();
    void save();
    void refreshNow();
    void refreshTables();
    void refreshForecast();
    void refreshInfo();
    static QString file();
    static QString chargeFile();

    QJsonObject data_;
    QTimer *timer_, *nowTimer_;
    QLabel *now_, *wear_, *forecast_, *advice_, *batInfo_;
    bool infoAsked_ = false;
    BarChart *scenesChart_;
    LineChart *healthChart_;
    QPushButton *longLife_;
    QTreeWidget *scenes_, *capacity_, *drains_;
    QString chargeFile_;   // the battery's charge_types (empty: no charge modes)
    // Previous sample on battery.
    bool prevValid_ = false;
    double prevWh_ = 0;
    qint64 prevWall_ = 0, prevMono_ = 0;
    QString prevScene_;
    // Previous sample on the charger, and the drain stretch in progress.
    bool acValid_ = false;
    qint64 acWall_ = 0, acMono_ = 0;
    Drain drain_;
    QElapsedTimer mono_;
    int dirty_ = 0;
    DgpuUsage *usage_ = nullptr;  // dGPU awake time, sampled from the same tick (Health → dGPU power)
};
