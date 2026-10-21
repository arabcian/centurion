#pragma once
// Fan curve assistant (Home → Fan curve → Assistant…): proposes a Custom-mode table for a goal
// (Quiet / Balanced / Cool) instead of hand-dragging ten points.
//
//  Quick    shapes the curve on the EC's own temperature steps (the CPU fan's sensor thresholds the
//           firmware reports): fans stay at level 1 below the goal's start temperature, ramp up to
//           the level that holds the goal temperature, and go to 10 well above it.
//  Measure  (~6 min, Custom profile, on AC) finds that "holding" level on this machine: a full CPU
//           load runs while the table is held flat at three levels in turn, the steady CPU
//           temperature at each is recorded, and the lowest level that keeps the goal temperature
//           under sustained load is interpolated. The original table is saved first
//           (~/.local/state/centurion/fan-assist-backup.json) and written back at the end, on
//           cancel and — if the app died meanwhile — the next time the fan curve dialog opens.
//
// The result only goes into the editor; nothing is kept until you press Apply there.
#include "fancurvedialog.h"
#include <QDialog>
#include <QVector>
#include <atomic>
#include <thread>
#include <vector>

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QTimer;

class FanAssistDialog : public QDialog {
    Q_OBJECT
public:
    FanAssistDialog(const QString &helper, const QVector<int> &current, const QVector<FanCurveFan> &fans, QWidget *parent = nullptr);
    ~FanAssistDialog() override;
    QVector<int> result() const { return result_; }

    /// Saved table of an interrupted measurement (empty = none).
    static QVector<int> pendingBackup();
    static void clearBackup();
    /// Builds the table for `goal` (0 quiet, 1 balanced, 2 cool) holding `hold` (1..10) at the goal temperature.
    static QVector<int> shape(int goal, int hold, const QVector<int> &stepTemps);

protected:
    void reject() override;

private:
    void startMeasure();
    void nextLevel();
    void tick();
    void finishMeasure(bool ok, const QString &why);
    void writeTable(const QVector<int> &lv, std::function<void(bool, const QString &)> done);
    void startLoad();
    void stopLoad();
    QVector<int> stepTemps() const;
    static double cpuTemp();

    QString helper_;
    QVector<int> current_;
    QVector<FanCurveFan> fans_;
    QVector<int> result_;
    QComboBox *goal_;
    QRadioButton *quick_, *measure_;
    QLabel *status_;
    QProgressBar *bar_;
    QPushButton *run_, *use_;

    // Measurement
    QTimer *timer_ = nullptr;
    QVector<int> levels_{3, 6, 9};
    int idx_ = -1, elapsed_ = 0;
    QVector<double> samples_;
    QVector<double> steady_;       // °C per measured level
    bool measuring_ = false, writing_ = false, touched_ = false;  // touched_: a flat table was written
    std::atomic<bool> stop_{false};
    std::vector<std::thread> workers_;
};
