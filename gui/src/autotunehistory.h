#pragma once
// Separate user-local GUI run evidence; the solver's own history and signature
// are not modified. The same bounded atomic store used by tunejournal is reused.
#include "tunejournal.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QStringList>
namespace autotunehistory {
inline QJsonObject completed(QJsonObject run, const QString &outcome, const QJsonObject &report,
                             qint64 elapsed, int exitCode, const QStringList &timeline) {
    run["outcome"]=outcome;
    run["ended"]=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    run["elapsed_ms"]=elapsed;
    run["exit_code"]=exitCode;
    run["timeline"]=QJsonArray::fromStringList(timeline);
    if (!report.isEmpty()) {
        const auto bytes=QJsonDocument(report).toJson(QJsonDocument::Compact);
        run["report_sha256"]=QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
        run["report"]=report;
        // Keep a clearly labelled summary if the full report cannot fit the
        // single-record budget. Never silently pretend it is the full report.
        if(tunejournal::encode(QJsonArray{run}).size()>tunejournal::limit/2) {
            run.remove("report"); run["report_omitted"]=true;
            QJsonObject summary;
            for(const char *k:{"goal","goal_label","weights","profile_summary","timings_ms","prefit_context","calibration_evidence"})
                if(report.contains(QLatin1String(k))) summary.insert(QLatin1String(k),report.value(QLatin1String(k)));
            if(QJsonDocument(summary).toJson(QJsonDocument::Compact).size()<=65536) run["report_summary"]=summary;
        }
    }
    return run;
}
}
