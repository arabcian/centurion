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
class QLineEdit;
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
    /// Guided triage for a category ("gpu", "storage" …): selects it and runs it (read-only).
    void startTriage(const QString &category);
    /// Names the "Look next" links ("page:sleep", "tool:USB"); empty hides a link this system lacks.
    void setTargetLabel(std::function<QString(const QString &)> label) { targetLabel_ = std::move(label); }

Q_SIGNALS:
    /// A "Look next" link: a Health page or System tool (toolcatalog::relatedFor).
    void openTarget(const QString &target);

private:
    void showRelated(const QString &category);
    void updateMode();
    void run();
    void activeGpuQuery();
    void refreshIncidents();
    void selectIncident();
    void filterIncidents();
    void stashIncidentDraft();
    void updateDraftInfo();
    struct IncidentDraft { QString note, status; };
    QHash<QString, IncidentDraft> drafts_;
    QString loadedIncident_;
    IncidentDraft originalIncident_;
    QLineEdit *incidentSearch_;
    QLabel *incidentCount_, *draftInfo_;
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
    QWidget *related_ = nullptr;
    std::function<QString(const QString &)> targetLabel_;
};
