#pragma once
// Scenes tab: edit scenes (which saved profile each component uses), apply
// one, and pick the scenes for automatic AC / battery switching.
#include "scenes.h"
#include <QJsonObject>
#include <QWidget>

class MainWindow;
class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

class ScenesTab : public QWidget {
    Q_OBJECT
public:
    explicit ScenesTab(MainWindow *win);

protected:
    void showEvent(QShowEvent *e) override;

private:
    void colorList();
    void reloadList(const QString &select = {});
    void select(const QString &name);
    void setEditor(const scenes::Scene &s);
    scenes::Scene fromEditor() const;
    void fillOptions();          // option lists come from the other tabs' saved profiles
    void updateDirty();
    void updateFirmwareLabel();
    void updatePowerLabel();
    void reloadAuto();
    void storeAuto();
    bool saveCurrent();
    void applyCurrent();
    void newScene(bool duplicate);
    void deleteScene();
    void captureFirmware();
    void captureFanTable(const QString &note);
    void setStatus(const QString &msg, const char *color = nullptr);

    MainWindow *win_;
    SceneEngine *eng_;
    QListWidget *list_;
    QGroupBox *editor_;
    QComboBox *profile_, *cpu_ = nullptr, *gpu_, *tuning_;
    QComboBox *fan_ = nullptr;
    QComboBox *lightProfile_ = nullptr, *lightBright_ = nullptr;  // only with a Spectrum keyboard
    QLabel *fwLabel_;
    QLabel *gpuLabel_ = nullptr;
    QPushButton *fwCapture_, *fwClear_, *fanClear_ = nullptr;
    QLabel *fanTableLabel_ = nullptr;
    QVector<int> fanTable_;          // editor copy of the captured Custom-mode fan table
    void updateFanTableLabel();
    // Display / keyboard extras: each row only where the machine has the thing behind it.
    QComboBox *panelOd_ = nullptr, *kbdLevel_ = nullptr, *refresh_ = nullptr;
    QWidget *panelOdLabel_ = nullptr, *refreshLabel_ = nullptr;
    QLabel *zone4Label_ = nullptr;
    QPushButton *zone4Capture_ = nullptr, *zone4Clear_ = nullptr;
    QJsonObject zone4_;              // editor copy of the captured 4-zone state
    QList<double> rates_;            // refresh rates the panel offers now (centurion-display list)
    bool ratesAsked_ = false;
    void updateZone4Label();
    void fillRefresh(double keep);
    void queryRates();
    void syncExtraRows();
    QLineEdit *command_;
    QPushButton *save_, *apply_, *dup_, *del_;
    QCheckBox *auto_;
    QCheckBox *resume_ = nullptr;
    class QPushButton *pause_ = nullptr;
    QComboBox *onAc_, *onBattery_;
    QLabel *power_, *status_, *empty_;

    scenes::Scene loaded_;           // as last saved / loaded
    QMap<QString, int> firmware_;    // editor copy of the firmware snapshot
    bool filling_ = false;
};
