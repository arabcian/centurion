#include "../src/autotunehistory.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <cstdlib>
static void check(bool b){if(!b)std::abort();}
int main(int argc,char **argv){
    QCoreApplication app(argc,argv);QTemporaryDir dir;check(dir.isValid());const auto path=dir.filePath("runs.json");
    const QJsonObject start{{"id","one"},{"outcome","pending"},{"request",QJsonObject{{"goal","gaming"}}}};
    check(tunejournal::store(path,start,false));check(!tunejournal::store(path,start,false));
    const QJsonObject report{{"ok",true},{"preset",QJsonObject{{"values",QJsonObject{{"vm.swappiness",60}}}}}};
    auto end=autotunehistory::completed(start,QStringLiteral("draft_changed"),report,500,0,{QStringLiteral("Result received")});
    check(end.value("report").toObject()==report && end.value("report_sha256").toString().size()==64);
    check(end.value("outcome")==QStringLiteral("draft_changed") && end.value("elapsed_ms").toInteger()==500);
    check(tunejournal::store(path,end,true));const auto saved=tunejournal::read(path);check(saved && saved->size()==1 && saved->at(0).toObject()==end);
    const auto failed=autotunehistory::completed(start,QStringLiteral("timeout"),{},1000,-1,{});check(!failed.contains("report") && failed.value("outcome")==QStringLiteral("timeout"));
    const auto large=autotunehistory::completed(start,QStringLiteral("staged"),QJsonObject{{"payload",QString(3<<20,QChar('x'))},{"goal","gaming"}},10,0,{});
    check(large.value("report_omitted").toBool() && !large.contains("report") && large.value("report_summary").toObject().value("goal")==QStringLiteral("gaming"));
    check(!tunejournal::store(dir.filePath("absent.json"),end,true));
}
