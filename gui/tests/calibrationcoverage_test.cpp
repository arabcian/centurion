#include "../src/calibrationcoverage.h"
#include "../src/calibrationmath.h"
#include <cstdio>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
int main() {
    const auto a=QJsonObject{{"ph",0},{"t",100},{"bv",6},{"k","7.2"},{"cx","a"},{"ps","rapl"},{"w",1.0},{"y",QJsonArray{0.1,QJsonValue::Null,0.0,0.3}}};
    auto b=a; b["y"]=QJsonArray{QJsonValue::Null,0.1,"bad",0.4}; b.remove("w"); b["t"]=200; b.remove("bv");
    auto c=a; c["cx"]="b";
    const auto groups=calibrationcoverage::summarize(QJsonArray{a,b,c});
    CHECK(groups.size()==2);
    for (const auto &g:groups) if (g.fields.at(2)=="a") {
        CHECK(g.rows==2 && g.measured[0]==1 && g.measured[1]==1 && g.measured[2]==1 && g.measured[3]==2 && g.unknownWeight==1);
        CHECK(g.latest==200 && g.benchmarkVersions.contains(6) && g.unknownBenchmark==1);
    } else CHECK(g.rows==1);
    CHECK(calibrationcoverage::summarize({}).isEmpty());
    const auto prediction = calibrationmath::prediction(0.5,0.25,0.0);
    CHECK(prediction && prediction->error == -0.5 && prediction->z == -2 && prediction->withinTwoSigma);
    CHECK(!calibrationmath::prediction(0.5,0.0,0.0));
    CHECK(calibrationmath::ramMatches(16000,16320));
    CHECK(!calibrationmath::ramMatches(16000,16321));
    return 0;
}
