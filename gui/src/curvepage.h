#pragma once
// Health → Curve stability: GPU / CPU curve profiles against the kernel errors that happened while
// each one was in effect, plus an optional guard that takes a curve back to stock after an error
// that points at it.
//
// Sources (nothing new is measured):
//   /var/lib/centurion/oc-journal.jsonl   every curve write by the root helpers (see ocjournal.rs)
//   /var/lib/centurion/exposure.json      boot start / end times (centurion-boot-guard)
//   ~/.local/state/centurion/health.log   the Monitor page's saved kernel errors (all boots)
// GPU curve ← NVIDIA Xid / GSP timeouts / D3cold failures; CPU curve (Curve Optimizer or Intel
// undervolt) ← machine checks and lockups. Xid 45 and 154 are follow-ups of another Xid and are
// not counted. A reboot resets every curve, so each boot starts at stock.
// Below the table: the memory test (memtestbox.h), for BIOS memory timings, the GPU test (gpustressbox.h),
// for a GPU core curve or a VRAM clock offset, and the CPU test (cpustressbox.h), for a Curve Optimizer or
// undervolt offset.
#include <QJsonObject>
#include <QWidget>

class QCheckBox;
class QLabel;
class MemTestBox;
class GpuStressBox;
class CpuStressBox;
class QTreeWidget;

class CurvePage : public QWidget {
    Q_OBJECT
public:
    explicit CurvePage(QWidget *parent = nullptr);
    /// A new kernel event of this session (HealthTab::fault).
    void onFault(const QJsonObject &e);
    /// Whether the kernel log is read live (HealthTab); the test boxes say so when it is not.
    void setKernelLog(bool live);
    /// Takes the GPU curve back to stock now (the guard's path; Monitor's action uses it too). `title` names the cause.
    void resetGpuCurve(const QString &title);
    /// "stock" or the journal's state of the last curve write this boot, for "gpu" / "cpu".
    static QString currentState(const QString &group);

Q_SIGNALS:
    void alert(const QString &title, const QString &message);
    /// The guard took a curve back to stock. `group` "gpu" / "cpu"; `profile` the saved profile that was in
    /// effect ("" when it was unsaved edits or not a profile), so the notification can offer to put it back.
    void curveReset(const QString &group, const QString &profile, const QString &message);

protected:
    void showEvent(QShowEvent *e) override;

private:
    void recompute();
    QTreeWidget *table_;
    QLabel *note_;
    QCheckBox *gpuReset_, *cpuReset_;
    MemTestBox *memTest_;
    GpuStressBox *gpuTest_;
    CpuStressBox *cpuTest_;
    qint64 lastGpuReset_ = 0, lastCpuReset_ = 0;  // ms since epoch: one reset per burst of errors
};
