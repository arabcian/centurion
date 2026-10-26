#pragma once
// Health → Sleep: what suspend does on this machine.
//   * kernel suspend statistics and the active mode (s2idle / deep), hardware sleep residency;
//   * one row per sleep from the sleep hook's log (/var/log/centurion/sleep.log, written by the
//     centurion-sleep-log system-sleep hook): duration, battery used per hour, hardware-sleep share,
//     the wake source, the NVIDIA dGPU state before and after;
//   * ACPI wake sources (/proc/acpi/wakeup) with a switch each — remembered for every boot by
//     tune-helper (needs the centurion-tune service to put them back at boot);
//   * suggestions: an enabled ACPI wake source that keeps waking the machine with the lid closed,
//     with the battery it cost and a button to switch it off (sleepadvice.h).
// Everything is read on show only; nothing runs in the background.
#include <QWidget>

class QJsonObject;
class QLabel;
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
    QLabel *summary_, *hint_, *sources_;
    QWidget *advice_;
    QTreeWidget *sleeps_, *wake_;
    bool filling_ = false;
};
