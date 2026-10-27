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
#include <QJsonObject>
#include <QWidget>
#include <functional>

class QComboBox;
class QLabel;
class QProcess;
class QProgressBar;
class QPushButton;
class QTreeWidget;

class CoolingPage : public QWidget {
    Q_OBJECT
public:
    explicit CoolingPage(QWidget *parent = nullptr);
    ~CoolingPage() override;
    /// Plain-text verdict for an `assessment` object of the tool (shared with the tests).
    static QString verdictText(const QJsonObject &assessment, double limitC);

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

    QLabel *context_, *live_, *verdict_;
    QProgressBar *bar_;
    QPushButton *run_, *cancel_, *baseline_, *forget_;
    QComboBox *length_;
    QTreeWidget *table_;
    QProcess *proc_ = nullptr;
    QByteArray buf_;
    double limitC_ = 95;
};
