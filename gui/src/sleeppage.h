#pragma once
// Health → Sleep: what suspend does on this machine.
//   * kernel suspend statistics and the active mode (s2idle / deep), hardware sleep residency;
//   * one row per sleep from the sleep hook's log (/var/log/centurion/sleep.log, written by the
//     centurion-sleep-log system-sleep hook): duration, battery used per hour, hardware-sleep share,
//     the wake source, the NVIDIA dGPU state before and after;
//   * ACPI wake sources (/proc/acpi/wakeup) with a switch each — remembered for every boot by
//     tune-helper (needs the centurion-tune service to put them back at boot);
//   * suggestions: an enabled ACPI wake source that keeps waking the machine with the lid closed,
//     with the battery it cost and a button to switch it off (sleepadvice.h);
//   * "Why doesn't the hardware sleep?": offered when the recent sleeps spent almost no time in the
//     hardware low-power state; one button runs the read-only check (health-reader, sleepdiag.rs);
//   * the limit for the "lid closed, still awake" tray warning (lidwatch.h) is set here (gui.ini).
// Everything is read on show only; nothing runs in the background. The check runs on the button only.
#include <QWidget>

class QJsonObject;
class QLabel;
class QPushButton;
class QTreeWidget;

class SleepPage : public QWidget {
    Q_OBJECT
public:
    explicit SleepPage(QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *e) override;

private:
    void refresh();
    void fillSleeps();
    void fillWake();
    void fillAdvice(const QList<QJsonObject> &rowsOldestFirst);
    void setWake(const QString &dev, bool on);
    void diagnose();
    void showDiagnosis(const QJsonObject &r);
    static QString findingText(const QString &code, const QJsonValue &detail);
    QLabel *summary_, *hint_, *sources_, *diagOut_;
    QWidget *advice_;
    QPushButton *diag_;
    QTreeWidget *sleeps_, *wake_;
    bool filling_ = false;
};
