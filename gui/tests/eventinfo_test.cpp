// Unit test for the kernel-event explanations (eventinfo.cpp) and the distribution defaults (distroprofile.cpp).
#include "../src/distroprofile.h"
#include "../src/eventinfo.h"

#include <QCoreApplication>
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static bool anyContains(const QStringList &l, const char *s) {
    for (const QString &x : l) if (x.contains(QLatin1String(s))) return true;
    return false;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    using namespace eventinfo;

    // Xid: the program and GPU address come from the record itself.
    const QJsonObject x = classifyLine(QStringLiteral("NVRM: Xid (PCI:0000:01:00): 13, pid=4242, name=game.exe, Graphics Exception"));
    CHECK(x.value(QStringLiteral("kind")).toString() == QLatin1String("xid") && x.value(QStringLiteral("code")).toInt() == 13);
    Info i = explain(x);
    CHECK(anyContains(i.facts, "game.exe") && anyContains(i.facts, "4242") && anyContains(i.facts, "0000:01:00"));
    CHECK(!i.steps.isEmpty() && !i.causes.isEmpty() && !i.meaning.isEmpty());
    // Xid 79 is a bus problem, Xid 154 a consequence.
    CHECK(explain(QJsonObject{{"kind", "xid"}, {"code", 79}}).meaning.contains(QLatin1String("PCIe")));
    CHECK(explain(QJsonObject{{"kind", "xid"}, {"code", 154}}).meaning.contains(QLatin1String("consequence")));
    // Xid 71 (CE4) is a copy-engine error, not an unknown code.
    CHECK(explain(QJsonObject{{"kind", "xid"}, {"code", 71}}).meaning.contains(QLatin1String("copy engines")));

    // OOM: the victim and its size.
    i = explain(classifyLine(QStringLiteral("Out of memory: Killed process 1234 (firefox) total-vm:9000000kB, anon-rss:4194304kB, file-rss:0kB")));
    CHECK(anyContains(i.facts, "firefox") && anyContains(i.facts, "4096 MiB"));

    // Kernel warning: module, source location, function.
    const QJsonObject w = classifyLine(QStringLiteral("WARNING: CPU: 3 PID: 77 at drivers/gpu/drm/amd/display/dc/core/dc.c:1234 dc_commit+0x1a/0x30 [amdgpu]"));
    CHECK(w.value(QStringLiteral("title")).toString() == QLatin1String("Kernel warning in amdgpu"));
    i = explain(w);
    CHECK(anyContains(i.facts, "amdgpu") && anyContains(i.facts, "dc.c:1234") && anyContains(i.facts, "dc_commit"));

    // Storage and PCIe: the device.
    CHECK(anyContains(explain(classifyLine(QStringLiteral("nvme nvme1: I/O 12 QID 3 timeout, aborting"))).facts, "nvme1"));
    // The device that erred is the one after "from", not the root port that reported it.
    CHECK(anyContains(explain(classifyLine(QStringLiteral("pcieport 0000:00:1c.0: AER: Corrected error message received from 0000:02:00.0"))).facts, "0000:02:00.0"));
    CHECK(anyContains(explain(classifyLine(QStringLiteral("pcieport 0000:00:1c.0: AER: Corrected error received: 0000:00:1c.0"))).facts, "0000:00:1c.0"));
    // Not errors: harmless lines that only mention the words, and AER detail lines.
    CHECK(classifyLine(QStringLiteral("BTRFS info (device sda2): scrub: finished on devid 1 with status: 0 errors")).isEmpty());
    CHECK(classifyLine(QStringLiteral("nvme 0000:02:00.0: AER:   device [144d:a80a] error status/mask=00000001/0000e000")).isEmpty());
    CHECK(classifyLine(QStringLiteral("iwlwifi 0000:00:14.3: loaded firmware version 89.0")).isEmpty());
    CHECK(classifyLine(QStringLiteral("systemd[1]: Started Out of memory killer daemon")).isEmpty());
    CHECK(anyContains(explain(classifyLine(QStringLiteral("BTRFS error (device nvme0n1p3): bdev /dev/nvme0n1p3 errs: wr 0, rd 0, flush 0, corrupt 1"))).steps, "scrub"));
    CHECK(anyContains(explain(classifyLine(QStringLiteral("EXT4-fs error (device sda2): ext4_find_entry:1455: inode #2"))).facts, "sda2"));
    CHECK(anyContains(explain(classifyLine(QStringLiteral("watchdog: BUG: soft lockup - CPU#5 stuck for 22s! [kworker/5:1:123]"))).facts, "kworker/5"));

    // Ordinary lines are not events; unknown kinds still get a generic answer.
    CHECK(classifyLine(QStringLiteral("usb 1-2: new high-speed USB device number 3")).isEmpty());
    CHECK(!explain(QJsonObject{{"kind", "something"}}).steps.isEmpty());
    CHECK(html(x, QStringLiteral("t")).contains(QLatin1String("<ol")));

    // Distribution families and their defaults.
    using namespace distroprofile;
    CHECK(familyOf(QStringLiteral("ID=cachyos\nID_LIKE=arch\n")) == QLatin1String("arch"));
    CHECK(familyOf(QStringLiteral("ID=linuxmint\nID_LIKE=\"ubuntu debian\"\n")) == QLatin1String("debian"));
    CHECK(familyOf(QStringLiteral("ID=\"almalinux\"\nID_LIKE=\"rhel centos fedora\"\n")) == QLatin1String("fedora"));
    CHECK(familyOf(QStringLiteral("ID=\"opensuse-leap\"\n")) == QLatin1String("opensuse"));
    CHECK(familyOf(QStringLiteral("ID=gentoo\n")) == QLatin1String("gentoo"));
    CHECK(familyOf(QStringLiteral("ID=nixos\n")).isEmpty());
    for (const QString &f : families()) {
        const QStringList ex = imageExcludes(f);
        CHECK(ex.contains(QStringLiteral("/proc/*")) && ex.contains(QStringLiteral("/sys/*")) && ex.contains(QStringLiteral("/dev/*")));
        CHECK(!excludeSummary(f).isEmpty() && !familyLabel(f).isEmpty() && !restoreHint(f).isEmpty());
    }
    CHECK(imageExcludes(QStringLiteral("arch")).contains(QStringLiteral("/var/lib/pacman/sync/*")));
    CHECK(imageExcludes(QStringLiteral("debian")).contains(QStringLiteral("/var/lib/apt/lists/*")));
    CHECK(!imageExcludes(QStringLiteral("debian")).contains(QStringLiteral("/usr/portage/distfiles/*")));
    CHECK(imageExcludes(QStringLiteral("gentoo")).contains(QStringLiteral("/usr/portage/distfiles/*")));
    CHECK(restoreHint(QStringLiteral("gentoo")).startsWith(QLatin1String("/mnt/gentoo")));
    // The Defaults menu replaces default lines only; the user's own stay.
    CHECK(isDefaultLine(QStringLiteral("/var/lib/pacman/sync/*")) && isDefaultLine(QStringLiteral("/usr/portage/distfiles/*")));
    CHECK(!isDefaultLine(QStringLiteral("/data/*")) && !isDefaultLine(QStringLiteral("/home/*/.cache/*")));
    CHECK(lines(QStringLiteral(" /a \r\n\n  \n/b")) == QStringList({QStringLiteral("/a"), QStringLiteral("/b")}));
    CHECK(lines(legacyDefaults()).size() == 11);
    CHECK(unescapeOctal(QStringLiteral("/swap\\040file\\011x")) == QStringLiteral("/swap file\tx"));
    CHECK(unescapeOctal(QStringLiteral("/a\\9")) == QStringLiteral("/a\\9"));
    for (const Optional &o : optionalExcludes()) {
        CHECK(o.pattern.startsWith(QLatin1Char('/')) && !o.label.isEmpty() && !o.about.isEmpty());
        CHECK(!o.pattern.endsWith(QLatin1String("/steamapps/*")));   // compatdata (Proton saves) must stay
    }

    if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
    else std::puts("eventinfo: all checks passed");
    return failures ? 1 : 0;
}
