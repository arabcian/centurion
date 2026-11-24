#include "../src/diagnostichistorydata.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>
#define CHECK(x) do {if(!(x)){std::cerr<<"line "<<__LINE__<<" failed\n";return EXIT_FAILURE;}}while(false)
using namespace diagnostichistory;
static bool save(const QString &dir,const QString &code,const QJsonArray &entries) {
    QFile f(dir+QLatin1Char('/')+QString::fromLatin1(QCryptographicHash::hash(code.toUtf8(),QCryptographicHash::Sha256).toHex())+QStringLiteral(".json"));
    const auto data=QJsonDocument(QJsonObject{{"version",1},{"entries",entries}}).toJson();
    return f.open(QIODevice::WriteOnly)&&f.write(data)==data.size();
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);QTemporaryDir dir;CHECK(dir.isValid());
    diag::Report report;report.code=QStringLiteral("S01");report.title=QStringLiteral("Report");report.add(diag::Level::Ok,QStringLiteral("Finding"));
    const auto snapshot=diag::archiveSnapshot(report);CHECK(validSnapshot(snapshot));
    const auto bootA=QStringLiteral("12345678-abcd-1234-abcd-123456789abc"),bootB=QStringLiteral("12345678-abcd-1234-abcd-123456789abd");
    const auto entry=[&](const QString &id,const QString &boot) {return QJsonObject{{"id",id},{"boot",boot},{"time","2026-01-01T10:00:00Z"},{"locale","en"},{"snapshot",snapshot}};};
    auto a=entry(QStringLiteral("12345678-0000-0000-0000-000000000001"),bootA);
    auto b=entry(QStringLiteral("12345678-0000-0000-0000-000000000002"),bootB);
    auto c=entry(QStringLiteral("12345678-0000-0000-0000-000000000003"),QStringLiteral("12345678"));
    CHECK(save(dir.path(),report.code,QJsonArray{a,b,c}));
    auto archive=load(dir.path());CHECK(archive.entries.size()==3&&archive.rejected==0);
    CHECK(archive.entries.at(0).boot==bootA&&archive.entries.at(1).boot==bootB&&archive.entries.at(2).boot.isEmpty());
    CHECK(archive.entries.at(2).reportedBoot==QStringLiteral("12345678"));
    auto invalid=snapshot;invalid.insert("records",QJsonArray{QJsonArray{QStringLiteral("row")}});CHECK(!validSnapshot(invalid));
    CHECK(save(dir.path(),report.code,QJsonArray{a,a,b,c}));archive=load(dir.path());CHECK(archive.entries.size()==2&&archive.rejected==2);
    CHECK(save(dir.path(),QStringLiteral("WRONG"),QJsonArray{b}));archive=load(dir.path());CHECK(archive.entries.size()==2&&archive.rejected==3);
    return EXIT_SUCCESS;
}
