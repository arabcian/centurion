#pragma once
// Health → Problems → Investigations: on-demand H01–H18 reports (advanceddiagnostics), the incident notebook
// (H17) and guided triage (H18). Nothing runs until a button is pressed; every source is read-only.
#include "diagreport.h"

#include <QHash>
#include <QJsonObject>
#include <QWidget>

#include <functional>

class DiagReportView;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTextBrowser;
class QTreeWidget;

namespace investigations {
/// The health-reader operations a report needs ("boots", "diagnostics", "nvme_deep").
QStringList opsFor(const QString &code);
/// Builds report `code` from the fetched operations (op → reply).
diag::Report reportFor(const QString &code, const QHash<QString, QJsonObject> &data);
/// Runs `ops` one after the other through health-reader; replies that failed are left out.
void fetch(QObject *ctx, const QStringList &ops, std::function<void(QHash<QString, QJsonObject>, QStringList failed)> done);
}

class AdvancedHealthPage : public QWidget {
    Q_OBJECT
public:
    explicit AdvancedHealthPage(QWidget *parent = nullptr);

private:
    void updateMode();
    void run();
    void activeGpuQuery();
    void refreshIncidents();
    void selectIncident();
    void saveNote();
    bool busy_ = false;
    QComboBox *feature_, *category_, *incidentStatus_, *incidentFilter_;
    QPushButton *scan_, *gpuQuery_, *save_;
    QLabel *message_;
    QStackedWidget *stack_;
    DiagReportView *view_;
    QWidget *incidentPage_;
    QTreeWidget *incidents_;
    QTextBrowser *incidentInfo_;
    QPlainTextEdit *note_;
    QJsonObject lastEvidence_;
};
