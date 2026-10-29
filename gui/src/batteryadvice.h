#pragma once
// Health → Battery use: the charger history and the "set the charge mode to Long_Life" advice.
// The pure rules live here (no sysfs, no widgets); batterypage.cpp keeps the history in battery.json
// ("ac_days", "ac_drain") and tests/batteryadvice_test.cpp covers the rules.
#include <QDate>
#include <QJsonObject>
#include <QStringList>

namespace batteryadvice {

constexpr int WINDOW_DAYS = 30;           // the advice looks at this many days
constexpr int KEEP_DAYS = 60;             // older days are dropped from battery.json
constexpr double MIN_AC_SEC = 20 * 3600;  // less time on the charger than this is too little to judge
constexpr double FULL_SHARE = 0.70;       // more than this share of the charger time at full charge

struct Advice {
    bool show = false;
    double acHours = 0;     // charger time in the window
    double fullShare = 0;   // of that, the part at full charge (capacity ≥ 98 %)
};

/// `days`: {"YYYY-MM-DD": {"ac": seconds on the charger, "full": seconds of it at full charge}}.
/// `modes` / `current`: the battery's charge modes and the one selected. No advice without a
/// Long_Life mode to switch to, or when it is already selected.
Advice evaluate(const QJsonObject &days, QDate today, const QStringList &modes, const QString &current);

/// The days within `keepDays` of `today` (today included).
QJsonObject pruned(const QJsonObject &days, QDate today, int keepDays);

struct ChargeModes {
    QStringList modes;
    QString current;
};
/// sysfs charge_types: "Standard [Fast] Long_Life" → modes Standard, Fast, Long_Life; current Fast.
ChargeModes parseChargeTypes(const QString &line);

}  // namespace batteryadvice
