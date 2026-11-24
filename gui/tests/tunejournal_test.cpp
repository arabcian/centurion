#include "../src/tunejournal.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <cstdlib>
static void check(bool b) { if (!b) std::abort(); }
int main(int argc, char **argv) {
    QCoreApplication app(argc,argv);
    QTemporaryDir dir; check(dir.isValid()); const auto path=dir.filePath("journal.json");
    QJsonObject record{{"id","one"},{"reply","pending"},{"requested",QJsonObject{{"cpu.epp","performance"}}}};
    check(tunejournal::store(path,record,false));
    check(!tunejournal::store(path,record,false));
    const auto first=tunejournal::read(path); check(first && first->size()==1);
    record["reply"]="reported_ok";
    record["receipts"]=QJsonArray{tunejournal::receipt(QJsonObject{{"key","cpu.epp"},{"ok",true},{"written",1},{"readback_expected",1},{"readback_matched",1},{"readback_missing",0},{"mismatch_count",0},{"error","private path"}})};
    check(tunejournal::store(path,record,true));
    const auto updated=tunejournal::read(path); check(updated && updated->at(0).toObject()==record);
    const auto receipt=record.value("receipts").toArray().at(0).toObject();
    check(!receipt.contains("error") && tunejournal::status(receipt)==tunereceipt::Status::Match);
    check(!(QFile::permissions(path)&(QFileDevice::ReadGroup|QFileDevice::ReadOther)));
    const auto pendingPath=dir.filePath("pending.json");
    for (int i=0; i<tunejournal::maxRows; ++i)
        check(tunejournal::store(pendingPath,QJsonObject{{"id",QString::number(i)},{"reply","pending"}},false));
    check(!tunejournal::store(pendingPath,QJsonObject{{"id","overflow"},{"reply","pending"}},false));
    check(tunejournal::store(pendingPath,QJsonObject{{"id","0"},{"reply","reported_ok"}},true));
    check(tunejournal::store(pendingPath,QJsonObject{{"id","overflow"},{"reply","pending"}},false));
    const auto pending=tunejournal::read(pendingPath); check(pending && pending->size()==tunejournal::maxRows);
    check(pending->at(0).toObject().value("id").toString()==QStringLiteral("1"));
    QFile f(path); check(f.open(QIODevice::WriteOnly|QIODevice::Truncate)); check(f.write("broken")==6); f.close();
    check(!tunejournal::store(path,record,true)); check(!tunejournal::read(path));
    check(f.open(QIODevice::ReadOnly) && f.readAll()=="broken");
}
