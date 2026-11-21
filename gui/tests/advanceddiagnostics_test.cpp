// Health → Evidence Lab / Investigations reports over fixtures: no hardware, root or GPU is touched.
#include "../src/advanceddiagnostics.h"
#include "../src/evidencereports.h"

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
        er::F2fsVolume v{"/dev/nvme0n1p3", "/", "rw,lazytime", "nvme0n1p3", {{"sb_status", "0x4"}, {"cp_status", "0x1"}}};
        CHECK(has(er::f2fsGuard({}, {v}, {}), Level::Attention, QStringLiteral("needing fsck")));
        v.sys = {{"sb_status", "0x0"}, {"cp_status", "0x8"}};
        CHECK(er::f2fsGuard({}, {v}, {}).verdict() == Level::Attention);
        v.sys = {{"sb_status", "0x0"}, {"cp_status", "0x81"}, {"errors", "0x0"}};
        CHECK(er::f2fsGuard({}, {v}, {}).verdict() == Level::Ok);
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
