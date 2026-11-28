#pragma once
// Health → Problems → Diagnostics → This Boot: one read-only scan of this boot's kernel log and device state
// (health-reader "diagnostics"), read once per component: machine checks (H02), kernel (H19), memory (H23),
// PCIe (H03), storage (H20), file systems (H05), NVIDIA Xid (H04), AMD / Intel GPU (H21), thermal & power
// (H22), USB (H24), audio (H25), Wi-Fi / Bluetooth (H09), firmware & drivers (H26). The AER sample for the
// next comparison is saved in the user's data directory.
#include <QJsonObject>
#include <QList>
#include <QWidget>

class DiagReportView;
class QComboBox;
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
    void applyFocus();
    QComboBox *focus_ = nullptr;
    QTabWidget *tabs_ = nullptr;
    QPushButton *scan_ = nullptr;
    QLabel *status_ = nullptr;
    QList<DiagReportView *> views_;
    bool busy_ = false;
};
