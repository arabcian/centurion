#include "../src/diagnostichistorydata.h"
#include "../src/diagsources.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>
#include <unistd.h>
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
    CHECK(matchesScope(RecordSource::Report, !archive.entries.at(0).boot.isEmpty(), RecordSource::Report, BootScope::Identified));
    CHECK(!matchesScope(RecordSource::Report, !archive.entries.at(2).boot.isEmpty(), RecordSource::Report, BootScope::Identified));
    CHECK(matchesScope(RecordSource::Report, !archive.entries.at(2).boot.isEmpty(), RecordSource::All, BootScope::Unresolved));
    CHECK(matchesScope(RecordSource::HealthLog, false, RecordSource::HealthLog, BootScope::All));
    CHECK(!matchesScope(RecordSource::KernelLog, true, RecordSource::HealthLog, BootScope::All));

    auto invalid=snapshot;invalid.insert("records",QJsonArray{QJsonArray{QStringLiteral("row")}});CHECK(!validSnapshot(invalid));
    CHECK(save(dir.path(),report.code,QJsonArray{a,a,b,c}));archive=load(dir.path());CHECK(archive.entries.size()==2&&archive.rejected==2);
    CHECK(save(dir.path(),QStringLiteral("WRONG"),QJsonArray{b}));archive=load(dir.path());CHECK(archive.entries.size()==2&&archive.rejected==3);
    QFile oversized(dir.filePath(QStringLiteral("0000.json")));
    CHECK(oversized.open(QIODevice::WriteOnly) && oversized.resize(33LL << 20));
    oversized.close();
    archive = load(dir.path());
    CHECK(archive.entries.size() == 2 && archive.rejected == 4);
    qputenv("XDG_STATE_HOME", dir.path().toUtf8());
    CHECK(QDir().mkpath(diagsrc::stateDir()));
    const QByteArray event = "2026-01-01 10:00:00 boot=example seq=42 warn [1.2] complete line\n";
    const QByteArray padding((4 << 20) - event.size() - 1, 'x');
    const auto writeLog = [&](const QByteArray &prefix) {
        QFile f(diagsrc::stateDir() + QStringLiteral("/kernel.log"));
        const QByteArray data = prefix + event + padding + '\n';
        return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
    };
    CHECK(writeLog("previous\n"));
    const auto complete = diagsrc::savedKernel();
    CHECK(complete.size() == 1 && complete.first().seq == 42);
    // A valid-looking suffix still belongs to a partial first record.
    CHECK(writeLog("previous "));
    CHECK(diagsrc::savedKernel().isEmpty());
    CHECK(diagsrc::savedLog(diagsrc::SavedLog::Kernel, 10).files.at(0).state == diagsrc::LogFileState::Missing);
    const auto capped = diagsrc::savedLog(diagsrc::SavedLog::Kernel, 10).files.at(1);
    CHECK(capped.state == diagsrc::LogFileState::Read && capped.omittedBytes > 0 && capped.rejectedLines == 1);
    const auto put = [](const QString &name, const QByteArray &data) {
        QFile f(diagsrc::stateDir() + QLatin1Char('/') + name);
        return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
    };
    const auto healthLine = [](const QString &boot, int seq, const QByteArray &message) {
        return QByteArray("2026-01-01 10:00:00 boot=") + boot.toLatin1() + " seq="
            + QByteArray::number(seq) + " warn disk Disk | " + message + '\n';
    };
    const auto old = healthLine(bootA, 1, "old");
    const auto repeated = healthLine(bootA, 10, "repeat");
    CHECK(put(QStringLiteral("health.log.1"), old + repeated));
    CHECK(put(QStringLiteral("health.log"), repeated + healthLine(bootA, 2, "clock check")
              + healthLine(bootA, 10, "different evidence")
              + healthLine(bootA.left(8), 20, "legacy") + "bad line\nunfinished"));
    const auto health = diagsrc::savedLog(diagsrc::SavedLog::Health, 20);
    CHECK(health.files.at(0).state == diagsrc::LogFileState::Read);
    CHECK(health.files.at(1).rejectedLines == 1 && health.files.at(1).incompleteTail);
    CHECK(diagsrc::savedLog(diagsrc::SavedLog::Health, 2).omittedLines == 4);
    const auto day = diagsrc::savedLog(diagsrc::SavedLog::Health, 1, 4096, QStringLiteral("2026-01-01"));
    CHECK(day.lines.size() == 1 && day.lines.first().seq == 20 && day.omittedLines == 5);
    const auto anotherDay = diagsrc::savedLog(diagsrc::SavedLog::Health, 1, 4096, QStringLiteral("2026-01-02"));
    CHECK(anotherDay.lines.isEmpty() && anotherDay.omittedLines == 0);
    const auto records = recordedEvents(health, {});
    CHECK(records.size() == 5 && records.first().boot.isEmpty());
    CHECK(records.at(1).sequence == 10 && records.at(1).current && records.at(1).rotated);
    CHECK(records.at(2).sequence == 10 && records.at(2).id != records.at(1).id);
    CHECK(records.at(3).sequence == 2 && records.at(4).sequence == 1);
    CHECK(QFile::link(diagsrc::stateDir() + QStringLiteral("/health.log"),
                      diagsrc::stateDir() + QStringLiteral("/kernel.log.1")));
    CHECK(diagsrc::savedLog(diagsrc::SavedLog::Kernel, 10).files.at(0).state == diagsrc::LogFileState::Unreadable);
    const auto kernel = diagsrc::SavedLog::Kernel;
    const auto path = diagsrc::savedLogPath(kernel);
    CHECK(diagsrc::clearSavedLog(kernel));
    CHECK(diagsrc::appendSavedLog(kernel, "previous\n", 4));
    CHECK(diagsrc::appendSavedLog(kernel, "current\n", 4));
    CHECK(diagsrc::readRecord(path, 64).value_or(QByteArray()) == "current\n");
    CHECK(diagsrc::readRecord(path + QStringLiteral(".1"), 64).value_or(QByteArray()) == "previous\n");
    CHECK(diagsrc::clearSavedLog(kernel));
    CHECK(QFile::link(diagsrc::savedLogPath(diagsrc::SavedLog::Health), path));
    CHECK(!diagsrc::appendSavedLog(kernel, "rejected\n", 4));
    CHECK(diagsrc::clearSavedLog(kernel));
    const auto kernelPath = QFile::encodeName(path);
    const auto rotatedPath = kernelPath + ".1";
    CHECK(put(QStringLiteral("kernel.log"), event + "2026-01-01 10:00:00 boot=example seq=43 warn [1.2] invalid "
              + QByteArray(1, char(0xff)) + '\n'));
    CHECK(::link(kernelPath.constData(), rotatedPath.constData()) == 0);
    const auto aliased = diagsrc::savedLog(kernel, 10);
    CHECK(aliased.lines.size() == 1 && aliased.lines.first().seq == 42);
    CHECK(aliased.files.at(0).rejectedLines == 1);
    CHECK(aliased.files.at(1).state == diagsrc::LogFileState::Unreadable);
    return EXIT_SUCCESS;
}
