#include "../src/smartwatch.h"
#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSettings>
#include <QTemporaryDir>
#include <cstdlib>
#include <limits>

// Always active, including Release/NDEBUG. No helper or device access.
#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (false)
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    CHECK(dir.isValid());
    qputenv("XDG_STATE_HOME", dir.path().toUtf8());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    QCoreApplication::setOrganizationName("CenturionFixture");
    QCoreApplication::setApplicationName("SmartHistory");
    CHECK(!smart::automaticSamplingEnabled());
    smart::setAutomaticSamplingEnabled(true); CHECK(smart::automaticSamplingEnabled());
    smart::setAutomaticSamplingEnabled(false); CHECK(!smart::automaticSamplingEnabled());
    using S=smart::RecordStatus;
    const QString id="disk|1000|0123456789ab";
    const QString file=dir.filePath("centurion/smart.json");
    const auto top=std::numeric_limits<qint64>::max();
    QJsonObject good{{"id",id},{"model","disk"},{"media_errors",QJsonValue(top-1)},{"secret","do not store"}};
    QJsonObject failed{{"dev","/dev/fake"},{"error","read failed"}};
    CHECK(smart::record({}).status==S::NoUsableSamples);
    CHECK(smart::record(QJsonArray{failed}).status==S::NoUsableSamples);
    CHECK(!QFile::exists(file));
    CHECK(smart::record(QJsonArray{QJsonObject{{"id",id}}}).status==S::NoUsableSamples);
    CHECK(smart::record(QJsonArray{good,good}).status==S::NoUsableSamples);
    CHECK(smart::record(QJsonArray{good,failed}).status==S::Partial);
    CHECK(!smart::loadHistory().contains("sampled"));
    const auto sample=[](const QString &key) {return smart::loadHistory().value("disks").toObject().value(key).toObject().value("samples").toArray().last().toObject();};
    CHECK(smart::counter(sample(id),"media_errors")==top-1);
    CHECK(!sample(id).contains("secret"));
    good["media_errors"]=QJsonValue(top);
    const auto saved=smart::record(QJsonArray{good});
    CHECK(saved.status==S::Saved && saved.alerts.size()==1 && saved.alerts.first().endsWith("1"));
    CHECK(smart::counter(sample(id),"media_errors")==top);
    CHECK(smart::loadHistory().value("sampled").toString()==QDate::currentDate().toString(Qt::ISODate));
    CHECK(smart::record(QJsonArray{good}).alerts.isEmpty());
    CHECK(!smart::counter(QJsonObject{{"n",-1}},"n"));
    CHECK(!smart::counter(QJsonObject{{"n",1.5}},"n"));
    CHECK(!smart::counter(QJsonObject{{"n","12"}},"n"));
    QFile corrupt(file); CHECK(corrupt.open(QIODevice::WriteOnly|QIODevice::Truncate)); CHECK(corrupt.write("{bad")==4); corrupt.close();
    CHECK(smart::record(QJsonArray{good}).status==S::Unavailable);
    CHECK(corrupt.open(QIODevice::ReadOnly)); CHECK(corrupt.readAll()=="{bad");
    return 0;
}
