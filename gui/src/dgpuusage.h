#pragma once
// Health → dGPU power: how long the NVIDIA dGPU stayed awake while the machine ran on battery, and
// which process was holding it when it woke.
//
// Sampled from the battery page's one-minute tick (no timer and no process of its own). The inputs are
// runtime_active_time / runtime_suspended_time and runtime_status from sysfs: reading them does not
// wake the card. Kept in ~/.local/state/centurion/dgpu.json:
//   "days":  {"YYYY-MM-DD": {"bat": {"awake_ms", "asleep_ms"}, "ac": {…}}}   the last MAX_DAYS days
//   "power": {"awake": {"sec", "wh"}, "asleep": {"sec", "wh"}}                battery intervals by state
//   "wakes": [{"t", "holders"}]   the last MAX_WAKES wake-ups; "holders" is filled in by a scan that runs
//                                 on a worker thread once the wake is seen
// Overview's Rust side (centurion-helpers overview::dgpu_usage) turns these sums into the page's numbers.
// A stretch that stays awake on battery for LONG_AWAKE_MS (10 minutes) emits longAwake once; nothing is stored for it.
#include <QJsonObject>
#include <QObject>
#include <functional>

class DgpuUsage : public QObject {
    Q_OBJECT
public:
    /// `dev`: the NVIDIA PCI device directory; empty means there is nothing to track. `holders` returns
    /// the processes holding the device (a description, empty when none); it is called on a worker thread.
    DgpuUsage(const QString &dev, std::function<QString(const QString &dev)> holders, QObject *parent = nullptr);
    ~DgpuUsage() override;

    /// One sample. `energyWh` < 0 means unknown. The clocks are passed in so tests can drive time.
    void sample(bool onBattery, double energyWh, qint64 wallMs, qint64 monoMs);

    static QString file();

Q_SIGNALS:
    /// The dGPU has been awake on battery for `minutes` without a break; `holders` is what held it at the wake.
    void longAwake(qint64 minutes, const QString &holders);

private:
    void onWake(qint64 unixSec);
    void setHolders(qint64 unixSec, const QString &who);
    QString holdersOfWake(qint64 unixSec) const;
    void save();

    QString dev_;
    std::function<QString(const QString &)> holders_;
    QJsonObject data_;
    bool havePrev_ = false, prevBattery_ = false;
    qint64 prevActive_ = 0, prevSuspended_ = 0, prevWall_ = 0, prevMono_ = 0;
    double prevWh_ = -1;
    QString prevStatus_;
    int dirty_ = 0;
    qint64 streakStart_ = 0;  // wall-clock ms the current awake stretch on battery began; 0 = none
    bool warned_ = false;     // longAwake was emitted for this stretch
};
