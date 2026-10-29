#pragma once
// Health → Notifications: which alert categories reach the desktop, the lowest severity for kernel
// alerts, and whether alerts wait until a game session has ended. Stored in gui.ini:
//   notify/off            categories switched off (keys below)
//   notify/critical_only  kernel alerts: only "critical" ones (default: error and critical)
//   notify/defer_game     hold alerts while a game session runs, then one summary (default: off)
// The decision (decide) is pure and tested in tests/notifyprefs_test.cpp. Alerts that are not in a
// category (game detected, scene switched, fans put back to Auto…) are always shown.
#include <QString>
#include <QStringList>

namespace notifyprefs {

// Category keys: they are also the names in gui.ini.
inline const QString KERNEL = QStringLiteral("kernel");    // Health scan: hardware / driver errors (has levels)
inline const QString NETWORK = QStringLiteral("network");  // Network guard: blocked connections
inline const QString BACKUP = QStringLiteral("backup");    // Backup reminders and finished jobs
inline const QString CURVE = QStringLiteral("curve");      // Curve guard: suspect or reset curves
inline const QString SMART = QStringLiteral("smart");      // Disk health (SMART)
inline const QString HEAT = QStringLiteral("heat");        // Lid closed while the machine stays awake
inline const QString COOLING = QStringLiteral("cooling");  // Cooling test overdue (Health → Cooling)
inline const QStringList ALL_CATEGORIES{KERNEL, NETWORK, BACKUP, CURVE, SMART, HEAT, COOLING};

// Severities of kernel alerts; other categories pass an empty one.
inline const QString ERROR_LEVEL = QStringLiteral("error");
inline const QString CRITICAL_LEVEL = QStringLiteral("critical");

struct Prefs {
    QStringList off;              // category keys switched off
    bool criticalOnly = false;
    bool deferGame = false;
};

Prefs load();
void save(const Prefs &p);

enum class Verdict { Show, Hide, Defer };

/// What to do with one alert of `category` and `severity`, given whether a game session runs now.
Verdict decide(const Prefs &p, const QString &category, const QString &severity, bool gameRunning);

}  // namespace notifyprefs
