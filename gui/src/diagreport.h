#pragma once
// The result of one Health diagnostic as a reader needs it: a verdict, findings in plain language (what was
// seen, why it matters, what to do), tables of the readings, the limits of what the sources can tell, and
// the raw text the verdict came from. Qt Core only, so every diagnostic stays testable without widgets;
// DiagReportView (diagreportview.h) draws it, toText() is the copy / save format.
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace diag {

/// Ordered by how much a reader should care. Unknown sits above Ok: a source that could not be read must
/// never be summed up as "no problems".
enum class Level { Ok, Info, Unknown, Review, Attention };

struct Finding {
    Level level = Level::Info;
    QString title;    // one line: what was seen
    QString detail;   // why it matters / what it means
    QString action;   // what to do (empty = nothing to do)
    // A change that does it (sysadmin-helper request, see sysadmin.h); empty = only the text above.
    QString fixLabel;
    QJsonObject fix;
    void setFix(const QString &label, const QJsonObject &request) { fixLabel = label; fix = request; }
};

struct Table {
    QString title;
    QStringList headers;
    QList<QStringList> rows;
    QList<Level> levels;  // one per row; colours the row's first cell
    QString note;
    // Optional tree presentation: one parent row index per row, strictly earlier or -1.
    QList<int> parents;
    QStringList rowKeys; // optional transient identities; empty entries must never restore selection
    bool privateBinary = false;
    QString viewKind; // transient stable presentation ID, not exported as evidence
    void add(const QStringList &row, Level l = Level::Info) { rows << row; levels << l; }
};

struct Report {
    QString code, title, about;
    QList<Finding> findings;
    QList<Table> tables;
    QStringList limits;   // what these sources cannot tell
    QString raw;          // the readings the findings came from (technical details)

    Finding &add(Level l, const QString &title, const QString &detail = {}, const QString &action = {});
    int count(Level l) const;
    /// Attention or Review when a finding has it; else Ok when anything was checked and found fine;
    /// else Unknown when sources were missing; else Info.
    Level verdict() const;
    /// One line for the top of the report, e.g. "2 findings need attention".
    QString headline() const;
    QString toText() const;
    QString toTextWithVerdict(const Report &original) const;
};

QString levelName(Level l);
/// Conservative best-effort privacy filter, not an anonymization guarantee.
QString redactPrivateText(QString text);
void redactPrivateFields(Report &report);
// Structured, redacted snapshot for local history: no raw dumps or executable fixes.
QJsonObject archiveSnapshot(const Report &report);
// Empty difference for equal unordered records; incompatible schema is explicit.
QStringList archiveDiff(const QJsonObject &before, const QJsonObject &after);

}  // namespace diag
