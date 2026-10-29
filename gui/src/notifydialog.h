#pragma once
// Health → Notifications…: the switches for the desktop alerts (notifyprefs.h). Saved on OK.
#include <QDialog>

class QCheckBox;

class NotifyDialog : public QDialog {
    Q_OBJECT
public:
    explicit NotifyDialog(QWidget *parent = nullptr);

private:
    void save();

    QCheckBox *categories_[7];  // in notifyprefs::ALL_CATEGORIES order
    QCheckBox *criticalOnly_, *deferGame_;
};
