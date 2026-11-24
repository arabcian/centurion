#include "../src/coarchive.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <cstdio>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir; CHECK(dir.isValid());
    const auto path = dir.filePath("co-search-runs.json");
    CHECK(coarchive::read(path)->isEmpty());
    for (int i=0;i<45;++i) CHECK(coarchive::append(path, QJsonObject{{"id",i}}));
    auto rows=coarchive::read(path); CHECK(rows && rows->size()==40 && rows->first().toObject().value("id").toInt()==5);
    QJsonArray attempts;
    for (int i=0;i<80;++i) attempts << QJsonObject{{"note", QString(20000, QLatin1Char('x'))}};
    CHECK(coarchive::append(path,QJsonObject{{"id",46},{"attempts",attempts}}));
    rows=coarchive::read(path); CHECK(rows && QFile(path).size()<=coarchive::limit);
    const auto last=rows->last().toObject(); CHECK(last.value("omitted_attempts").toInt()>0);
    CHECK(last.value("attempts").toArray().size()+last.value("omitted_attempts").toInt()==80);
    const auto frozen=*rows;
    CHECK(coarchive::append(path,QJsonObject{{"id",48}}));
    CHECK(!coarchive::remove(path,frozen,0));
    rows=coarchive::read(path); CHECK(rows && coarchive::remove(path,*rows,0));
    CHECK(coarchive::read(path)->size()==rows->size()-1);
    const QJsonObject c1{{"ccd",0},{"slot",1},{"value",-10}}, c2{{"ccd",1},{"slot",0},{"value",-20}};
    QJsonObject a{{"cpu_fingerprint","fixture"},{"topology",QJsonArray{c1,c2}}};
    QJsonObject b{{"cpu_fingerprint","fixture"},{"topology",QJsonArray{c2,c1}}};
    CHECK(coarchive::compare(a,b).last()==QStringLiteral("No recorded offset differences."));
    b["cpu_fingerprint"]="other"; CHECK(coarchive::compare(a,b).first().contains("different CPU"));
    QFile corrupt(path); CHECK(corrupt.open(QIODevice::WriteOnly|QIODevice::Truncate)); CHECK(corrupt.write("broken")==6); corrupt.close();
    CHECK(!coarchive::read(path)); CHECK(!coarchive::remove(path,frozen,0)); CHECK(!coarchive::append(path,QJsonObject{{"id",47}}));
    CHECK(corrupt.open(QIODevice::ReadOnly)); CHECK(corrupt.readAll()=="broken");
    return 0;
}
