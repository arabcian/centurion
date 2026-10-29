#pragma once
// Health → dGPU power: everything that decides whether the NVIDIA dGPU can
// reach runtime D3cold, on one page (no commands to remember).
//
// Read-only sysfs/procfs only — runtime-PM attributes, the ACPI power state,
// the driver's own /proc/driver/nvidia report and module parameters, and which
// of the user's processes hold the device open. Nothing here goes through
// NVML/nvidia-smi or PCI config space, so looking at the page never wakes the
// GPU or resets its idle timer. Polled only while the page is visible, on a
// worker thread (a wedged driver must not freeze the GUI).
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QWidget>

class QCheckBox;
class QLabel;
class QTimer;
class QTreeWidget;

class DgpuPage : public QWidget {
    Q_OBJECT
public:
    explicit DgpuPage(QWidget *parent = nullptr);
    static bool present();  // an NVIDIA display-class PCI function exists
    /// Canonical sysfs directory of that function (empty when there is none).
    static QString devicePath();
    /// Processes of the user holding the device at `dev`, as "name (pid) → nodes; …" (empty when none).
    /// Reads /proc only; safe on a worker thread and never wakes the GPU.
    static QString holdersAt(const QString &dev);

    struct Row { QString section, key, value; int level; };  // 0 plain, 1 good, 2 attention
    struct Report { QString verdict; int level = 0; QList<Row> rows, holders; };

protected:
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;

private:
    void refresh(bool scanHolders);
    void apply(const Report &r);
    void refreshUsage();          // dgpu_usage from the usage tracker (a helper call, about once a minute)
    void onUsage(const QJsonObject &r);

    void setAwake(bool on);

    QCheckBox *awake_;
    QLabel *verdict_;
    QTreeWidget *tree_;
    QTimer *timer_;
    bool busy_ = false;
    int tick_ = 0;
    QList<Row> holders_;   // last "open handles" scan (rescanned every few ticks)
    QList<Row> usageRows_; // "Usage on battery" and "Wake-ups" sections, from the tracker
    Report last_;          // last sysfs report, so the usage rows can be shown without a new scan
    QString shown_;        // text of what the tree shows (skip identical rebuilds)
};
