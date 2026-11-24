#include "../src/hometab.h" // deliberately first: must work outside hometab.cpp and in moc consumers
#include "../src/profilediff.h"
#include "../src/ryzenprofilediff.h"
#include <QCoreApplication>
#include <type_traits>
#include <cstdlib>
static_assert(std::is_destructible_v<std::optional<sysinfo::PresentationReading>>);
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QJsonObject fw{{"ppt_pl1_spl",120},{"cpu_temp",89}};
    const QString text=profilediff::display(fw);
    if (!text.contains("CPU PL1 (W): 120") || !text.contains("CPU temperature") || text.startsWith('{')) return EXIT_FAILURE;
    if(profilediff::display(QJsonValue(QJsonValue::Undefined))==profilediff::display(QJsonValue(0)))return EXIT_FAILURE;
    const QJsonObject core{{"ccd",0},{"ccx",0},{"slot",1},{"coper",-20},{"disabled",false}};
    const auto normalized=ryzenprofilediff::normalize(QJsonObject{{"cores",QJsonArray{core}}});
    if(!normalized || !normalized->value("cores").toObject().contains("CCD 0 / CCX 0 / Slot 1"))return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
