#pragma once
// SMART record and disk-health alerts.
//
// The record is kept by the GUI as the user, in ~/.local/state/centurion/smart.json (730 days):
//   "disks":   {id: {"model", "samples": [{"d", "temp_c", "passed", "life_used_pct", "spare_pct",
//                                          "spare_threshold_pct", "media_errors", "unsafe_shutdowns",
//                                          "critical_warning", "written_tb", "power_on_hours"}]}}
//   "sampled": "YYYY-MM-DD"   the day of the last sample
// id = model|capacity|hash of the serial (tools.rs). Serial numbers are never written.
//
// Samples come from the root helper (tune-helper {"op":"tool","tool":"smart_json"}): on every run of the
// SMART page, and once a day from SmartWatch. A daily sample needs no password only for a wheel or sudo
// admin (49-centurion.rules), so SmartWatch skips the day for anyone else; they get one when the page opens.
// Alerts compare with the previous day's sample: critical_warning became non-zero, media errors went up,
// spare capacity fell below its threshold. SmartWatch sends them to the tray; the page shows them too.
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <optional>

class QTimer;

namespace smart {

/// The number under `key`, or nullopt when it is absent or null.
std::optional<double> field(const QJsonObject &o, const char *key);
QJsonObject loadHistory();
/// Stores today's sample of each disk (replacing today's earlier one) and returns the alert lines.
/// Entries carrying an "error" are skipped.
QStringList record(const QJsonArray &disks);

} // namespace smart

/// Once an hour checks whether today's sample is due; takes it for admins only. Emits `alert` with new alert lines.
class SmartWatch : public QObject {
    Q_OBJECT
public:
    explicit SmartWatch(QObject *parent);

Q_SIGNALS:
    void alert(const QStringList &lines);

private:
    void check();
    QTimer *timer_;
    bool busy_ = false;
};
