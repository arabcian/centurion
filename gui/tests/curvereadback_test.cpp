#include "../src/curvereadback.h"
#include <QCoreApplication>
#include <cstdio>
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); return 1; } } while(false)
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    const QJsonObject a{{"domain","gpu"},{"index",3},{"volt_uV",600000},{"freq_kHz",1200000},{"freq_offset_kHz",-120000},{"base_kHz",1320000}};
    auto b = a; b["index"] = 8; b["volt_uV"] = 625000;
    QJsonObject o{{"request_id","new"},{"gpu","GPU A"},{"vf_curve",QJsonArray{a,b}}};
    const auto id = curvereadback::identity(o,QStringLiteral("new"));
    CHECK(id && id->indices == (QVector<int>{3,8}) && id->voltages == (QVector<qint64>{600000,625000}));
    CHECK(!curvereadback::identity(o,QStringLiteral("old")));
    o["vf_curve"] = QJsonArray{a,a}; CHECK(!curvereadback::identity(o,QStringLiteral("new")));
    o["vf_curve"] = QJsonArray{b,a}; CHECK(!curvereadback::identity(o,QStringLiteral("new")));
    b.remove("freq_offset_kHz"); o["vf_curve"] = QJsonArray{a,b}; CHECK(!curvereadback::identity(o,QStringLiteral("new")));
    o.remove("request_id"); CHECK(!curvereadback::identity(o,QStringLiteral("new")));
    return 0;
}
