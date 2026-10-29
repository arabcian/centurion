// Unit test for Health → Curve stability → GPU test (gpustress.cpp). Plain asserts, like memtest_test.cpp.
#include "../src/gpustress.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

using namespace gpustress;

static Run fed(const QStringList &lines) {
    Run r;
    for (const QString &l : lines) feed(r, l);
    return r;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    // Plan arguments: --percent only for the VRAM test.
    Plan v = plan(Mode::Vram, "0000:01:00.0", 80, 600);
    CHECK(v.error.isEmpty() && v.args == (QStringList{"vram", "--device", "0000:01:00.0", "--percent", "80", "--seconds", "600"}));
    Plan c = plan(Mode::Core, "0000:01:00.0", 80, 600);
    CHECK(c.error.isEmpty() && c.args == (QStringList{"core", "--device", "0000:01:00.0", "--seconds", "600"}));
    CHECK(plan(Mode::Vram, "", 80, 600).error == QLatin1String("device") && plan(Mode::Core, "", 0, 600).args.isEmpty());
    CHECK(plan(Mode::Vram, "x", 9, 600).error == QLatin1String("percent") && plan(Mode::Vram, "x", 96, 600).error == QLatin1String("percent"));
    CHECK(plan(Mode::Vram, "x", 10, 600).error.isEmpty() && plan(Mode::Vram, "x", 95, 600).error.isEmpty());
    CHECK(plan(Mode::Core, "x", 0, 600).error.isEmpty());  // percent means nothing to the core test
    CHECK(plan(Mode::Core, "x", 0, 0).error == QLatin1String("seconds"));

    // `list`: CPU implementations are hidden, the discrete GPU is preselected (an iGPU may come first).
    const QByteArray list = R"j([
        {"pci":"0000:06:00.0","name":"AMD Radeon Graphics","vendor_id":4098,"device_id":5688,"type":"integrated","vram_mib":512,"driver":"radv"},
        {"pci":"vk:2","name":"llvmpipe (LLVM 20.1.2, 256 bits)","vendor_id":65541,"device_id":0,"type":"cpu","vram_mib":8027,"driver":"llvmpipe"},
        {"pci":"0000:01:00.0","name":"NVIDIA GeForce RTX 4060","vendor_id":4318,"device_id":10246,"type":"discrete","vram_mib":8188,"driver":"NVIDIA"}])j";
    const QList<Device> devs = parseDevices(list);
    CHECK(devs.size() == 2 && devs[0].pci == "0000:06:00.0" && devs[1].pci == "0000:01:00.0" && devs[1].vramMib == 8188);
    CHECK(preselect(devs) == 1);
    CHECK(parseDevices("[]").isEmpty() && parseDevices("not json").isEmpty() && preselect({}) == 0);
    CHECK(preselect(QList<Device>{{"a", "A", "integrated", 0}, {"b", "B", "other", 0}}) == 0);  // no discrete GPU: the first

    // Line parsing: start / progress / error / end; garbage and unknown lines are ignored.
    Run ok = fed({R"j({"ev":"start","mode":"vram","pci":"0000:01:00.0","name":"RTX","driver":"NVIDIA","bytes":6442450944,"chunks":24})j",
                  "not json at all", R"j({"ev":"something-new"})j",
                  R"j({"ev":"progress","t":1.0,"passes":3,"bytes_checked":100,"errors":0})j",
                  R"j({"ev":"end","status":"pass","t":600.0,"passes":40,"errors":0,"msg":"no errors"})j"});
    CHECK(ok.name == "RTX" && ok.bytes == 6442450944 && ok.passes == 40 && ok.errors == 0 && ok.ended && ok.status == "pass" && !ok.deviceLost);
    // Error lines are capped by the tool: the full count in progress / end wins, but never less than the lines seen.
    Run bad = fed({R"j({"ev":"error","t":2.0,"pass":3,"pattern":"walk0","word":7,"expected":"0x1","actual":"0x0"})j",
                   R"j({"ev":"error","t":2.1,"pass":3,"pattern":"walk0","word":9,"expected":"0x1","actual":"0x0"})j"});
    CHECK(bad.errors == 2 && !bad.ended);
    feed(bad, R"j({"ev":"progress","t":3.0,"passes":3,"bytes_checked":1,"errors":5000})j");
    CHECK(bad.errors == 5000);
    feed(bad, R"j({"ev":"error","t":3.1,"pass":4,"pattern":"addr","word":1,"expected":"0x1","actual":"0x0"})j");
    CHECK(bad.errors == 5000);
    feed(bad, R"j({"ev":"progress","t":4.0,"passes":3,"bytes_checked":1,"errors":5000000000})j");  // more than INT_MAX
    CHECK(bad.errors == 5000000000LL);
    Run lost = fed({R"j({"ev":"end","status":"device_lost","t":9.0,"passes":1,"errors":0,"msg":"the device was lost"})j"});
    CHECK(lost.deviceLost && lost.ended && lost.msg == "the device was lost");

    // Kernel events that count against a running test.
    CHECK(isGpuFault("xid", 79) && isGpuFault("xid", 13) && isGpuFault("amdgpu", 0));
    CHECK(!isGpuFault("xid", 45) && !isGpuFault("xid", 154));  // follow-ups of another Xid
    CHECK(!isGpuFault("mce", 0) && !isGpuFault("igpu", 0) && !isGpuFault("gsp", 0) && !isGpuFault("lockup", 0));

    // Verdict: every branch.
    CHECK(verdict(ok, 0, false, false, 0) == Verdict::Pass);
    CHECK(verdict(ok, 1, false, false, 0) == Verdict::Incomplete);        // end said pass but the exit code disagrees
    CHECK(verdict(Run{}, 0, false, false, 0) == Verdict::Incomplete);     // no end line
    CHECK(verdict(ok, 0, true, false, 0) == Verdict::Incomplete);         // crashed (or killed after the deadline)
    CHECK(verdict(ok, 4, false, true, 0) == Verdict::Incomplete);         // cancelled
    CHECK(verdict(fed({R"j({"ev":"end","status":"stopped","t":5.0,"passes":1,"errors":0,"msg":"stopped"})j"}), 4, false, false, 0) == Verdict::Incomplete);
    CHECK(verdict(fed({R"j({"ev":"end","status":"init_failed","t":0.0,"passes":0,"errors":0,"msg":"could not allocate enough VRAM"})j"}), 3, false, false, 0) == Verdict::Incomplete);
    CHECK(verdict(fed({R"j({"ev":"end","status":"fail","t":600.0,"passes":40,"errors":1,"msg":"1 wrong word(s)"})j"}), 1, false, false, 0) == Verdict::Fail);
    CHECK(verdict(bad, 0, true, true, 0) == Verdict::Fail);               // error then cancelled (or crashed): still a failure
    CHECK(verdict(bad, 4, false, true, 0) == Verdict::Fail);
    CHECK(verdict(lost, 2, false, false, 0) == Verdict::Fail);            // a lost device is itself a failed test
    CHECK(verdict(Run{}, -1, true, false, 0) == Verdict::Incomplete);
    CHECK(verdict(ok, 0, false, false, 1) == Verdict::Fail);              // fault event, zero errors
    CHECK(verdict(Run{}, 0, false, true, 2) == Verdict::Fail);            // fault event, then cancelled
    CHECK(verdictName(Verdict::Pass) == "pass" && verdictName(Verdict::Fail) == "fail" && verdictName(Verdict::Incomplete) == "incomplete");
    CHECK(modeName(Mode::Vram) == "vram" && modeName(Mode::Core) == "core");

    // The record keeps the newest KEEP results.
    QTemporaryDir dir;
    qputenv("XDG_STATE_HOME", dir.path().toUtf8());
    CHECK(load().isEmpty() && stateFile().endsWith("/centurion/gpustress.json"));
    for (int i = 0; i < KEEP + 3; ++i) CHECK(append(QJsonObject{{"t", i}}));
    const QJsonArray all = load();
    CHECK(all.size() == KEEP && all.first().toObject().value("t").toInt() == 3 && all.last().toObject().value("t").toInt() == KEEP + 2);

    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
