// No hardware is accessed by these pure fixture tests.
#include "../src/advanceddiagnostics.h"
#include <QCoreApplication>
#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#define CHECK(expr) do { if (!(expr)) { qCritical() << "FAILED" << #expr << __LINE__; return 1; } } while(false)
int main(int argc, char **argv) {
    QTemporaryDir privateData;
    CHECK(privateData.isValid());
    qputenv("XDG_DATA_HOME", privateData.path().toUtf8());
    qputenv("XDG_STATE_HOME", privateData.path().toUtf8());
    QCoreApplication app(argc, argv);
    using namespace advanceddiagnostics;
    // Actual Linux AER sysfs files contain labelled totals, not scalar values.
    CHECK(aerTotal(QStringLiteral("Receiver Error 2\nBad TLP 1\nTOTAL_ERR_COR: 14\n"),QStringLiteral("TOTAL_ERR_COR"))==QStringLiteral("14"));
    CHECK(aerTotal(QStringLiteral("TOTAL_ERR_NONFATAL: 0\n"),QStringLiteral("TOTAL_ERR_NONFATAL"))==QStringLiteral("0"));
    CHECK(aerTotal(QStringLiteral("Receiver Error 3"),QStringLiteral("TOTAL_ERR_COR")).isEmpty());
    CHECK(aerTotal(QStringLiteral("TOTAL_ERR_COR: overflow"),QStringLiteral("TOTAL_ERR_COR")).isEmpty());
    CHECK(saveIncident(QStringLiteral("123abcde/42"),QStringLiteral("Monitoring"),QStringLiteral("previous findings")) == QString());
    const QJsonObject saved=incidentNote(QStringLiteral("123abcde/42"));
    CHECK(saved.value(QStringLiteral("note")).toString()==QStringLiteral("previous findings"));
    CHECK(saved.value(QStringLiteral("status")).toString()==QStringLiteral("Monitoring"));
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
    const QJsonObject oldOther{{QStringLiteral("0000:01:00.0"),QJsonObject{
        {QStringLiteral("vendor"),QStringLiteral("0x10de")},
        {QStringLiteral("device"),QStringLiteral("0x2684")},
        {QStringLiteral("aer_dev_correctable"),QStringLiteral("20")}}}};
    const QJsonObject newOther{{QStringLiteral("0000:01:00.0"),QJsonObject{
        {QStringLiteral("vendor"),QStringLiteral("0x10de")},
        {QStringLiteral("device"),QStringLiteral("0x2c58")},
        {QStringLiteral("aer_dev_correctable"),QStringLiteral("25")}}}};
    CHECK(linkDelta(oldOther,newOther,true).contains(QStringLiteral("identity changed")));
    CHECK(!linkDelta(oldOther,newOther,true).contains(QStringLiteral("delta: 5")));
    QJsonArray boots;
    boots.append(QJsonObject{{QStringLiteral("boot"),QStringLiteral("a0000000")},
                             {QStringLiteral("state"),QStringLiteral("unclean")},
                             {QStringLiteral("duration"),240},
                             {QStringLiteral("events"),QJsonObject{{QStringLiteral("xid"),4}}}});
    CHECK(compareBoots(boots,{}).contains(QStringLiteral("No regression verdict")));
    CHECK(flightCoverage(boots).contains(QStringLiteral("No previous-boot flight samples")));
    QJsonArray flight;
    for (const int t : {1000,1030,1060,1090,1200}) flight.append(QJsonObject{{QStringLiteral("t"),t}});
    const QJsonArray retained{QJsonObject{{QStringLiteral("boot"),QStringLiteral("a0000000")},
        {QStringLiteral("state"),QStringLiteral("unclean")},
        {QStringLiteral("flight_interval"),30},{QStringLiteral("flight"),flight}}};
    const QString coverage=flightCoverage(retained);
    CHECK(coverage.contains(QStringLiteral("gaps >75s=1")));
    CHECK(!coverage.contains(QStringLiteral("gaps >15s=4")));
    // Header cadence may have been changed from 5s to 30s mid-boot.
    const QJsonArray changedCadence{QJsonObject{{QStringLiteral("boot"),QStringLiteral("b0000000")},
        {QStringLiteral("state"),QStringLiteral("unclean")},{QStringLiteral("flight_interval"),5},
        {QStringLiteral("flight"),flight}}};
    const QString uncertain=flightCoverage(changedCadence);
    CHECK(uncertain.contains(QStringLiteral("Cadence differs")));
    CHECK(uncertain.contains(QStringLiteral("gaps >75s=1")));
    boots.prepend(QJsonObject{{QStringLiteral("boot"),QStringLiteral("b0000000")},
                             {QStringLiteral("state"),QStringLiteral("clean")},
                             {QStringLiteral("duration"),3600},
                             {QStringLiteral("events"),QJsonObject{{QStringLiteral("xid"),1}}}});
    boots.prepend(QJsonObject{{QStringLiteral("boot"),QStringLiteral("c0000000")},
                             {QStringLiteral("state"),QStringLiteral("clean")},
                             {QStringLiteral("duration"),1800},
                             {QStringLiteral("events"),QJsonObject{{QStringLiteral("xid"),3}}}});
    // One retained boot/sequence in both log files must count once, not twice.
    const QString state=privateData.path()+QStringLiteral("/centurion");
    CHECK(QDir().mkpath(state));
    {
        QFile kernel(state+QStringLiteral("/kernel.log"));
        CHECK(kernel.open(QIODevice::WriteOnly));
        CHECK(kernel.write("boot=a0000000 seq=1 PM: suspend entry\n"
                           "boot=a0000000 seq=2 PM: resume error\n"
                           "boot=a0000000 seq=3 usb 1-2 reset SuperSpeed\n"
                           "boot=a0000000 seq=4 NVIDIA D3cold failed\n")>0);
    }
    {
        QFile health(state+QStringLiteral("/health.log"));
        CHECK(health.open(QIODevice::WriteOnly));
        CHECK(health.write("boot=a0000000 seq=2 PM: resume error\n")>0);
    }
    const QJsonObject sleepBoots{{QStringLiteral("boots"),QJsonArray{
        QJsonObject{{QStringLiteral("boot"),QStringLiteral("a0000000")},
                    {QStringLiteral("state"),QStringLiteral("unclean")}}}}};
    const QString sleep=healthReport(QStringLiteral("H12"),sleepBoots);
    CHECK(sleep.contains(QStringLiteral("a0000000 | 2 | 2 | 1 | 1 | unclean")));
    // Corrupt files are NOT zero-crash or cleared crash guards.
    {
        QFile guards(state+QStringLiteral("/co-search-crash-guards.json"));
        CHECK(guards.open(QIODevice::WriteOnly));
        CHECK(guards.write("{corrupt")>0);
    }
    CHECK(coCrossLink({},{}).contains(QStringLiteral("guard count UNKNOWN")));
    {
        QFile session(state+QStringLiteral("/co-search-session.json"));
        CHECK(session.open(QIODevice::WriteOnly));
        CHECK(session.write("{corrupt")>0);
    }
    CHECK(coCrossLink({},{}).contains(QStringLiteral("trial state UNKNOWN")));
    const QString diff=compareBoots(boots,{});
    CHECK(diff.contains(QStringLiteral("xid:")));
    CHECK(diff.contains(QStringLiteral("change unknown")));
    CHECK(diff.contains(QStringLiteral("Unclean boot")));
    return 0;
}
