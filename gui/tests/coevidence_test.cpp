// Unit test for the CO Search crash watcher's log parsing and blame (coevidence.cpp), and for how the search
// uses a blame (costrategy.cpp: believed once per search). Plain asserts, like costrategy_test.cpp.
#include "../src/coevidence.h"

#include <QCoreApplication>
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

using namespace coevidence;
using cosearch::Core;

/// 2 CCDs x 4 cores with SMT: core k = CPU k and k + 8.
static QList<Core> cpu8() {
    QList<Core> out;
    for (int i = 0; i < 8; ++i) out << Core{i / 4, i % 4, {i, i + 8}, -1};
    return out;
}

static Record kern(const char *line) { return *fromKernelText(QString::fromLatin1(line), QStringLiteral("watch-kernel")); }

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    // Parsing: the kernel's record and the AMD decoder's, corrected and uncorrected.
    {
        auto r = fromKernelText("mce: [Hardware Error]: CPU 5: Machine Check: 0 Bank 5: bea0000000000108", "boot-kernel");
        CHECK(r && r->kind == "mce" && r->cpu == 5 && r->bank == 5 && r->coreUnit && !r->uncorrected);
        r = fromKernelText("mce: [Hardware Error]: CPU 3: Machine Check Exception: 5 Bank 1: b2000000000c0150", "pstore");
        CHECK(r && r->cpu == 3 && r->bank == 1 && r->coreUnit && r->uncorrected);
        r = fromKernelText("[Hardware Error]: CPU:13 (19:61:2) MC6_STATUS[Over|CE|MiscV|AddrV|-|-|SyndV|-|-|-]: 0xdc2041000000011b", "watch-kernel");
        CHECK(r && r->cpu == 13 && r->bank == 6 && r->coreUnit && !r->uncorrected);
        r = fromKernelText("[Hardware Error]: CPU:2 (19:61:2) MC17_STATUS[-|UE|MiscV|AddrV|PCC|-|SyndV|-|-|-]: 0xbea0000000000108", "boot-kernel");
        CHECK(r && r->cpu == 2 && r->bank == 17 && !r->coreUnit && r->uncorrected);  // memory controller: shared
        r = fromKernelText("watchdog: BUG: soft lockup - CPU#7 stuck for 22s! [centurion-cpust:1234]", "watch-kernel");
        CHECK(r && r->kind == "lockup" && r->cpu == 7);
        r = fromKernelText("NMI watchdog: Watchdog detected hard LOCKUP on cpu 9", "pstore");
        CHECK(r && r->kind == "lockup" && r->cpu == 9);
        // Lines that name no CPU / bank, or are not machine checks at all.
        CHECK(!fromKernelText("mce: [Hardware Error]: Machine check events logged", "boot-kernel"));
        CHECK(!fromKernelText("[Hardware Error]: IPID: 0x000500b000000000, Syndrome: 0x000000005a020001", "boot-kernel"));
        CHECK(!fromKernelText("[Hardware Error]: Execution Unit Ext. Error Code: 0", "boot-kernel"));
        CHECK(!fromKernelText("usb 1-2: new high-speed USB device number 3", "pstore"));
        // Threshold counters: the unit name decides whether it is the core's own.
        auto c = fromCounterPath("/sys/devices/system/machinecheck/machinecheck5/execution_unit/misc0/error_count");
        CHECK(c && c->cpu == 5 && c->unit == "execution_unit" && c->coreUnit);
        c = fromCounterPath("/sys/devices/system/machinecheck/machinecheck2/l3_cache/misc0/error_count");
        CHECK(c && c->cpu == 2 && !c->coreUnit);
        CHECK(!fromCounterPath("/sys/devices/system/machinecheck/machinecheck2/l3_cache/misc0/threshold_limit"));
        // A journal round trip keeps everything; junk is refused.
        const Record k = kern("mce: [Hardware Error]: CPU 5: Machine Check: 0 Bank 5: bea0000000000108");
        const auto back = Record::fromJson(k.toJson());
        CHECK(back && back->cpu == 5 && back->bank == 5 && back->coreUnit && back->source == "watch-kernel");
        CHECK(!Record::fromJson(QJsonObject{{"kind", "rm -rf"}, {"cpu", 1}}));
    }

    const QList<Core> cores = cpu8();
    const QList<int> set{-20, -18, -25, 0, -15, -22, -10, -30};  // core 3 (CPU 3 / 11) at stock

    // Blame: one core's own unit, undervolted; SMT siblings count as the same core.
    {
        const Verdict v = judge({kern("mce: [Hardware Error]: CPU 5: Machine Check: 0 Bank 5: bea0000000000108"),
                                 kern("[Hardware Error]: CPU:13 (19:61:2) MC6_STATUS[Over|CE|MiscV|-|-|-|-|-|-|-]: 0x1")},
                                cores, set);
        CHECK(v.kind == Verdict::Blamed && v.core == 5 && v.cpus == QList<int>{5});
        CHECK(v.summary.contains("CCD1/1"));
    }
    // False-positive protection.
    {
        CHECK(judge({}, cores, set).kind == Verdict::NoEvidence);
        // Lockups never blame, even alone.
        const Verdict l = judge({kern("watchdog: BUG: soft lockup - CPU#7 stuck for 22s! [x:1]")}, cores, set);
        CHECK(l.kind == Verdict::NoEvidence && l.summary.contains("Lockup"));
        // A shared unit (memory controller) next to a core record: nobody.
        CHECK(judge({kern("mce: [Hardware Error]: CPU 5: Machine Check: 0 Bank 5: 1"),
                     kern("[Hardware Error]: CPU:2 (19:61:2) MC17_STATUS[-|UE|-|-|-|-|-|-|-|-]: 0x1")}, cores, set).kind
              == Verdict::SharedUnit);
        CHECK(judge({*fromCounterPath("/sys/devices/system/machinecheck/machinecheck2/l3_cache/misc0/error_count")}, cores, set).kind
              == Verdict::SharedUnit);
        // Two different cores: ambiguous.
        CHECK(judge({kern("mce: [Hardware Error]: CPU 5: Machine Check: 0 Bank 5: 1"),
                     kern("mce: [Hardware Error]: CPU 1: Machine Check: 0 Bank 0: 1")}, cores, set).kind == Verdict::Ambiguous);
        // A core at stock (CPU 11 = core 3): contradicts the Curve Optimizer as the cause.
        CHECK(judge({kern("mce: [Hardware Error]: CPU 11: Machine Check: 0 Bank 5: 1")}, cores, set).kind == Verdict::AtStock);
        // A CPU this search does not know: ignored.
        CHECK(judge({kern("mce: [Hardware Error]: CPU 40: Machine Check: 0 Bank 5: 1")}, cores, set).kind == Verdict::NoEvidence);
    }

    // The search believes a blame once: the named core backs off by 2; a second blamed crash backs off all.
    {
        using namespace cosearch;
        Search s(Mode::PerCore, Level::Fast, -30, cpu8());
        for (const Core &c : s.cores()) s.setStart(c.ccd, c.slot, -10);
        std::optional<Trial> t;
        while ((t = s.next()) && !t->what.startsWith("20-minute")) s.report({!t->perCore.contains(-11), {}});
        CHECK(t.has_value());
        const QList<int> before = s.values();
        int i5 = -1;
        for (int i = 0; i < s.cores().size(); ++i) if (s.cores()[i].cpus.contains(5)) i5 = i;
        s.report(Outcome{false, {5}, true});
        for (int i = 0; i < before.size(); ++i) CHECK(s.values()[i] == before[i] + (i == i5 ? 2 : 0));
        auto r = Search::load(s.save());  // survives a resume
        CHECK(r.has_value());
        if (r) {
            CHECK(r->next().has_value());
            const QList<int> mid = r->values();
            r->report(Outcome{false, {5}, true});
            for (int i = 0; i < mid.size(); ++i) CHECK(r->values()[i] == std::min(0, mid[i] + 2));
        }
    }

    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("coevidence: all checks passed");
    return 0;
}
