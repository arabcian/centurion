#pragma once
// Fan curve assistant (Home → Fan curve → Assistant…): sets the firmware's CPU and GPU temperature limits
// (firmware-attributes cpu_temp / gpu_temp, honoured in the Custom profile) and proposes a Custom-mode table
// for a noise preset instead of hand-dragging ten points.
//
// The presets are built into the program (Quiet / Balanced / Performance / Max, each with its own curve). A
// ~/.config/centurion/fan-presets.json, when the user creates one, replaces them:
//     {"presets": [{"name": "Quiet", "level": 5, "cpu_min": 85, "cpu_max": 89, "ramp_from": 3}, ...]}
// level: fan level at the CPU limit (1–10); cpu_min..cpu_max: the CPU limits the preset offers (within the
// firmware's range); ramp_from: a temperature step (0–9, as in the editor); steps up to it stay at level 1. Or, instead of level / ramp_from, the curve
// itself: "curve": [10 levels, one per temperature step as in the editor], never going down, last three ≥ 3 / 5 / 7.
//
// The CPU throttles at its limit, so under sustained load it sits there: the curve is shaped on the EC's own
// temperature steps (those of the fan whose thresholds reach the hottest — the CPU's) so the step the EC is on
// at the limit gets the preset's level, steps below ramp up to it from level 1 at step ramp_from, and steps above it
// hold it up to limit + 8 °C, then 10.
//
// The limits are written when you put the proposal in the editor; the curve is kept only when you press Apply there.
#include "fancurvedialog.h"
#include <QDialog>
#include <QVector>

class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

struct FanPreset { QString name; int level, cpuMin, cpuMax, rampFrom; QVector<int> curve; };  // curve: empty = shaped

class FanAssistDialog : public QDialog {
    Q_OBJECT
public:
    explicit FanAssistDialog(const QVector<FanCurveFan> &fans, QWidget *parent = nullptr);
    QVector<int> result() const { return result_; }

    /// Builds the table that holds fan level `hold` (1..10) at the CPU limit `limit` °C, level 1 up to step `rampFrom` (0..9).
    static QVector<int> shape(double limit, int hold, int rampFrom, const QVector<int> &stepTemps);

private:
    struct Limit {
        QString path;              // firmware-attributes current_value; empty = not exposed here
        int current = 0, min = 0, max = 0, step = 1;  // the firmware's range and grid
        QSpinBox *spin = nullptr;
    };
    void propose();
    void apply();
    void applyPreset();  // the CPU limit's range follows the preset
    const FanCurveFan *cpuFan() const;

    QVector<FanCurveFan> fans_;
    QVector<FanPreset> presets_;
    QVector<int> result_;
    Limit cpu_, gpu_;
    QComboBox *noise_;
    QLabel *status_;
    QPushButton *use_;
};
