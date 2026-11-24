#pragma once
// SMART record and disk-health alerts.
//
// The record is kept by the GUI as the user, in ~/.local/state/centurion/smart.json (730 days; a disk
// not seen for that long is dropped):
//   "disks":   {id: {"model", "samples": [{"d", "temp_c", "passed", "life_used_pct", "spare_pct",
//                                          "spare_threshold_pct", "media_errors", "unsafe_shutdowns",
//                                          "critical_warning", "written_tb", "power_on_hours"}]}}
//   "sampled": "YYYY-MM-DD"   the last day with a complete saved batch
// id = model|capacity|hash of the serial (tools.rs). Serial numbers are never written.
//
// Samples come from the root helper (tune-helper {"op":"tool","tool":"smart_json"}): on every run of the
// SMART page, and once a day from SmartWatch when explicitly enabled. A daily sample needs no password only for a wheel or sudo
// admin (49-centurion.rules), so SmartWatch skips the day for anyone else; they get one when the page opens.
// Alerts compare with the last retained sample (including the same day): critical_warning became non-zero, media errors went up,
// spare capacity fell below its threshold. SmartWatch sends them to the tray; the page shows them too.
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <optional>
#include <cstdint>
#include <functional>

class QTimer;
namespace privileged { struct Result; }

namespace smart {

/// The number under `key`, or nullopt when it is absent or null.
std::optional<double> field(const QJsonObject &o, const char *key);
std::optional<std::int64_t> counter(const QJsonObject &o, const char *key);
QJsonObject loadHistory();
std::optional<QJsonObject> historySnapshot();
// Older helpers could generate the same ID for all serial-less disks.
bool usableIdentity(const QString &id);
// A partial or failed read must not advance the last-complete-day marker.
enum class RecordStatus { Saved, Partial, NoUsableSamples, Unavailable };
struct RecordResult { RecordStatus status = RecordStatus::Unavailable; QStringList alerts; };
RecordResult record(const QJsonArray &disks);
bool automaticSamplingEnabled();
void setAutomaticSamplingEnabled(bool enabled);
// GUI-thread-only shared physical acquisition. The history is saved once per
// successful batch. Timeout does not release the flight before process exit.
using SampleCallback = std::function<void(const privileged::Result &, const RecordResult &)>;
void requestSample(QObject *owner, SampleCallback callback, std::function<void()> onTimeout = {});

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
