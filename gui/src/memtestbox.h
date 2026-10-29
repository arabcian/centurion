#pragma once
// Health → Curve stability → Memory test: runs stressapptest or memtester as the user for a chosen share
// of the free memory and time, counts machine checks the Monitor reports meanwhile, and keeps each result
// with the BIOS memory timing snapshot (memtest.h has the logic and the record).
#include "memtest.h"

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

class MemTestBox : public QWidget {
    Q_OBJECT
public:
    explicit MemTestBox(QWidget *parent = nullptr);
    /// A new kernel event of this session (CurvePage::onFault): machine checks count against a running test.
    void onFault(const QJsonObject &e);
    /// Whether the kernel log is read live (CurvePage::setKernelLog). When not, events come minutes late and are not counted.
    void setKernelLog(bool live);

Q_SIGNALS:
    void alert(const QString &title, const QString &message);

private:
    void start();
    void launch(const memtest::Plan &p, const QJsonObject &bios);
    void finished(int exitCode, bool crashed);
    void conclude(int exitCode, bool crashed, qint64 seconds);
    void tick();
    void fill();
    static QString biosText(const QJsonObject &bios);

    memtest::Tool tool_ = memtest::Tool::None;
    QString program_;
    QSpinBox *percent_ = nullptr;
    QComboBox *minutes_ = nullptr;
    QPushButton *run_ = nullptr;
    QLabel *status_ = nullptr, *klogNote_ = nullptr;
    QTreeWidget *results_ = nullptr;
    QProcess *proc_ = nullptr;
    QTimer *ticker_ = nullptr;
    QElapsedTimer elapsed_;
    QByteArray partial_;
    memtest::Run run_state_;
    memtest::Plan plan_;
    QJsonObject bios_;
    int seconds_ = 0, mce_ = 0;
    bool stopped_ = false, cancelled_ = false, settling_ = false, kernelLive_ = true;
};
