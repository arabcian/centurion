// No hardware is accessed by these pure fixture tests.
#include "../src/advanceddiagnostics.h"
#include <QCoreApplication>
#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#define CHECK(expr) do { if (!(expr)) { qCritical() << "FAILED" << #expr << __LINE__; return 1; } } while(false)
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    using namespace advanceddiagnostics;
    const QString usb=usbLedger({QStringLiteral("boot=abc12345 seq=1 usb 1-2: device descriptor read/64, error -71"),
                                 QStringLiteral("boot=abc12345 seq=1 usb 1-2: device descriptor read/64, error -71"),
                                 QStringLiteral("USB keyboard connected normally")});
    CHECK(usb.contains(QStringLiteral("error -71")));
    CHECK(usb.count(QStringLiteral("error -71")) == 1);
    CHECK(!usb.contains(QStringLiteral("keyboard connected")));
    CHECK(usbLedger({QStringLiteral("everything normal")}).contains(QStringLiteral("coverage is limited")));
    const QJsonObject prior{{QStringLiteral("0000:01:00.0"),QJsonObject{{QStringLiteral("current_link_speed"),QStringLiteral("16.0 GT/s")},
                                               {QStringLiteral("current_link_width"),QStringLiteral("16")},
                                               {QStringLiteral("aer_dev_correctable"),QStringLiteral("12")}}}};
    const QJsonObject now{{QStringLiteral("0000:01:00.0"),QJsonObject{{QStringLiteral("current_link_speed"),QStringLiteral("8.0 GT/s")},
                                               {QStringLiteral("current_link_width"),QStringLiteral("16")},
                                               {QStringLiteral("aer_dev_correctable"),QStringLiteral("14")}}}};
    CHECK(linkDelta(prior,now,true).contains(QStringLiteral("delta: 2")));
    CHECK(linkDelta(prior,now,false).contains(QStringLiteral("No comparable same-boot")));
    QJsonArray boots;
    boots.append(QJsonObject{{QStringLiteral("boot"),QStringLiteral("a0000000")},
                             {QStringLiteral("state"),QStringLiteral("unclean")},
                             {QStringLiteral("duration"),240},
                             {QStringLiteral("events"),QJsonObject{{QStringLiteral("xid"),4}}}});
    CHECK(compareBoots(boots,{}).contains(QStringLiteral("No regression verdict")));
    CHECK(flightCoverage(boots).contains(QStringLiteral("No previous-boot flight samples")));
    boots.prepend(QJsonObject{{QStringLiteral("boot"),QStringLiteral("b0000000")},
                             {QStringLiteral("state"),QStringLiteral("clean")},
                             {QStringLiteral("duration"),3600},
                             {QStringLiteral("events"),QJsonObject{{QStringLiteral("xid"),1}}}});
    boots.prepend(QJsonObject{{QStringLiteral("boot"),QStringLiteral("c0000000")},
                             {QStringLiteral("state"),QStringLiteral("clean")},
                             {QStringLiteral("duration"),1800},
                             {QStringLiteral("events"),QJsonObject{{QStringLiteral("xid"),3}}}});
    const QString diff=compareBoots(boots,{});
    CHECK(diff.contains(QStringLiteral("xid:")));
    CHECK(diff.contains(QStringLiteral("change unknown")));
    CHECK(diff.contains(QStringLiteral("Unclean boot")));
    return 0;
}
