#pragma once
// Existing diagnostic-history v1 archive reader. Bounded and read-only.
#include "diagnostichistory.h"
#include "diagreport.h"
#include "diagsources.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <algorithm>
namespace diagnostichistory {
struct Entry { QString id,code,title,boot,reportedBoot,time,locale,note; QJsonObject snapshot; };
struct Archive { QList<Entry> entries; int rejected=0; bool limited=false; };
inline bool validSnapshot(const QJsonObject &snap) {
    if(snap.value("version").toInt()!=1 || !snap.value("code").isString() || !snap.value("title").isString() || !snap.value("records").isArray()) return false;
    const auto level=[](const QJsonValue &v) {return v.isDouble()&&v.toDouble()>=0&&v.toDouble()<=4&&v.toDouble()==v.toInt(-1);};
    const auto strings=[](const QJsonValue &v) {if(!v.isArray()) return false;for(const auto &x:v.toArray()) if(!x.isString()) return false;return true;};
    if(!level(snap.value("verdict"))) return false;
    for(const auto &v:snap.value("records").toArray()) {
        if(!v.isArray()) return false;
        const auto r=v.toArray();if(r.isEmpty()) return false;
        const auto kind=r.at(0).toString();
        if(kind==QLatin1String("finding")) {if(r.size()!=5 || !level(r.at(1)) || !r.at(2).isString() || !r.at(3).isString() || !r.at(4).isString()) return false;}
        else if(kind==QLatin1String("table")) {if(r.size()!=4 || !r.at(1).isString() || !strings(r.at(2)) || !r.at(3).isString()) return false;}
        else if(kind==QLatin1String("row")) {if(r.size()!=5 || !r.at(1).isString() || !strings(r.at(2)) || !strings(r.at(3)) || !level(r.at(4))) return false;}
        else if(kind==QLatin1String("limit")) {if(r.size()!=2 || !r.at(1).isString()) return false;}
        else return false;
    }
    return true;
}
inline QString full(const QString &s) { return QString::fromStdString(fullBoot(s.toStdString())); }
inline Archive load(const QString &dir) {
    Archive out;
    const auto files=QDir(dir).entryInfoList({QStringLiteral("*.json")},QDir::Files|QDir::NoSymLinks,QDir::Name);
    QSet<QString> ids,duplicates;
    qsizetype bytes=0; int fileCount=0;
    for(const auto &fi:files) {
        if(++fileCount>128 || bytes>=32*1024*1024) {out.limited=true;break;}
        if(fi.size()>32*1024*1024-bytes) {out.limited=true;break;}
        const auto data=diagsrc::readRecord(fi.absoluteFilePath(),qMin<qint64>(1024*1024,32*1024*1024-bytes));
        if(!data) {++out.rejected;continue;}
        bytes+=data->size();
        const auto o=QJsonDocument::fromJson(*data).object(); const auto entries=o.value("entries").toArray();
        if(o.value("version").toInt()!=1 || !o.value("entries").isArray() || entries.size()>20) {++out.rejected;continue;}
        for(const auto &v:entries) {
            const auto e=v.toObject(), snap=e.value("snapshot").toObject();
            const auto id=full(e.value("id").toString()), code=snap.value("code").toString();
            const auto expected=QString::fromLatin1(QCryptographicHash::hash(code.toUtf8(),QCryptographicHash::Sha256).toHex())+QStringLiteral(".json");
            if(id.isEmpty() || code.isEmpty() || code.size()>128 || expected!=fi.fileName() || !validSnapshot(snap)) {++out.rejected;continue;}
            if(ids.contains(id)) {duplicates.insert(id);++out.rejected;continue;}
            ids.insert(id);
            const auto boot=e.value("boot").toString();
            out.entries.append({id,code,diag::redactPrivateText(snap.value("title").toString()),full(boot),diag::redactPrivateText(boot).left(80),e.value("time").toString().left(80),e.value("locale").toString().left(32),diag::redactPrivateText(e.value("note").toString()).left(4096),snap});
        }
    }
    for(qsizetype i=out.entries.size();i>0;--i) if(duplicates.contains(out.entries.at(i-1).id)) {out.entries.removeAt(i-1);++out.rejected;}
    // Presentation order only. Wall timestamps are not monotonic causal evidence.
    std::sort(out.entries.begin(),out.entries.end(),[](const Entry&a,const Entry&b){return a.time==b.time?a.id<b.id:a.time>b.time;});
    return out;
}
}
