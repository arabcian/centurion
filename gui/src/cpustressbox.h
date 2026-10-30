#pragma once
// Health → Curve stability → CPU test: runs `centurion-cpustress` as the user (per core after changing
// per-core Curve Optimizer offsets, all cores after an all-core offset or an Intel undervolt), counts the
// machine checks and lockups the Monitor reports meanwhile, and keeps each result with the CPU curve in
// effect (cpustress.h has the logic and the record). On AMD it can hold the Optimizations "CO validation"
// preset for the length of the test, as a game session of tune-helper, so the previous tuning comes back
// afterwards (and tune-helper's stale-session cleanup returns it if Centurion dies first).
#include "cpustress.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QProcess;
class QPushButton;
class QSpinBox;
class QTimer;
class QTreeWidget;

class CpuStressBox : public QWidget {
    Q_OBJECT
public:
    explicit CpuStressBox(QWidget *parent = nullptr);
    /// A new kernel event of this session (CurvePage::onFault): machine checks and lockups count against a running test.
    void onFault(const QJsonObject &e);
    /// Whether the kernel log is read live (CurvePage::setKernelLog). When not, events come minutes late and are not counted.
    void setKernelLog(bool live);

Q_SIGNALS:
    void alert(const QString &title, const QString &message);

private:
    void start();
    void launch();
    void finished(int exitCode, bool crashed);
    void conclude(int exitCode, bool crashed, qint64 seconds);
    void releasePreset();
    QJsonObject record(const QString &verdict) const;
    void tick();
    void fill();
    void fillDurations();
    void setIdle(bool idle);
    void note(const QString &text, const char *color);
    QString cpuName(int cpu) const;
    QString cpuList(const QList<int> &cpus) const;
    int totalSeconds() const;

    QString program_;
    cpustress::Info info_;
    QComboBox *mode_ = nullptr, *load_ = nullptr, *duration_ = nullptr;
    QSpinBox *cycles_ = nullptr;
    QLabel *cyclesLabel_ = nullptr, *durationLabel_ = nullptr, *status_ = nullptr, *klogNote_ = nullptr;
    QCheckBox *transitions_ = nullptr, *preset_ = nullptr;
    QPushButton *run_ = nullptr;
    QTreeWidget *results_ = nullptr;
    QProcess *proc_ = nullptr;
    QTimer *ticker_ = nullptr;
    QElapsedTimer elapsed_;
    QByteArray partial_;
    cpustress::Run run_state_;
    cpustress::Plan plan_;
    cpustress::Mode runMode_ = cpustress::Mode::PerCore;
    QString load_name_, curve_, profileHash_;
    qint64 id_ = 0;
    int seconds_ = 0, cycles_n_ = 1, faults_ = 0;
    QList<int> faultCpus_;
    bool cancelled_ = false, settling_ = false, kernelLive_ = true, presetHeld_ = false, starting_ = false;
};
