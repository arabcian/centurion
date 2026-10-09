#pragma once
// Read-only "what this machine's firmware reports" view: model series and
// generation, BIOS, the Lenovo WMI interfaces present, and the capability list
// (LENOVO_CAPABILITY_DATA_00) that decides which optional features are offered.
// Data: legion-profile-helper {"machine": "report"}.
#include <QDialog>

class QLabel;
class QPlainTextEdit;

class DeviceInfoDialog : public QDialog {
    Q_OBJECT
public:
    DeviceInfoDialog(const QString &helper, QWidget *parent = nullptr);
private:
    void load();
    QString helper_;
    QPlainTextEdit *text_;
    QLabel *status_;
};
