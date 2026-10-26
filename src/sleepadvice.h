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
};
Sleep parse(const QJsonObject &o);

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
