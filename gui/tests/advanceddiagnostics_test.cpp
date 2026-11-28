// Health → Evidence Lab / Investigations reports over fixtures: no hardware, root or GPU is touched.
#include "../src/advanceddiagnostics.h"
#include "../src/evidencereports.h"
#include "../src/eventinfo.h"
#include "../src/componentreports.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

#define CHECK(expr) do { if (!(expr)) { qCritical() << "FAILED" << #expr << "line" << __LINE__; return 1; } } while (false)

using diag::Level;
namespace ad = advanceddiagnostics;
namespace er = evidencereports;

static bool has(const diag::Report &r, Level l, const QString &needle) {
    for (const diag::Finding &f : r.findings) if (f.level == l && (f.title + f.detail).contains(needle)) return true;
    return false;
}
static QJsonObject boot(const QString &id, qint64 start, const QString &state, double hours, const QJsonObject &versions, const QJsonObject &events = {},
                        const QJsonObject &curves = {}) {
    QJsonObject b{{"boot", id}, {"start", double(start)}, {"state", state}, {"duration", hours * 3600}, {"events", events}, {"curves", curves}};
    if (!versions.isEmpty()) b.insert(QStringLiteral("versions"), versions);
    return b;
}

int main(int argc, char **argv) {
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    qputenv("XDG_DATA_HOME", tmp.path().toUtf8());
    qputenv("XDG_STATE_HOME", tmp.path().toUtf8());
    QCoreApplication app(argc, argv);

    // Binomial tail: 6 of 6 events on a side with half the exposure is unlikely by luck; 3 of 6 is not.
    CHECK(ad::upperTail(6, 6, 0.5) < 0.02);
    CHECK(ad::upperTail(3, 6, 0.5) > 0.5);
    CHECK(ad::upperTail(0, 4, 0.3) == 1.0);

    // AER files as the kernel prints them ("TOTAL_%s %llu"), and the colon form.
    CHECK(ad::aerTotal(QStringLiteral("RxErr 0\nBadTLP 3\nTOTAL_ERR_COR 3\n"), QStringLiteral("TOTAL_ERR_COR")) == QStringLiteral("3"));
    CHECK(ad::aerTotal(QStringLiteral("TOTAL_ERR_COR: 14\n"), QStringLiteral("TOTAL_ERR_COR")) == QStringLiteral("14"));
    CHECK(ad::aerTotal(QStringLiteral("TOTAL_ERR_COR overflow"), QStringLiteral("TOTAL_ERR_COR")).isEmpty());

    // H01: a finding names the boots it is about, so the card can open Boot History on exactly those.
    {
        QJsonArray boots;
        qint64 t = 1000000;
        for (int i = 0; i < 6; ++i, t += 40000) boots.prepend(boot(QStringLiteral("old%1").arg(i), t, QStringLiteral("clean"), 10, {{"kernel", "6.12.1"}}));
        const QJsonObject events[] = {{{"xid", 3}}, {{"mce", 2}}, {}, {}};
        for (int i = 0; i < 4; ++i, t += 40000) boots.prepend(boot(QStringLiteral("new%1").arg(i), t, QStringLiteral("clean"), 5, {{"kernel", "6.13.0"}}, events[i]));
        const diag::Report r = ad::bootRegression(boots, {}, {{"kernel", "6.13.0"}});
        QStringList named;
        for (const diag::Finding &f : r.findings)
            if (f.title.contains(QStringLiteral("hardware faults"))) named = f.bootIds;
        CHECK(named == QStringList({QStringLiteral("new0"), QStringLiteral("new1")}));  // the boots with faults, not the quiet ones
    }

    // H01: unclean ends pile up after a kernel change.
    {
        QJsonArray boots;
        qint64 t = 1000000;
        for (int i = 0; i < 8; ++i, t += 40000) boots.prepend(boot(QStringLiteral("old%1").arg(i), t, QStringLiteral("clean"), 10, {{"kernel", "6.12.1"}}));
        for (int i = 0; i < 5; ++i, t += 40000) boots.prepend(boot(QStringLiteral("new%1").arg(i), t, QStringLiteral("unclean"), 4, {{"kernel", "6.13.0"}}));
        const diag::Report r = ad::bootRegression(boots, {}, {{"kernel", "6.13.0"}});
        CHECK(r.verdict() == Level::Attention);
        CHECK(has(r, Level::Attention, QStringLiteral("6.13.0")));
        // Same versions throughout: nothing to compare, no false alarm.
        QJsonArray same;
        for (int i = 0; i < 4; ++i) same.append(boot(QStringLiteral("s%1").arg(i), 1000 + i, QString::fromLatin1(i == 1 ? "unclean" : "clean"), 5, {{"kernel", "6.12.1"}}));
        CHECK(ad::bootRegression(same, {}, {}).verdict() == Level::Info);
        // No versions anywhere: Unknown, not "no regression".
        QJsonArray none{boot("a", 1, "clean", 3, {}), boot("b", 2, "clean", 3, {})};
        CHECK(ad::bootRegression(none, {}, {}).verdict() == Level::Unknown);
    }

    // H06: NVMe status codes and a harmless error log.
    {
        bool media = false;
        CHECK(ad::nvmeStatus(0x2002, &media) == QStringLiteral("Invalid Field in Command") && !media);
        CHECK(ad::nvmeStatus(0x0109) == QStringLiteral("Invalid Log Page"));
        CHECK(ad::nvmeStatus(0x0281, &media) == QStringLiteral("Unrecovered Read Error") && media);
        const QJsonObject nv{{"ok", true}, {"controllers", QJsonArray{QJsonObject{
            {"name", "nvme0"}, {"model", "Test SSD"}, {"smart_status", "ok"}, {"error_status", "read-only Error Information Log success"},
            {"smart", QJsonObject{{"critical_warning", 0}, {"temp_c", 41}, {"spare_pct", 100}, {"spare_threshold_pct", 10},
                                  {"life_used_pct", 3}, {"media_errors", 0}, {"error_log_entries", 1200}}},
            {"error_log", QJsonObject{{"entries", 2}, {"items", QJsonArray{QJsonObject{{"error_count", 1200}, {"status_field", 0x2002}, {"sqid", 0}},
                                                                          QJsonObject{{"error_count", 1199}, {"status_field", 0x2002}, {"sqid", 0}}}}}}}}}};
        const diag::Report r = ad::nvmeHealth(nv);
        CHECK(r.verdict() == Level::Ok);
        CHECK(has(r, Level::Info, QStringLiteral("1200 logged errors are rejected admin commands")));
        // Technical details are labelled lines, not JSON.
        CHECK(r.raw.contains(QStringLiteral("Temperature: 41 °C")) && r.raw.contains(QStringLiteral("Available spare: 100 %")));
        CHECK(r.raw.contains(QStringLiteral("Invalid Field in Command · admin queue")) && !r.raw.contains(QLatin1Char('{')));
        QJsonObject bad = nv;
        QJsonArray cs = bad.value(QStringLiteral("controllers")).toArray();
        QJsonObject c = cs.at(0).toObject();
        QJsonObject s = c.value(QStringLiteral("smart")).toObject();
        s.insert(QStringLiteral("critical_warning"), 0x04);
        c.insert(QStringLiteral("smart"), s);
        cs[0] = c;
        bad.insert(QStringLiteral("controllers"), cs);
        CHECK(ad::nvmeHealth(bad).verdict() == Level::Attention);
        CHECK(ad::nvmeHealth(QJsonObject{{"ok", false}, {"error", "denied"}}).verdict() == Level::Unknown);
    }

    // H10: one port failing repeatedly; the same kernel record in both logs counts once.
    {
        QList<diagsrc::LogLine> k;
        for (int i = 0; i < 4; ++i)
            k << diagsrc::LogLine{"2026-11-20 10:00:0" + QString::number(i), "abc12345", "err", {}, {}, "usb 3-2: device descriptor read/64, error -71", quint64(10 + i)};
        k << diagsrc::LogLine{"2026-11-20 10:01:00", "abc12345", "err", {}, {}, "usb usb1-port4: over-current condition", 20};
        const diag::Report r = ad::usbStability(diagsrc::mergeUnique(k, k), {{"3-2", "Logitech Receiver"}});
        CHECK(has(r, Level::Review, QStringLiteral("3-2")));
        CHECK(has(r, Level::Attention, QStringLiteral("1-4")));
        CHECK(r.tables.first().rows.first().value(3) == QStringLiteral("1") || r.tables.first().rows.at(1).value(3) == QStringLiteral("4"));
        CHECK(ad::usbStability({}, {}).verdict() == Level::Unknown);
    }

    // H12: a boot that went to sleep and never resumed; failed suspends this boot.
    {
        ad::SleepInputs in;
        in.currentBoot = QStringLiteral("cur-boot");
        in.memSleep = QStringLiteral("[s2idle] deep");
        in.stats = {{"success", "10"}, {"fail", "1"}, {"last_failed_dev", "0000:00:08.1"}, {"last_failed_errno", "-16"}, {"last_failed_step", "suspend"}};
        in.inflight = QStringLiteral("deadbeef-1111 1700000000 suspend");
        const QJsonArray boots{boot("deadbeef-1111", 1699990000, "unclean", 3, {}), boot("cur-boot", 1700050000, "running", 1, {})};
        const diag::Report r = ad::sleepMatrix(in, boots);
        CHECK(has(r, Level::Attention, QStringLiteral("never resumed")));
        CHECK(has(r, Level::Info, QStringLiteral("EBUSY")));
        ad::SleepInputs quiet;
        quiet.currentBoot = QStringLiteral("x");
        quiet.inflight = QStringLiteral("x 1700000000 suspend");  // this boot's own marker: not a dead boot
        CHECK(!has(ad::sleepMatrix(quiet, {}), Level::Attention, QStringLiteral("never resumed")));
    }

    // H13: an unreadable guard file is never "no guards"; attempts are counted from the array.
    {
        ad::CoRecords co;
        co.guardsValid = false;
        co.sessionPresent = true;
        co.session = QJsonObject{{"version", 3}, {"pending", true}, {"attempts", QJsonArray{1, 2, 3}}, {"trial", QJsonObject{{"what", "CCD0/2 -20"}}}};
        const diag::Report r = ad::coCrossLink(co, {}, {});
        CHECK(has(r, Level::Attention, QStringLiteral("crash-guard")));
        CHECK(has(r, Level::Review, QStringLiteral("3 attempts")));
        // More unclean ends under a CO profile than at stock.
        QJsonArray boots;
        for (int i = 0; i < 6; ++i) boots.append(boot(QStringLiteral("s%1").arg(i), i, "clean", 10, {}, {}, {{"cpu_co", "reset"}}));
        for (int i = 0; i < 4; ++i) boots.append(boot(QStringLiteral("p%1").arg(i), 100 + i, "unclean", 2, {}, {}, {{"cpu_co", "profile:tight"}}));
        CHECK(has(ad::coCrossLink(ad::CoRecords{}, boots, {}), Level::Review, QStringLiteral("profile:tight")));
    }

    // H15: 30 s cadence is not a gap; a real hole is.
    {
        QJsonArray flight;
        for (int t : {1000, 1030, 1060, 1090, 1200}) flight.append(QJsonObject{{"t", t}, {"cpu_c", 97}});
        QJsonObject b = boot("aaaa1111", 900, "unclean", 1, {});
        b.insert(QStringLiteral("flight"), flight);
        b.insert(QStringLiteral("flight_interval"), 30);
        const diag::Report r = ad::flightCoverage(QJsonArray{b});
        CHECK(has(r, Level::Review, QStringLiteral("1 gap")));
        CHECK(has(r, Level::Review, QStringLiteral("97")));
        CHECK(ad::flightCoverage({}).verdict() == Level::Unknown);
    }

    // H16: nvidia-smi -q output parsed by section.
    {
        const QString q = QStringLiteral(
            "==============NVSMI LOG==============\n\nAttached GPUs                             : 1\nGPU 00000000:01:00.0\n"
            "    ECC Mode\n        Current                           : N/A\n        Pending                           : N/A\n"
            "    Remapped Rows\n        Correctable Error                 : 0\n        Uncorrectable Error               : 2\n"
            "        Pending                           : Yes\n        Remapping Failure Occurred        : No\n");
        const diag::Report r = ad::gpuMemory(QJsonObject{{"xid", QJsonArray{QJsonObject{{"code", 31}}}}}, {QStringLiteral("0000:01:00.0 · GPU · active")}, q);
        CHECK(has(r, Level::Attention, QStringLiteral("Uncorrectable Error")));
        CHECK(has(r, Level::Review, QStringLiteral("Pending")));
        CHECK(has(r, Level::Ok, QStringLiteral("No memory-related Xid")));
        CHECK(has(ad::gpuMemory(QJsonObject{{"xid", QJsonArray{QJsonObject{{"code", 48}}}}}, {}, {}), Level::Attention, QStringLiteral("48")));
    }

    // H17: notes survive, keys are validated, only health.log lines are incidents.
    {
        CHECK(ad::saveIncident(QStringLiteral("123abcde/42"), QStringLiteral("Monitoring"), QStringLiteral("swapped the cable")).isEmpty());
        CHECK(ad::incidentNote(QStringLiteral("123abcde/42")).value(QStringLiteral("note")).toString() == QStringLiteral("swapped the cable"));
        CHECK(!ad::saveIncident(QStringLiteral("../../x"), QStringLiteral("New"), {}).isEmpty());
        CHECK(!ad::saveIncident(QStringLiteral("123abcde/43"), QStringLiteral("Bogus"), {}).isEmpty());
        const auto health = diagsrc::parseLine(QStringLiteral("2026-11-20 10:00:00  boot=123abcde seq=42  CRITICAL XID     Xid 79: GPU has fallen off the bus  |  NVRM: Xid (PCI:0000:01:00): 79"));
        const auto kernel = diagsrc::parseLine(QStringLiteral("2026-11-20 10:00:01  boot=123abcde seq=43  warn   [12.345678] some driver warning"));
        CHECK(health && health->kind == QStringLiteral("xid") && health->title.startsWith(QStringLiteral("Xid 79")));
        CHECK(kernel && kernel->kind.isEmpty() && kernel->text == QStringLiteral("some driver warning"));
        CHECK(!diagsrc::parseLine(QStringLiteral("2026-11-20 10:00:01  boot=123abcde seq=18446744073709551616 warn [12.3] invalid")));
        QTemporaryDir bounded; CHECK(bounded.isValid());
        const QString record=bounded.filePath(QStringLiteral("record.json"));
        { QFile f(record); CHECK(f.open(QIODevice::WriteOnly)); CHECK(f.write("{}") == 2); }
        CHECK(diagsrc::readRecord(record,2).has_value());
        CHECK(!diagsrc::readRecord(record,1).has_value());
        const QString link=bounded.filePath(QStringLiteral("link.json")); CHECK(QFile::link(record,link));
        CHECK(!diagsrc::readRecord(link,100).has_value());

        const auto list = ad::incidents({*health, *kernel}, ad::incidentNotes());
        CHECK(list.size() == 1 && list.first().status == QStringLiteral("Monitoring"));
    }

    // H18: findings of the checks are collected under their code.
    {
        diag::Report a;
        a.code = QStringLiteral("H04");
        a.add(Level::Attention, QStringLiteral("Xid 79 × 2"));
        diag::Report b;
        b.code = QStringLiteral("S04");
        b.add(Level::Ok, QStringLiteral("fine"));
        const diag::Report t = ad::triage(QStringLiteral("GPU"), {a, b});
        CHECK(t.verdict() == Level::Attention && has(t, Level::Attention, QStringLiteral("H04 · Xid 79")));
        CHECK(t.tables.first().rows.size() == 2);
        CHECK(ad::triage(QStringLiteral("GPU"), {b}).verdict() == Level::Ok);
        // Nothing could be read: never summed up as "no problem".
        diag::Report u;
        u.code = QStringLiteral("H16");
        u.add(Level::Unknown, QStringLiteral("no source"));
        diag::Report empty;
        empty.code = QStringLiteral("S06");
        CHECK(ad::triage(QStringLiteral("GPU"), {u}).verdict() == Level::Unknown);
        CHECK(ad::triage(QStringLiteral("GPU"), {u, empty}).verdict() == Level::Unknown);
        CHECK(ad::triage(QStringLiteral("GPU"), {u, b}).verdict() == Level::Ok);
    }

    // H02: errors on one core's own units blame that core; shared units blame nobody.
    {
        const QJsonObject scan{{"mce", QJsonArray{
            QJsonObject{{"text", "[Hardware Error]: CPU:5 (1a:44:0) MC5_STATUS[-|CE|MiscV|-|-|-|-|SyndV|-]: 0x9c2040000000011b"}, {"ts_us", 5e6}},
            QJsonObject{{"text", "[Hardware Error]: CPU:5 (1a:44:0) MC2_STATUS[-|CE|MiscV|-|-|-|-|SyndV|-]: 0x9c2040000000011b"}, {"ts_us", 6e6}}}}};
        QHash<int, er::CoreRef> cores{{5, {0, 2, 98304}}, {21, {0, 2, 98304}}, {8, {1, 0, 32768}}};
        const diag::Report r = er::mceAtlas(scan, cores);
        CHECK(has(r, Level::Review, QStringLiteral("CCD0 / core 2")));
        CHECK(!has(r, Level::Attention, QString()));
        const QJsonObject shared{{"mce", QJsonArray{QJsonObject{{"text", "[Hardware Error]: CPU:3 (1a:44:0) MC17_STATUS[-|UE|MiscV]: 0xbc"}}}}};
        const diag::Report s = er::mceAtlas(shared, cores);
        CHECK(has(s, Level::Attention, QStringLiteral("uncorrected")) && has(s, Level::Review, QStringLiteral("shared")));
        CHECK(er::mceAtlas(QJsonObject{{"mce", QJsonArray{}}}, {}).verdict() == Level::Ok);
    }

    // H03: deltas within one boot and one device identity only.
    {
        const QJsonObject prev{{"boot_id", "b1"}, {"counters", QJsonArray{QJsonObject{{"dev", "0000:01:00.0"}, {"vendor", "0x10de"}, {"device", "0x2c58"}, {"cor", 100}, {"nonfatal", 0}, {"fatal", 0}}}}};
        const QJsonObject now{{"boot_id", "b1"}, {"aer", QJsonArray{QJsonObject{{"dev", "0000:01:00.0"}, {"vendor", "0x10de"}, {"device", "0x2c58"}, {"cor", 130}, {"nonfatal", 0}, {"fatal", 0}}}}};
        const diag::Report r = er::aerDelta(now, prev);
        CHECK(r.tables.first().rows.first().value(2) == QStringLiteral("+30") && r.verdict() == Level::Review);
        QJsonObject other = prev;
        other.insert(QStringLiteral("boot_id"), QStringLiteral("b0"));
        CHECK(er::aerDelta(now, other).tables.first().rows.first().value(2) == QStringLiteral("—"));
        const QJsonObject fatal{{"boot_id", "b1"}, {"aer", QJsonArray{QJsonObject{{"dev", "0000:02:00.0"}, {"cor", 0}, {"nonfatal", 1}, {"fatal", 0}}}}};
        CHECK(er::aerDelta(fatal, {}).verdict() == Level::Attention);
        const QJsonObject missing{{"boot_id", "b1"}, {"aer", QJsonArray{QJsonObject{{"dev", "0000:02:00.0"}, {"cor", 0}}}}};
        CHECK(er::aerDelta(missing, {}).verdict() == Level::Unknown);
        QJsonObject changed = prev;
        auto counters = changed.value(QStringLiteral("counters")).toArray();
        auto endpoint = counters.first().toObject();
        endpoint.remove(QStringLiteral("device"));
        counters[0] = endpoint;
        changed.insert(QStringLiteral("counters"), counters);
        CHECK(er::aerDelta(now, changed).tables.first().rows.first().value(2) == QStringLiteral("other device"));
    }

    // H04: grouped by code with the meaning; Xid 154 shown as a case.
    {
        const QJsonObject scan{{"xid", QJsonArray{QJsonObject{{"kind", "xid"}, {"code", 79}, {"title", "Xid 79: GPU has fallen off the bus"}, {"ts_us", 1e6}},
                                                QJsonObject{{"kind", "xid"}, {"code", 79}, {"title", "Xid 79: GPU has fallen off the bus"}, {"ts_us", 2e6}},
                                                QJsonObject{{"kind", "xid"}, {"code", 13}, {"title", "Xid 13: Graphics Engine Exception"}, {"ts_us", 3e6}}}},
                               {"xid_cases", QJsonArray{}}};
        const diag::Report r = er::xidCases(scan);
        CHECK(r.verdict() == Level::Attention && r.tables.first().rows.size() == 2);
        CHECK(er::xidCases(QJsonObject{{"xid", QJsonArray{}}}).verdict() == Level::Ok);
    }

    // H05: F2FS's own need-fsck flag (superblock bit 2) and checkpoint error flag.
    {
        er::FsVolume v{"/dev/nvme0n1p3", "f2fs", "rw,lazytime", "nvme0n1p3", {"/"}, {{"sb_status", "0x4"}, {"cp_status", "0x1"}}};
        CHECK(has(er::fsGuard({}, {v}, {}), Level::Attention, QStringLiteral("needing fsck")));
        v.sys = {{"sb_status", "0x0"}, {"cp_status", "0x8"}};
        CHECK(er::fsGuard({}, {v}, {}).verdict() == Level::Attention);
        v.sys = {{"sb_status", "0x0"}, {"cp_status", "0x81"}, {"errors", "0x0"}};
        CHECK(er::fsGuard({}, {v}, {}).verdict() == Level::Ok);
        // The kernel writes the flags in hex without "0x": "42" is CRC_RECOVERY|ORPHAN_PRESENT, not decimal 42 (0x2A, error bit set).
        v.sys = {{"sb_status", "1"}, {"cp_status", "42"}};
        CHECK(er::fsGuard({}, {v}, {}).verdict() == Level::Ok);
        v.sys = {{"sb_status", "1"}, {"cp_status", "c8"}};
        CHECK(has(er::fsGuard({}, {v}, {}), Level::Attention, QStringLiteral("needing fsck")));
    }

    // H05: every disk file system, each judged by the error state it keeps itself.
    {
        er::FsVolume ext{"/dev/sda1", "ext4", "rw,relatime", "sda1", {"/home"}, {{"errors_count", "3"}, {"first_error_func", "ext4_lookup"}, {"first_error_time", "1700000000"}}};
        CHECK(has(er::fsGuard({}, {ext}, {}), Level::Attention, QStringLiteral("3 error(s)")));
        ext.sys = {{"errors_count", "0"}};
        CHECK(er::fsGuard({}, {ext}, {}).verdict() == Level::Ok);
        er::FsVolume btrfs{"/dev/nvme0n1p2", "btrfs", "rw,subvol=/@", "nvme0n1p2", {"/", "/home"}, {{"devices", "1"}, {"missing", "0"}, {"write_errs", "0"},
                                                                                                       {"read_errs", "0"}, {"corruption_errs", "7"}}};
        CHECK(has(er::fsGuard({}, {btrfs}, {}), Level::Attention, QStringLiteral("corruption 7")));
        btrfs.sys = {{"devices", "2"}, {"missing", "1"}, {"write_errs", "0"}};
        CHECK(has(er::fsGuard({}, {btrfs}, {}), Level::Attention, QStringLiteral("without 1 of its 2")));
        er::FsVolume zfs{"rpool/ROOT/default", "zfs", "rw,xattr", "default", {"/"}, {{"pool_state", "DEGRADED"}}};
        CHECK(has(er::fsGuard({}, {zfs}, {}), Level::Attention, QStringLiteral("rpool is DEGRADED")));
        // A file system that keeps no error state is judged by its kernel log; nearly full is worth a look.
        er::FsVolume fat{"/dev/nvme0n1p1", "vfat", "rw,fmask=0077", "nvme0n1p1", {"/boot/efi"}, {}, 1000, 900};
        CHECK(er::fsGuard({}, {fat}, {}).verdict() == Level::Ok);
        fat.avail = 20;
        CHECK(has(er::fsGuard({}, {fat}, {}), Level::Review, QStringLiteral("98% full")));
        // This boot's errors name the file system and device; a volume read-only after them needs attention.
        er::FsVolume xfs{"/dev/sdb1", "xfs", "ro,relatime", "sdb1", {"/data"}, {}};
        const QJsonObject scan{{"boot_id", "b1"}, {"fs_supported", QJsonArray{"ext4", "xfs"}}, {"fs_events", QJsonArray{
            QJsonObject{{"kind", "fs"}, {"text", "XFS (sdb1): Filesystem has been shut down due to log error (0x2)."}, {"ts_us", 5e6}}}}};
        const diag::Report r = er::fsGuard(scan, {xfs}, {});
        CHECK(has(r, Level::Attention, QStringLiteral("xfs on sdb1: 1 integrity error")));
        CHECK(has(r, Level::Attention, QStringLiteral("read-only after 1 error")));
        CHECK(r.tables.size() == 2 && r.tables.first().note.contains(QStringLiteral("ext4, xfs")));
        CHECK(r.tables.last().rows.first().at(2) == QStringLiteral("xfs"));
        // Earlier boots' errors from the saved log, any file system.
        diagsrc::LogLine old;
        old.boot = QStringLiteral("b0"); old.kind = QStringLiteral("fs"); old.text = QStringLiteral("exFAT-fs (sdc1): error, invalid access to FAT");
        CHECK(has(er::fsGuard(QJsonObject{{"boot_id", "b1"}}, {fat}, {old}), Level::Review, QStringLiteral("1 earlier boot")));
    }

    // H19–H26: this boot's kernel log per component, and the sysfs state that explains it.
    {
        namespace cr = componentreports;
        auto ev = [](const char *kind, const char *level, const char *title, const char *dev, const char *text) {
            return QJsonObject{{"kind", kind}, {"level", level}, {"title", title}, {"dev", dev}, {"text", text}, {"ts_us", 61e6}};
        };
        auto scanOf = [](const char *component, const QJsonArray &events) {
            return QJsonObject{{"boot_id", "b1"}, {"components", QJsonObject{{QLatin1String(component), events}}}};
        };
        // An older helper without component events, or an unreadable log: never "no problem".
        CHECK(cr::usb(QJsonObject{{"boot_id", "b1"}}).verdict() == Level::Unknown);
        CHECK(cr::usb(QJsonObject{{"components", QJsonObject{}}, {"components_error", "EPERM"}}).verdict() == Level::Unknown);
        CHECK(cr::usb(scanOf("usb", {})).verdict() == Level::Ok);
        // Kernel: a warning is a complaint (Review), an oops needs attention; taint B is bad memory.
        const QJsonObject warn = scanOf("kernel", {ev("oops", "error", "Kernel warning in amdgpu", "", "WARNING: CPU: 3 PID: 1 at x [amdgpu]")});
        CHECK(cr::kernel(warn, {}).verdict() == Level::Review);
        CHECK(cr::kernel(scanOf("kernel", {ev("lockup", "critical", "CPU lockup / RCU stall", "", "watchdog: BUG: soft lockup")}), {}).verdict() == Level::Attention);
        CHECK(has(cr::kernel(scanOf("kernel", {}), {1 << 5}), Level::Attention, QStringLiteral("bad memory page")));
        CHECK(cr::kernel(scanOf("kernel", {}), {(1 << 0) | (1 << 12)}).verdict() == Level::Ok);  // NVIDIA: P and O only
        CHECK(has(cr::kernel(scanOf("kernel", {}), {1 << 7}), Level::Review, QStringLiteral("Taint D")));
        // Storage: block I/O errors per disk, a degraded RAID array, failed commands counted by the disk.
        cr::StorageState st;
        st.disks << cr::Disk{"sda", "Samsung SSD 870", 500107862016, 4, false};
        st.mdstat = QStringLiteral("Personalities : [raid1]\nmd0 : active raid1 sdb1[1] sda1[0]\n      976630464 blocks super 1.2 [2/1] [U_]\n\nunused devices: <none>");
        const diag::Report sr = cr::storage(scanOf("storage", {ev("blk_io", "critical", "Block device I/O error", "sda", "I/O error, dev sda, sector 2048"),
                                                               ev("blk_io", "critical", "Block device I/O error", "sda", "I/O error, dev sda, sector 4096")}), st);
        CHECK(has(sr, Level::Attention, QStringLiteral("Read / write failed (2×): sda")));
        CHECK(has(sr, Level::Attention, QStringLiteral("md0 is degraded [U_]")));
        CHECK(has(sr, Level::Review, QStringLiteral("sda: 4 command(s) failed")));
        CHECK(sr.tables.size() == 3 && sr.tables.first().rows.size() == 1 && sr.tables.first().rows.first().at(2) == QStringLiteral("2"));
        // GPU, power: a critical temperature is attention; ACPI firmware errors alone are information.
        CHECK(cr::gpu(scanOf("gpu", {ev("amdgpu", "error", "AMD iGPU hang / reset", "amdgpu 0000:65:00.0", "amdgpu ring timeout")}), {}).verdict() == Level::Attention);
        CHECK(cr::power(scanOf("power", {ev("thermal", "critical", "Critical temperature reached", "thermal thermal_zone0", "critical temperature reached")}), {}).verdict() == Level::Attention);
        CHECK(cr::power(scanOf("power", {ev("thermal", "warn", "CPU package temperature above threshold", "CPU0", "Package temperature above threshold")}), {}).verdict() == Level::Review);
        const diag::Report acpi = cr::power(scanOf("power", {ev("acpi", "warn", "ACPI firmware error", "", "ACPI BIOS Error (bug): Could not resolve symbol")}), {});
        CHECK(acpi.verdict() != Level::Review && acpi.verdict() != Level::Attention && acpi.count(Level::Info) == 1);
        cr::PowerState ps;
        ps.zones << cr::ThermalZone{"thermal_zone1", "x86_pkg_temp", 97, 100} << cr::ThermalZone{"thermal_zone2", "acpitz", 0, 120};
        CHECK(has(cr::power(scanOf("power", {}), ps), Level::Attention, QStringLiteral("within 5 °C")));
        ps.zones.first().tempC = 60;
        CHECK(cr::power(scanOf("power", {}), ps).verdict() == Level::Ok);
        // Memory: EDAC counters and offline pages; OOM kills counted by the kernel even when the log rotated.
        cr::MemoryState ms;
        ms.edac << cr::EdacController{"mc0 · F19h", 3, 0};
        ms.hardwareCorruptedKib = 0;
        ms.oomKills = 2;
        const diag::Report mr = cr::memory(scanOf("memory", {}), ms);
        CHECK(has(mr, Level::Review, QStringLiteral("3 corrected memory error")) && has(mr, Level::Review, QStringLiteral("2 process(es) were killed")));
        ms.hardwareCorruptedKib = 8;
        CHECK(has(cr::memory(scanOf("memory", {}), ms), Level::Attention, QStringLiteral("8 kB of memory")));
        // USB, audio, firmware.
        CHECK(has(cr::usb(scanOf("usb", {ev("overcurrent", "error", "USB port over-current", "usb usb1-port3", "usb usb1-port3: over-current condition")})),
                  Level::Attention, QStringLiteral("usb usb1-port3")));
        const diag::Report ar = cr::audio(scanOf("audio", {}), QStringLiteral(" 0 [PCH            ]: HDA-Intel - HDA Intel PCH\n                      HDA Intel PCH at 0xfe000000 irq 150\n"));
        CHECK(ar.verdict() == Level::Ok && ar.tables.first().rows.first().at(1) == QStringLiteral("HDA-Intel"));
        CHECK(cr::audio(scanOf("audio", {}), QStringLiteral("--- no soundcards ---")).verdict() == Level::Review);
        const diag::Report fr = cr::firmware(scanOf("firmware", {ev("fw_missing", "warn", "Firmware file not found", "iwlwifi 0000:00:14.3",
                                                                    "iwlwifi 0000:00:14.3: Direct firmware load for iwlwifi-so-a0-gf-a0-89.ucode failed with error -2")}));
        CHECK(fr.verdict() == Level::Info && fr.tables.first().rows.first().at(1) == QStringLiteral("iwlwifi iwlwifi-so-a0-gf-a0-89.ucode"));
        CHECK(cr::firmware(scanOf("firmware", {ev("module", "error", "Kernel module does not match the kernel", "", "nvidia: Unknown symbol x")})).verdict() == Level::Attention);
    }

    // The kernel names file system and device in its own words, per driver.
    {
        auto is = [](const char *msg, const char *type, const char *dev) {
            const eventinfo::FsRef f = eventinfo::fsOfMessage(QString::fromLatin1(msg));
            return f.fstype == QLatin1String(type) && f.dev == QLatin1String(dev);
        };
        CHECK(is("EXT4-fs error (device nvme0n1p2): ext4_lookup:1855: inode #131073", "ext4", "nvme0n1p2"));
        CHECK(is("EXT4-fs (sda1): Remounting filesystem read-only", "ext4", "sda1"));
        CHECK(is("EXT2-fs error (device sdb1): ext2_check_page: bad entry", "ext2", "sdb1"));
        CHECK(is("Aborting journal on device sda1-8.", "ext4", "sda1"));
        CHECK(is("BTRFS error (device dm-0): bdev /dev/mapper/root errs: wr 0, rd 0, flush 0, corrupt 3, gen 0", "btrfs", "dm-0"));
        CHECK(is("BTRFS: error (device sda1: state EA) in __btrfs_free_extent:3072: errno=-5 IO failure", "btrfs", "sda1"));
        CHECK(is("XFS (sda1): Corruption detected. Unmount and run xfs_repair", "xfs", "sda1"));
        CHECK(is("F2FS-fs (nvme0n1p3): checkpoint failed, I/O error", "f2fs", "nvme0n1p3"));
        CHECK(is("FAT-fs (sdb1): error, fat_get_cluster: invalid cluster chain", "vfat", "sdb1"));
        CHECK(is("exFAT-fs (sdc1): error, invalid access to FAT", "exfat", "sdc1"));
        CHECK(is("ntfs3(sdb2): ino=5, Looks like your dir is corrupt", "ntfs3", "sdb2"));
        CHECK(is("bcachefs (nvme0n1p3): btree node read error", "bcachefs", "nvme0n1p3"));
        CHECK(is("SQUASHFS error: Unable to read fragment cache entry", "squashfs", ""));
        CHECK(is("usb 1-2: device descriptor read/64, error -71", "", ""));
        CHECK(eventinfo::fsRepairAction(QStringLiteral("exfat")).contains(QStringLiteral("fsck.exfat")));
        CHECK(eventinfo::fsRepairAction(QStringLiteral("btrfs")).contains(QStringLiteral("scrub")));
        CHECK(eventinfo::classifyLine(QStringLiteral("FAT-fs (sdb1): error, fat_get_cluster: invalid cluster chain")).value(QStringLiteral("kind")) == QStringLiteral("fs"));
        CHECK(eventinfo::classifyLine(QStringLiteral("FAT-fs (sdb1): Volume was not properly unmounted.")).isEmpty());
    }

    // H09: firmware load failure per adapter.
    {
        const QJsonObject scan{{"boot_id", "b1"}, {"radio_events", QJsonArray{
            QJsonObject{{"kind", "wifi"}, {"title", "Wi-Fi firmware load failure"}, {"text", "mt7925e 0000:03:00.0: Failed to load firmware"}}}}};
        CHECK(has(er::radioLedger(scan, {}), Level::Attention, QStringLiteral("mt7925e")));
        CHECK(er::radioLedger(QJsonObject{{"radio_events", QJsonArray{}}}, {}).verdict() == Level::Ok);
    }

    // The text export carries the verdict and every finding.
    {
        diag::Report r;
        r.code = QStringLiteral("X");
        r.title = QStringLiteral("T");
        r.add(Level::Review, QStringLiteral("something"), QStringLiteral("why"), QStringLiteral("do this"));
        const QString t = r.toText();
        CHECK(t.contains(QStringLiteral("something")) && t.contains(QStringLiteral("→ do this")) && t.contains(QStringLiteral("Review")));
    }
    return 0;
}
