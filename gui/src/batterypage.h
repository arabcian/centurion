#pragma once
// Health → Battery: time left at the current draw, battery drain per scene, and the capacity
// (wear) history.
//
// The record is kept by the GUI itself, as the user, in ~/.local/state/centurion/battery.json:
//   "scenes":   {name: {"sec", "wh"}}   time spent on battery and energy taken from it, per active scene
//                                        (from energy_now differences, so it is what the battery really gave)
//   "capacity": [{"d", "full", "design", "cycles"}]   one entry per day the app ran
// One sysfs read per minute (status); on battery a few more. Intervals that span a suspend are not
// counted here (sleep drain is on the Sleep page).
#include <QElapsedTimer>
#include <QJsonObject>
#include <QWidget>

class QLabel;
class QTimer;
class QTreeWidget;

class BatteryPage : public QWidget {
    Q_OBJECT
public:
    explicit BatteryPage(QWidget *parent = nullptr);
    ~BatteryPage() override;
    static bool present();

protected:
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;

private:
    void tick();
    void sampleCapacity();
    void save();
    void refreshNow();
    void refreshTables();
    static QString file();

    QJsonObject data_;
    QTimer *timer_, *nowTimer_;
    QLabel *now_, *wear_;
    QTreeWidget *scenes_, *capacity_;
    // Previous sample on battery.
    bool prevValid_ = false;
    double prevWh_ = 0;
    qint64 prevWall_ = 0, prevMono_ = 0;
    QString prevScene_;
    QElapsedTimer mono_;
    int dirty_ = 0;
};
