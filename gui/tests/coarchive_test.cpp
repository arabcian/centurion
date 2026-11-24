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
    QFile corrupt(path); CHECK(corrupt.open(QIODevice::WriteOnly|QIODevice::Truncate)); CHECK(corrupt.write("broken")==6); corrupt.close();
    CHECK(!coarchive::read(path)); CHECK(!coarchive::append(path,QJsonObject{{"id",47}}));
    CHECK(corrupt.open(QIODevice::ReadOnly)); CHECK(corrupt.readAll()=="broken");
    return 0;
}
