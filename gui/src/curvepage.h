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
#include <QJsonObject>
#include <QWidget>

class QCheckBox;
class QLabel;
class QTreeWidget;

class CurvePage : public QWidget {
    Q_OBJECT
public:
    explicit CurvePage(QWidget *parent = nullptr);
    /// A new kernel event of this session (HealthTab::fault).
    void onFault(const QJsonObject &e);

Q_SIGNALS:
    void alert(const QString &title, const QString &message);

protected:
    void showEvent(QShowEvent *e) override;

private:
    void recompute();
    /// "stock" or the journal's state of the last curve write this boot, for "gpu" / "cpu".
    static QString currentState(const QString &group);
    QTreeWidget *table_;
    QLabel *note_;
    QCheckBox *gpuReset_, *cpuReset_;
    qint64 lastGpuReset_ = 0, lastCpuReset_ = 0;  // ms since epoch: one reset per burst of errors
};
