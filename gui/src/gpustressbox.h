#pragma once
// Health → Curve stability → GPU test: runs `centurion-gpustress` as the user (the VRAM test after a memory
// clock offset change, the core test after a core curve change), counts the kernel GPU errors the Monitor
// reports meanwhile, and keeps each result with the GPU curve state in effect (gpustress.h has the logic and
// the record).
#include "gpustress.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QWidget>

class QComboBox;
class QLabel;
class QProcess;
class QPushButton;
class QSpinBox;
class QTimer;
class QTreeWidget;

class GpuStressBox : public QWidget {
    Q_OBJECT
public:
    explicit GpuStressBox(QWidget *parent = nullptr);
    /// The page is shown (CurvePage::showEvent). `blocked`: an NVIDIA dGPU is on the bus but unusable. Listing the
    /// Vulkan devices would then wake it or hang on it, so the tool is not run and the test is not offered.
    void refresh(bool blocked);
    /// A new kernel event of this session (CurvePage::onFault): NVIDIA Xids and AMD GPU resets count against a running test.
    void onFault(const QJsonObject &e);

Q_SIGNALS:
    void alert(const QString &title, const QString &message);

private:
    void loadDevices();
    void showDevices();
    void start();
    void launch(const gpustress::Plan &p, gpustress::Mode m, int seconds);
    void finished(int exitCode, bool crashed);
    void conclude(int exitCode, bool crashed, qint64 seconds);
    void tick();
    void fill();
    void setIdle(bool idle);
    void note(const QString &text, const char *color);

    QString program_;
    QComboBox *device_ = nullptr, *mode_ = nullptr, *minutes_ = nullptr;
    QSpinBox *percent_ = nullptr;
    QLabel *percentLabel_ = nullptr, *status_ = nullptr;
    QPushButton *run_ = nullptr;
    QTreeWidget *results_ = nullptr;
    QProcess *proc_ = nullptr, *listProc_ = nullptr;
    QTimer *ticker_ = nullptr;
    QElapsedTimer elapsed_;
    QByteArray partial_;
    gpustress::Run run_state_;
    QList<gpustress::Device> devices_;
    QString pci_, deviceName_, curve_;
    gpustress::Mode runMode_ = gpustress::Mode::Vram;
    int seconds_ = 0, faults_ = 0;
    bool cancelled_ = false, blocked_ = false, settling_ = false;
};
