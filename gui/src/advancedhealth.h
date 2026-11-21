#pragma once
#include <QJsonObject>
#include <QWidget>
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QProcess;

// H01/H06/H10/H12/H13/H15/H16/H17/H18 investigations.
// The user explicitly starts each acquisition. None run in background.
class AdvancedHealthPage : public QWidget {
public:
    explicit AdvancedHealthPage(QWidget *parent = nullptr);
private:
    void run();
    void showReport(const QString &code, const QJsonObject &boots = {},
                    const QJsonObject &evidence = {}, const QJsonObject &nvme = {});
    void updateMode();
    void refreshIncidents();
    void selectIncident();
    void saveNote();
    void activeGpuQuery();
    QComboBox *feature_ = nullptr, *event_ = nullptr, *incidentStatus_ = nullptr, *wizardCategory_ = nullptr;
    QPlainTextEdit *output_ = nullptr, *note_ = nullptr;
    QPushButton *scan_ = nullptr, *save_ = nullptr, *gpuQuery_ = nullptr;
    QLabel *message_ = nullptr;
    QWidget *incidentRow_ = nullptr;
    bool busy_ = false;
};
