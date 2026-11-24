#include "../src/cohistory.h"
#include "../src/ryzenprofilediff.h"
#include <QCoreApplication>
#include <cstdio>
#define CHECK(x) do { if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;} } while(0)
int main(int argc,char **argv){
    QCoreApplication app(argc,argv);
    const QString boot=QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject core{{"ccd",1},{"slot",3},{"value",-15},{"excluded",false}};
    QJsonObject a{{"event_id",QUuid::createUuid().toString(QUuid::WithoutBraces)},{"event_boot",boot},{"scope","isolated"},{"ccd",1},{"slot",3},{"core",0},{"value",-15},{"observed_result","pass"},{"pass",true}};
    auto b=a;b["event_id"]=QUuid::createUuid().toString(QUuid::WithoutBraces);b["observed_result"]="fail";b["pass"]=false;
    QJsonObject r{{"mode","per"},{"topology",QJsonArray{core}},{"attempts",QJsonArray{a,b}}};
    auto s=cohistory::summarizeRecord(r);CHECK(s.cells.size()==1);CHECK(s.cells.begin()->second.contradictory());
    r["attempts"]=QJsonArray{a,a};s=cohistory::summarizeRecord(r);CHECK(s.cells.empty()&&s.rejected==2);
    b["scope"]="verification";r["attempts"]=QJsonArray{a,b};s=cohistory::summarizeRecord(r);CHECK(s.cells.begin()->second.passed==1&&s.rejected==1);
    core["excluded"]=true;r["topology"]=QJsonArray{core};CHECK(cohistory::summarizeRecord(r).cells.empty());
    core["excluded"]=false;r["topology"]=QJsonArray{core,core};CHECK(cohistory::summarizeRecord(r).cells.empty());
    auto invalid=a;invalid["event_boot"]="not-a-uuid";r["topology"]=QJsonArray{core};r["attempts"]=QJsonArray{invalid};CHECK(cohistory::summarizeRecord(r).cells.empty());
    QJsonObject c1{{"ccd",0},{"slot",1},{"coper",-10},{"disabled",false}},c2{{"ccd",1},{"core",0},{"coper",QJsonValue::Null},{"disabled",true}};
    QJsonObject x{{"coall",-5},{"cores",QJsonArray{c1,c2}}},y{{"coall",-5},{"cores",QJsonArray{c2,c1}}};
    CHECK(ryzenprofilediff::normalize(x));CHECK(ryzenprofilediff::normalize(x)==ryzenprofilediff::normalize(y));
    y["cores"]=QJsonArray{c1,c1};CHECK(!ryzenprofilediff::normalize(y));
    c2["coper"]=1.25;y["cores"]=QJsonArray{c1,c2};CHECK(!ryzenprofilediff::normalize(y));
    return 0;
}
