#pragma once
// Health → Investigations (H01–H18): each turns records Centurion already keeps (boot history, the saved
// kernel logs, the sleep log, CO Search records, the flight recorder) into a diag::Report. The report
// functions take their inputs as arguments so they can be tested with fixtures; the *FromSystem helpers
// read the real files. Read-only: nothing here writes hardware or kernel state.
#include "diagreport.h"
#include "diagsources.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace advanceddiagnostics {

/// P(X >= k) for X ~ Binomial(n, p): how surprising k of n events falling on one side is when that side
/// had a share p of the exposure. Small = the side with k really has the higher rate.
double upperTail(int k, int n, double p);

// H01. `boots`: health-reader "boots" (newest first). `legacy`: the GUI-sampled version file of the earlier
// release ({"boots": {boot_id: {kernel, nvidia, bios}}}), used for boots the boot guard did not record.
diag::Report bootRegression(const QJsonArray &boots, const QJsonObject &legacy, const QJsonObject &current);
QJsonObject currentVersions();
QJsonObject legacyVersions();

// H06. health-reader "nvme_deep".
diag::Report nvmeHealth(const QJsonObject &nvme);
/// "Invalid Field in Command" for status 0x2002 (phase bit already shifted out, as nvme-cli prints it).
QString nvmeStatus(quint64 statusField, bool *mediaError = nullptr);

// H10. Saved kernel-log lines (warnings and worse). `present`: USB port path → product name now.
diag::Report usbStability(const QList<diagsrc::LogLine> &kernel, const QHash<QString, QString> &present);
QHash<QString, QString> usbPresent();

// H12.
struct SleepInputs {
    QJsonObject stats;              // /sys/power/suspend_stats: name → value
    QList<QJsonObject> sleeps;      // /var/log/centurion/sleep.log, oldest first
    QList<QJsonObject> died;        // /var/log/centurion/sleep-failed.log
    QString inflight;               // /var/lib/centurion/sleep-inflight
    QString currentBoot, memSleep;  // boot_id, /sys/power/mem_sleep
};
SleepInputs sleepInputsFromSystem();
diag::Report sleepMatrix(const SleepInputs &in, const QJsonArray &boots);

// H13. `guardsValid`: the guard file is absent or readable (an unreadable one must not read as "no guards").
struct CoRecords {
    QJsonObject session;  bool sessionPresent = false;
    QJsonArray guards;    bool guardsValid = true;
    QJsonArray history;
    QString bios;
};
CoRecords coRecordsFromSystem();
diag::Report coCrossLink(const CoRecords &co, const QJsonArray &boots, const QJsonObject &evidence);

// H15.
diag::Report flightCoverage(const QJsonArray &boots);

// H16. `evidence`: health-reader "diagnostics" (its "xid" list); `activeQuery`: nvidia-smi -q -d ECC,… text
// when the user asked for it.
diag::Report gpuMemory(const QJsonObject &evidence, const QStringList &gpus, const QString &activeQuery);
/// NVIDIA display controllers now: "0000:01:00.0 · NVIDIA GeForce RTX … · runtime suspended".
QStringList nvidiaGpus();

// H17: classified events from health.log with their private status and note.
struct Incident { diagsrc::LogLine line; QString status, note, edited; };
QList<Incident> incidents(const QList<diagsrc::LogLine> &events, const QJsonObject &notes);
QJsonObject incidentNotes();
QJsonObject incidentNote(const QString &key);
QString saveIncident(const QString &key, const QString &status, const QString &note);
QStringList incidentStatuses();

// H18: the category's checks merged into one report.
struct TriageCategory { QString key; const char *label; QStringList checks; };
QList<TriageCategory> triageCategories();
diag::Report triage(const QString &label, const QList<diag::Report> &checks);

// S04 helper, kept for the PCIe pages: the "TOTAL_ERR_COR: 14" line of an aer_dev_* file.
QString aerTotal(const QString &text, const QString &counter);

}  // namespace advanceddiagnostics
