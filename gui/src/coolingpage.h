#pragma once
// Health → Cooling: does the laptop still cool as well as it did?
//
// Runs `centurion-cooltest` (as the user): a fixed CPU load for a few minutes, sampling die
// temperature, fan speed and clock. The first clean run under a given set of firmware settings
// (PL1 / PL2 / PL3, temperature target, power profile, power source) becomes that setting's
// baseline; later runs are compared with it, and a hotter die, louder fans or - when the firmware
// pins the die at its temperature target - lower clocks suggest dust or dried-out thermal paste.
// The analysis lives in the tool (crates/centurion-helpers/src/cooling.rs); this page only runs it
// and words the result. Nothing runs in the background: the tool is started by the button.
//
// Fan max test (button, only with the Home tab's fan controls): all fans go to maximum for 30 s,
// after 5 s of reading, sampling each fan's peak speed; then the fans go back as they were on Home.
// The peaks are judged by centurion-cooltest against the first test under the same firmware settings.
#include <QElapsedTimer>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QWidget>
#include <functional>

class QComboBox;
class QLabel;
class QProcess;
class QProgressBar;
class QPushButton;
class QTimer;
class QTreeWidget;

/// The Home tab's fan controls as the fan max test uses them. The main window wires them to the Home tab
/// (this page does not depend on it). Without them the test is not offered.
struct FanControls {
    std::function<QList<QPair<QString, int>>()> targets;  // fan key and its target (0 = Auto)
    std::function<bool()> maxMode, busy;
    std::function<void()> allMax, allAuto;
    std::function<void(const QString &, int)> setTarget;
};

class CoolingPage : public QWidget {
    Q_OBJECT
public:
    explicit CoolingPage(QWidget *parent = nullptr);
    ~CoolingPage() override;
    /// Plain-text verdict for an `assessment` object of the tool (shared with the tests).
    static QString verdictText(const QJsonObject &assessment, double limitC);
    /// Plain-text result of `centurion-cooltest fanmax`; `warn` is set when a fan peak is too low.
    static QString fanMaxText(const QJsonObject &result, bool *warn = nullptr);
    void setFanControls(const FanControls &controls);

protected:
    void showEvent(QShowEvent *e) override;

private:
    void refresh();
    void fillStatus(const QJsonObject &s);
    void start();
    void onOutput();
    void finished(const QJsonObject &result);
    void tool(const QStringList &args, const std::function<void(const QJsonObject &)> &done);
    void setRunning(bool on);
    void showVerdict(const QString &text, const char *color);
    void fanMaxStart();
    void fanTick();
    void fanStop(bool aborted);
    void fanRestore(int tries);

    QLabel *context_, *live_, *verdict_;
    QProgressBar *bar_;
    QPushButton *run_, *cancel_, *baseline_, *forget_, *fanMax_;
    QComboBox *length_;
    QTreeWidget *table_;
    QProcess *proc_ = nullptr;
    QByteArray buf_;
    double limitC_ = 95;
    FanControls fans_;
    QTimer *fanTimer_;
    QElapsedTimer fanClock_;
    bool fanTesting_ = false, fanMaxed_ = false, fanPrevMax_ = false, fanReading_ = false;
    QList<QPair<QString, int>> fanPrev_;  // fan key and its target before the test (0 = Auto)
    QList<double> fanPeaks_;              // highest reading of each fan while at maximum
};
