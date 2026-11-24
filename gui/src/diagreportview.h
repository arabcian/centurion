#pragma once
// Draws a diag::Report the way Health → Drive health draws a drive: a verdict line, one card per finding,
// the readings as tables, the limits of the check, and the raw readings behind a toggle. Copy and Save
// export Report::toText().
#include "diagreport.h"

#include <QWidget>

class QLabel;
class QComboBox;
class QLineEdit;
class QScrollArea;
class QTreeWidget;
class QPushButton;
class QVBoxLayout;

class DiagReportView : public QWidget {
    Q_OBJECT
public:
    explicit DiagReportView(QWidget *parent = nullptr);
    void setReport(const diag::Report &r);
    /// A line in place of a report (before the first run, while one runs, or when it could not run).
    void setMessage(const QString &text);
    const diag::Report &report() const { return report_; }

Q_SIGNALS:
    /// A finding's fix button changed the system: the owner reads the report again.
    void fixApplied();

private:
    void clearBody();
    void filterReport();
    void previewExport();
    void archiveWorkspace();
    void evidenceWorkspace();
    QScrollArea *scroll_;
    QLineEdit *search_;
    QComboBox *severity_, *sections_;
    QLabel *filterInfo_, *snapshot_;
    QPushButton *preview_, *pin_, *compare_, *archive_, *evidence_;
    QString capturedAt_, captureBoot_;
    QString baselineCode_, baselineText_, baselineTime_;
    QList<QWidget *> findingWidgets_, sectionWidgets_;
    QList<QTreeWidget *> tables_;
    bool hasReport_ = false;
    diag::Report report_;
    QLabel *verdict_, *headline_, *about_;
    QPushButton *copy_, *save_;
    QWidget *body_;
    QVBoxLayout *bodyLayout_;
};
