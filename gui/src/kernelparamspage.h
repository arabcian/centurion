#pragma once
// Health → Kernel parameters: the live value of every readable setting under a chosen part of /sys,
// debugfs included (the scheduler's /sys/kernel/debug/sched is root-only, so it is read through
// health-reader's read-only "ksnap" op: silent polkit action, nothing is written, see ksnap.rs).
// Read on the button only, never in the background. The scheduler scope also lists the sched_* sysctls.
#include <QJsonArray>
#include <QJsonObject>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QTableView;
class QSortFilterProxyModel;
class KernelParamsModel;
class QPushButton;

class KernelParamsPage : public QWidget {
    Q_OBJECT
public:
    explicit KernelParamsPage(QWidget *parent = nullptr);

private:
    void read();
    bool fill(const QJsonArray &entries);
    void applyFilter();
    QComboBox *scope_;
    QLineEdit *filter_;
    QLabel *status_;
    QTableView *tree_;
    KernelParamsModel *model_;
    QSortFilterProxyModel *proxy_;
    QString viewScope_;
    bool busy_ = false;
    QJsonArray captured_;
    QJsonObject capturedMeta_;
    QString capturedScope_, capturedAt_, capturedStatus_;
    QPushButton *export_ = nullptr;
    void exportSnapshot();
};
