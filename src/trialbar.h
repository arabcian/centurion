#pragma once
// Trial apply for the curve tabs (NVIDIA, Ryzen, Intel): with "Trial apply" ticked, a successful apply
// starts a countdown; unless Keep is pressed in time, the tab's reset runs (curve back to stock).
// Same idea as the AMD GPU tab's trial. Protects against a curve that makes the desktop unusable but
// leaves the machine running (artefacts, a hung game, a black window); a hard freeze or reboot already
// resets every curve by itself (the SMU, MSRs and NVIDIA driver forget them).
#include <QFrame>
#include <functional>

class QCheckBox;
class QLabel;
class QPushButton;
class QTimer;

class TrialBar : public QFrame {
    Q_OBJECT
public:
    /// `key`: gui.ini key for the "Trial apply" tick (e.g. "nvidia/trial").
    explicit TrialBar(const QString &key, QWidget *parent = nullptr);
    bool wanted() const;
    bool running() const;
    /// Starts the countdown if trial apply is ticked; `revert` runs on time-out or "Revert now".
    void offer(std::function<void()> revert);
    /// The tab applied or reset something else meanwhile: the trial is over (no revert).
    void cancel();

private:
    void finish(bool keep);
    QString key_;
    QCheckBox *on_;
    QLabel *text_;
    QPushButton *keep_, *revert_;
    QTimer *timer_;
    int left_ = 0;
    std::function<void()> revertFn_;
};
