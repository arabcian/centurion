#pragma once
// Lighting tab for the 4-zone RGB keyboard (Legion 5 / 5 Pro / LOQ / IdeaPad
// Gaming generations before per-key Spectrum; ITE 048D:C9xx, 33-byte report).
// The controller is write-only, so the tab shows what it last applied (kept in
// ~/.config/centurion/zone4.ini) and changes nothing until Apply.
#include <QColor>
#include <QWidget>
#include <array>

class QComboBox;
class Zone4Canvas;
class QLabel;
class QGridLayout;
class QPushButton;

class Zone4Tab : public QWidget {
    Q_OBJECT
public:
    explicit Zone4Tab(QWidget *parent = nullptr);

protected:
    void resizeEvent(QResizeEvent *event) override;
private:
    void reflowZones();
    void load();
    void save() const;
    void syncEnabled();
    void paintZone(int i);
    void editZone(int i);
    void apply(bool off);

    std::array<QColor, 4> colors_{QColor("#ffffff"), QColor("#ffffff"), QColor("#ffffff"), QColor("#ffffff")};
    std::array<QPushButton *, 4> zone_{};
    QGridLayout *zonesLayout_=nullptr;
    Zone4Canvas *canvas_=nullptr;
    QWidget *canvasHost_=nullptr;
    int zoneColumns_=0;
    QComboBox *effect_ = nullptr, *speed_ = nullptr, *brightness_ = nullptr, *direction_ = nullptr;
    QLabel *speedLabel_ = nullptr, *directionLabel_ = nullptr, *zonesLabel_ = nullptr, *status_ = nullptr;
    QPushButton *apply_ = nullptr, *off_ = nullptr;
    bool busy_ = false;
};
