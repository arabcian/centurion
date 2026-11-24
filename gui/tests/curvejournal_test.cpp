#include "../src/curvejournal.h"
#include <QCoreApplication>
#include <cstdio>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); return 1; } } while(false)
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    QJsonObject device{{"schema",1},{"mapping","single_visible_physical_gpu"},{"uuid","GPU-01234567-89ab-cdef-0123-456789abcdef"},{"pci","00000000:01:00.0"},{"driver","610.1"},{"vbios","99.01"}};
    QJsonObject point{{"domain","gpu"},{"index",3},{"volt_uV",600000},{"freq_kHz",1200000},{"freq_offset_kHz",-120000}};
    QJsonObject reply{{"request_id","new"},{"gpu","GPU A"},{"device_identity",device},{"vf_curve",QJsonArray{point}},{"mem_offset_mhz",0}};
    QJsonObject data{{"curve_deltas",QJsonObject{{"3",-120000}}},{"mem_offset_mhz",0}};
    QJsonObject req{{"op","apply_gpu_offsets"},{"request_id","new"},{"expected_gpu","GPU A"},{"expected_device",device},{"expected_points",QJsonArray{QJsonArray{3,600000}}},{"profile_data",data}};
    auto status=[&](bool reached=true,bool ok=true){return curvejournal::inspect(req,reply,reached,ok).value("status").toString();};
    CHECK(curvereadback::verifiedDevice(device)); CHECK(status()==QStringLiteral("point_match_only"));
    CHECK(status(false)==QStringLiteral("unknown")); CHECK(status(true,false)==QStringLiteral("helper_failed"));
    reply["request_id"]="old"; CHECK(status()==QStringLiteral("readback_missing")); reply["request_id"]="new";
    auto other=device; other["uuid"]="GPU-ffffffff-89ab-cdef-0123-456789abcdef"; reply["device_identity"]=other; CHECK(status()==QStringLiteral("device_changed")); reply["device_identity"]=device;
    point["freq_offset_kHz"]=-110000; reply["vf_curve"]=QJsonArray{point}; CHECK(status()==QStringLiteral("point_or_memory_mismatch"));
    data["flatten_from"]=3; req["profile_data"]=data; CHECK(status()==QStringLiteral("observed_only"));
    QJsonObject planPoint{{"index",3},{"volt_uV",600000},{"base_kHz",1310000},{"requested_delta_kHz",-110000},{"projected_kHz",1200000}};
    QJsonObject plan{{"schema",1},{"device_identity",device},{"scope","gpu_point_plan_only"},{"points",QJsonArray{planPoint}}};
    reply["apply_plan"]=plan;
    CHECK(status()==QStringLiteral("point_match_only"));
    planPoint["requested_delta_kHz"]=-120000; planPoint["projected_kHz"]=1190000;
    plan["points"]=QJsonArray{planPoint}; reply["apply_plan"]=plan;
    CHECK(status()==QStringLiteral("point_or_memory_mismatch"));
    planPoint["projected_kHz"]=1190001; plan["points"]=QJsonArray{planPoint}; reply["apply_plan"]=plan;
    CHECK(status()==QStringLiteral("observed_only"));
    reply.remove("apply_plan");
    point["volt_uV"]=625000; reply["vf_curve"]=QJsonArray{point}; CHECK(status()==QStringLiteral("layout_changed"));
    other["uuid"]="broken"; CHECK(!curvereadback::verifiedDevice(other));
    return 0;
}
