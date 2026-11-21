#pragma once
// Health → Problems → Evidence Lab: one read-only scan of this boot's kernel log (health-reader
// "diagnostics"), shown as five reports (H02 machine checks, H03 PCIe AER, H04 NVIDIA Xid, H05 F2FS,
// H09 Wi-Fi / Bluetooth). The AER sample for the next comparison is saved in the user's data directory.
#include <QJsonObject>
#include <QWidget>

class DiagReportView;
class QLabel;
class QPushButton;
class QTabWidget;

class HealthEvidencePage : public QWidget {
    Q_OBJECT
public:
    explicit HealthEvidencePage(QWidget *parent = nullptr);

private:
    void collect();
    void display(const QJsonObject &reply);
    QTabWidget *tabs_ = nullptr;
    QPushButton *scan_ = nullptr;
    QLabel *status_ = nullptr;
    DiagReportView *views_[5]{};
    bool busy_ = false;
};
