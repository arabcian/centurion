#pragma once
// Health → Sleep suggestions: which ACPI wake source keeps waking the machine
// while the lid is closed, and how much battery the machine spent awake
// afterwards. Pure functions over the sleep log (centurion-sleep-log), so the
// rules can be tested without sysfs.
//
// A wake counts as "unexpected" when neither the lid nor the power / sleep
// button is among the sources that woke it. The battery cost of such a wake is
// what the battery lost between waking and the next sleep, when both ends were
// on battery and the gap is at most MAX_AWAKE_S (the machine fell asleep again
// on its own or was put back, rather than being used for hours).
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

namespace sleepadvice {

inline constexpr int MIN_WAKES = 2;                 // a single odd wake is not a pattern
inline constexpr qint64 MAX_AWAKE_S = 2 * 3600;

struct Sleep {
    qint64 start = 0, end = 0;
    QStringList wokeBy;            // kernel wakeup-source names
    QString statusBefore;          // battery status when it went to sleep ("Discharging" = on battery)
    double energyBefore = 0, energyAfter = 0, full = 0;
    bool hasHwSleep = false;       // the sleep hook recorded the hardware sleep time (0 is a reading too)
    double hwSleepUs = 0;          // time in the hardware (SoC) low-power state
};
Sleep parse(const QJsonObject &o);

// "Why doesn't the hardware sleep?" is offered when the last sleeps of a useful length spent almost no
// time in the hardware low-power state (the sleep hook measured it).
inline constexpr int HW_IDLE_SLEEPS = 3;
inline constexpr qint64 HW_IDLE_MIN_S = 10 * 60;
inline constexpr double HW_IDLE_SHARE = 0.02;
/// `rowsOldestFirst`: the last HW_IDLE_SLEEPS sleeps with a reading and at least HW_IDLE_MIN_S long
/// are all below HW_IDLE_SHARE. Sleeps without a reading or too short are skipped.
bool hwSleepIdle(const QList<Sleep> &rowsOldestFirst);

/// Lid or power / sleep button (by kernel wakeup-source or ACPI device name).
bool isUserWake(const QString &name);

struct Advice {
    QString device;                // ACPI wake device (/proc/acpi/wakeup), e.g. XHC0
    int wakes = 0;                 // unexpected wakes it took part in
    double awakePct = 0;           // battery % lost while awake after those wakes (0 = not measurable)
};

/// `rows` oldest first. `resolve` maps a kernel wakeup-source name to the ACPI
/// wake device that can be switched off for it ("" = none / already off).
/// Returns the devices with at least MIN_WAKES unexpected wakes, costliest first.
QList<Advice> analyse(const QList<Sleep> &rows, const std::function<QString(const QString &)> &resolve);

} // namespace sleepadvice
