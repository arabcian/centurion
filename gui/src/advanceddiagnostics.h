#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

// Observational tools only. All sampling happens after an explicit GUI click.
// Missing/permission-denied evidence is never converted into a zero measurement.
namespace advanceddiagnostics {
QString systemReport(const QString &code);
QString healthReport(const QString &code, const QJsonObject &boots = {},
                     const QJsonObject &evidence = {}, const QJsonObject &nvme = {});
QStringList incidentKeys();
QString incidentText(const QString &key);
QJsonObject incidentNote(const QString &key);
QString saveIncident(const QString &key, const QString &status, const QString &note);
// Pure parsers for independently runnable fixture tests.
QString compareBoots(const QJsonArray &boots, const QJsonObject &contexts);
QString flightCoverage(const QJsonArray &boots);
QString coCrossLink(const QJsonArray &boots, const QJsonObject &diagnostics);
QString usbLedger(const QStringList &lines);
QString linkDelta(const QJsonObject &before, const QJsonObject &after, bool sameBoot);
QString aerTotal(const QString &text, const QString &counter);
}
