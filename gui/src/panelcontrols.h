#pragma once
// EDID / VRR → Panel controls: the internal panel's brightness (its /sys/class/backlight device) and refresh
// rate, through centurion-display, the same tool Scenes uses. Runs as the user; nothing here needs a password.
#include <QDialog>
#include <QJsonObject>
#include <functional>

class QComboBox;
class QLabel;
class QPushButton;
class QSlider;

class PanelControlsDialog : public QDialog {
    Q_OBJECT
public:
    explicit PanelControlsDialog(QWidget *parent = nullptr);

private:
    void run(const QStringList &args, std::function<void(const QJsonObject &)> done);

    QSlider *bright_;
    QLabel *brightValue_, *status_;
    QComboBox *rates_;
    QPushButton *setBright_, *setRate_;
};
