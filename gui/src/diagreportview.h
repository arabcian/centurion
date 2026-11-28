#pragma once
// Draws a diag::Report the way Health → Drive health draws a drive: a verdict line, one card per finding,
// the readings as tables, the limits of the check, and the raw readings behind a toggle. Copy and Save
// export Report::toText().
#include "diagreport.h"

#include <QWidget>
#include <memory>

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
    /// Sets the findings/readings search from outside (Evidence Lab incident focus).
    void setFilterText(const QString &text);
    /// Fewer controls above the report (Evidence Lab: five reports under one incident focus): the snapshot, history and
    /// evidence tools go into one "More" menu next to Copy / Save, the search box (the page's focus drives it) and the
    /// report's description line are hidden, and the "Showing …" line appears only while something is filtered out.
    void setCompact(bool compact);

Q_SIGNALS:
    /// A finding's fix button changed the system: the owner reads the report again.
    void fixApplied();
    /// A finding about boots was clicked: the owner opens Boot History with these boots (their ids) highlighted;
    /// `why` is the finding's title.
    void showBoots(const QStringList &bootIds, const QString &why);

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
    QPushButton *preview_, *pin_, *compare_, *archive_, *evidence_, *crossBoot_;
    QWidget *more_ = nullptr;
    class QHBoxLayout *actions_ = nullptr;
    bool compact_ = false;
    QString capturedAt_, captureBoot_;
    std::shared_ptr<QJsonObject> hardwareTreeState_ = std::make_shared<QJsonObject>();
    std::shared_ptr<QJsonObject> acpiTreeState_ = std::make_shared<QJsonObject>();
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
