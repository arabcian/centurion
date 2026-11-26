// System tools reports over a fake sysfs / procfs tree and synthetic EDIDs: no real hardware is read.
#include <cmath>
#include <algorithm>
#include <unistd.h>
#include "../src/diagnostictools.h"
#include "../src/systemdiagnostics.h"
#include "../src/hierarchyviewmodel.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTemporaryDir>

#define CHECK(x) do { if (!(x)) { qCritical() << "FAILED" << #x << "line" << __LINE__; return 1; } } while (false)

using diag::Level;

static QString root;
static void put(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(root + path).path());
    QFile f(root + path);
    if (!f.open(QIODevice::WriteOnly)) qFatal("cannot write fixture %s", qPrintable(f.fileName()));
    f.write(data);
}
static bool has(const diag::Report &r, Level l, const QString &needle) {
    for (const diag::Finding &f : r.findings) if (f.level == l && (f.title + f.detail).contains(needle)) return true;
    return false;
}
static void checksum(QByteArray &b, int block) {
    int sum = 0;
    for (int i = block * 128; i < block * 128 + 127; ++i) sum += quint8(b.at(i));
    b[block * 128 + 127] = char((256 - sum % 256) % 256);
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    root = tmp.path();

    // Sparse CPU numbering, canonicalized SMT siblings and conflicting offline state.
    const QString cpuBase = QStringLiteral("/sys/devices/system/cpu/");
    put(cpuBase + "online", "0,8");
    for (const QString &id : {QStringLiteral("cpu0"), QStringLiteral("cpu8"), QStringLiteral("cpu10")}) {
        const QString t = cpuBase + id + "/topology/";
        put(t + "physical_package_id", "0"); put(t + "die_id", "0"); put(t + "core_id", "1");
        put(t + "thread_siblings_list", id == "cpu0" ? "0,8" : id == "cpu8" ? "8,0" : "0,10");
    }
    put(cpuBase + "cpu10/online", "1"); // contradicts global online list
    put("/sys/bus/event_source/devices/cpu_core/cpus", "0,8");
    put("/sys/bus/event_source/devices/cpu_atom/cpus", "10");
    const auto topology = systemdiagnostics::cpuTopology(root);
    int parent0 = -2, parent8 = -3, parent10 = -4;
    for (int i = 0; i < topology.rows.size(); ++i) {
        const auto &row = topology.rows.at(i);
        if (row.value(0) == "cpu0") { parent0 = topology.parents.at(i); CHECK(row.value(7) == "Performance PMU (cpu_core)"); }
        if (row.value(0) == "cpu8") { parent8 = topology.parents.at(i); CHECK(row.value(7) == "Performance PMU (cpu_core)"); }
        if (row.value(0) == "cpu10") { parent10 = topology.parents.at(i); CHECK(row.value(2) == "unknown"); CHECK(topology.levels.at(i) == Level::Unknown); }
    }
    CHECK(parent0 >= 0 && parent0 == parent8 && parent10 >= 0 && parent10 != parent0);
    QDir(root + cpuBase).removeRecursively();
    const QString fwBase = QStringLiteral("/sys/class/firmware-attributes/test/attributes/ppt_pl1_spl/");
    put(fwBase + "type", "integer"); put(fwBase + "current_value", "140");
    put(fwBase + "default_value", "120"); put(fwBase + "min_value", "30");
    put(fwBase + "max_value", "150"); put(fwBase + "scalar_increment", "1");
    bool seenTarget = false;
    for (const auto &table : systemdiagnostics::powerLimits(root).tables) {
        // One grouped table: no flat copy and no separate firmware table repeat the same rows.
        CHECK(table.title == "Limits and Their Sources" && !table.parents.isEmpty());
        for (const auto &row : table.rows) {
            if (!row.value(1).contains("ppt_pl1_spl")) continue;
            CHECK(!seenTarget);
            CHECK(row.value(2) == "140" && row.value(3) == "30 … 150 (step 1)" && row.value(4) == "120");
            CHECK(row.value(5) == "Reported target (not enforcement)");
            seenTarget = true;
        }
    }
    CHECK(seenTarget);
    CHECK(systemdiagnostics::powerLimits(root).tables.size() == 1);
    QDir(root + QStringLiteral("/sys/class/firmware-attributes")).removeRecursively();
    QDir(root + QStringLiteral("/sys/bus/event_source")).removeRecursively();



    // ── EDID: identity, preferred timing, range limits with the +255 offset, FreeSync block in a CTA extension.
    QByteArray e(256, '\0');
    const QByteArray header = QByteArray::fromHex("00ffffffffffff00");
    for (int i = 0; i < 8; ++i) e[i] = header[i];
    e[8] = char(0x4c); e[9] = char(0x83);   // "SDC": 5-bit letters 19, 4, 3
    e[17] = char(33);                        // 2023
    const unsigned char dtd[] = {0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40};
    for (int i = 0; i < 8; ++i) e[54 + i] = char(dtd[i]);
    const unsigned char range[] = {0, 0, 0, 0xfd, 0x02, 48, 225, 0, 0};  // flags 10b: max + 255 = 480 Hz
    for (int i = 0; i < 9; ++i) e[72 + i] = char(range[i]);
    const char name[] = {0, 0, 0, char(0xfc), 0, 'P', 'a', 'n', 'e', 'l', '\n'};
    for (int i = 0; i < 11; ++i) e[90 + i] = name[i];
    e[16] = char(12);                        // week 12
    e[20] = char(0xa5); e[21] = char(34); e[22] = char(21); e[23] = char(0x78);  // digital 8 bit, 34 × 21 cm, gamma 2.2
    e[126] = 1;
    checksum(e, 0);
    e[128] = 0x02; e[129] = 0x03; e[130] = 24; e[131] = char(0x70);  // CTA, DTDs from byte 24; audio, YCbCr 4:4:4 and 4:2:2
    const unsigned char vsdb[] = {0x67, 0x1a, 0x00, 0x00, 0x01, 0x00, 48, 240};  // tag 3, len 7, AMD OUI, v1, caps, 48–240
    for (int i = 0; i < 8; ++i) e[132 + i] = char(vsdb[i]);
    const unsigned char hdr[] = {0xe6, 0x06, 0x0d, 0x01, 0x80, 0x70, 0x40};  // HDR static metadata: SDR + PQ + HLG, peak code 0x80 = 800 nits
    for (int i = 0; i < 7; ++i) e[140 + i] = char(hdr[i]);
    const unsigned char col[] = {0xe3, 0x05, 0xc0, 0x00};                    // colorimetry: BT.2020 YCC and RGB
    for (int i = 0; i < 4; ++i) e[147 + i] = char(col[i]);
    checksum(e, 1);
    const diagnostictools::Edid d = diagnostictools::decodeEdid(e);
    CHECK(d.header && !d.truncated && d.badBlocks.isEmpty());
    CHECK(d.maker == QStringLiteral("SDC") && d.year == 2023 && d.name == QStringLiteral("Panel"));
    CHECK(d.timings.value(0).contains(QStringLiteral("1920×1080 @ 60.00")));
    CHECK(d.rangeMin == 48 && d.rangeMax == 480);
    CHECK(d.vrrMin == 48 && d.vrrMax == 240 && d.vrrSource.contains(QStringLiteral("FreeSync")));
    CHECK(d.widthCm == 34 && d.heightCm == 21 && d.bitDepth == 8 && d.week == 12 && std::abs(d.gamma - 2.2) < 0.001);
    CHECK(d.audio && d.ycc444 && d.ycc422 && !d.hdmi);
    CHECK(d.hdr == QStringList({QStringLiteral("HDR10 (PQ)"), QStringLiteral("HLG")}) && std::abs(d.maxNits - 800) < 0.01 && d.minNits > 0.4 && d.minNits < 0.6);
    CHECK(d.colorimetry == QStringList({QStringLiteral("BT.2020 YCC"), QStringLiteral("BT.2020 RGB")}));
    QByteArray broken = e;
    broken[200] = char(broken.at(200) ^ 1);
    CHECK(diagnostictools::decodeEdid(broken).badBlocks.size() == 1);
    CHECK(diagnostictools::decodeEdid(broken).vrrMax == 0);
    CHECK(diagnostictools::decodeEdid(broken).timings.isEmpty());
    CHECK(diagnostictools::decodeEdid(broken).hdr.isEmpty());
    QByteArray interlaced=e; interlaced[71]=char(quint8(interlaced.at(71))|0x80); checksum(interlaced,0);
    CHECK(diagnostictools::decodeEdid(interlaced).timings.isEmpty());
    QByteArray undeclared=e; undeclared[126]=0; checksum(undeclared,0);
    CHECK(!diagnostictools::decodeEdid(undeclared).structuralValid && diagnostictools::decodeEdid(undeclared).vrrMax==0);
    CHECK(diagnostictools::decodeEdid(e.left(128)).vrrMax==0);
    QByteArray invalidRange=e; invalidRange[138]=char(250); checksum(invalidRange,1);
    CHECK(diagnostictools::decodeEdid(invalidRange).vrrMax==0);
    CHECK(diagnostictools::decodeEdid(e.left(128)).truncated);
    put(QStringLiteral("/sys/class/drm/card1-eDP-1/status"), "connected\n");
    put(QStringLiteral("/sys/class/drm/card1-eDP-1/edid"), e);
    put(QStringLiteral("/sys/class/drm/card1-eDP-1/modes"), "1920x1080\n1920x1080\n1280x720\n");
    const diag::Report disp = diagnostictools::displays(root);
    CHECK(has(disp, Level::Info, QStringLiteral("48–240")));
    CHECK(disp.tables.size() >= 2 && disp.tables.at(1).rows.at(0).value(4).contains(QStringLiteral("800 nits")));

    CHECK(disp.tables.size() >= 4 && disp.tables.at(2).rows.size() == 2);
    if (disp.tables.size() >= 4 && disp.tables.at(2).rows.size() == 2) {
        CHECK(disp.tables.at(2).rows.at(0).value(4) == QStringLiteral("0"));
        CHECK(disp.tables.at(2).rows.at(1).value(4) == QStringLiteral("0"));
        CHECK(disp.tables.at(2).rows.at(1).value(2) == QStringLiteral("CTA-861"));
        CHECK(disp.tables.at(3).rows.at(0).value(1).contains(QStringLiteral("1920×1080")));
        CHECK(disp.tables.at(3).rows.at(0).value(2) == QStringLiteral("EDID base block"));
        CHECK(disp.tables.at(3).rows.at(0).value(3) == QStringLiteral("Listed"));
    }
    bool foundBinary=false, foundTimings=false, foundKernelModes=false;
    for (const auto &table : disp.tables) {
        if (table.title == QStringLiteral("EDID Binary Inspector")) { CHECK(table.rows.size()==16); CHECK(table.rows.first().value(2)==QStringLiteral("0x0000")); foundBinary=true; }
        if (table.title == QStringLiteral("Progressive Timing Details")) { CHECK(!table.rows.isEmpty()); foundTimings=true; }
        if (table.title == QStringLiteral("Modes Listed by Linux DRM")) {
            CHECK(table.rows.size() == 2 && table.rows.first().value(1) == QStringLiteral("1920x1080"));
            CHECK(table.rows.first().value(2) == QStringLiteral("sysfs DRM modes"));
            foundKernelModes = true;
        }
    }
    CHECK(foundBinary && foundTimings && foundKernelModes);
    QByteArray shortCta=e; shortCta[130]=7; checksum(shortCta,1);
    CHECK(diagnostictools::decodeEdid(shortCta).vrrMin<=0);
    // A Type IX nominal mode is listed even when a same-resolution
    // interlaced declaration and the kernel's progressive name differ.
    QByteArray displayId(256, '\0');
    const auto didConnector = QStringLiteral("/sys/class/drm/card2-DP-1/");
    put(didConnector + "status", "connected\n");
    put(didConnector + "modes", "1920x1080\n");
    const auto setDisplayId = [&](const QByteArray &section) {
        displayId.fill('\0', 256);
        displayId.replace(0, 8, header);
        displayId[126] = 1;
        checksum(displayId, 0);
        displayId.replace(128, section.size(), section);
        unsigned sum = 0;
        for (qsizetype i = 129; i < 128 + section.size(); ++i) sum += quint8(displayId.at(i));
        displayId[128 + section.size()] = char(-sum);
        checksum(displayId, 1);
        put(didConnector + "edid", displayId);
    };
    const auto didRows = [&] {
        QList<QStringList> rows;
        for (const auto &table : diagnostictools::displays(root).tables)
            if (table.title == QStringLiteral("Advertised Timings"))
                for (const auto &row : table.rows)
                    if (row.value(0) == QStringLiteral("card2-DP-1")) rows.append(row);
        return rows;
    };
    setDisplayId(QByteArray::fromHex("7020090300240006027f0737048f"));
    auto rows = didRows();
    CHECK(rows.size() == 1 && rows.value(0).value(1) == QStringLiteral("1920×1080 @ 144 Hz (nominal)"));
    CHECK(rows.value(0).value(2) == QStringLiteral("DisplayID Type IX") && rows.value(0).value(3) == QStringLiteral("Listed"));
    setDisplayId(QByteArray::fromHex("70130e030004000b007d0010ef44a437042c34"));
    rows = didRows();
    CHECK(rows.size() == 1 && rows.value(0).value(1).contains(QStringLiteral("1920×1080 interlaced")));
    CHECK(rows.value(0).value(2) == QStringLiteral("DisplayID Type II") && rows.value(0).value(3) == QStringLiteral("Not listed"));
    put(didConnector + "modes", "1920x1080i\n");
    CHECK(didRows().value(0).value(3) == QStringLiteral("Listed"));
    displayId[147] = char(quint8(displayId.at(147)) ^ 1); // Break the section checksum, then fix the outer one.
    checksum(displayId, 1);
    put(didConnector + "edid", displayId);
    CHECK(didRows().isEmpty());
    const auto redacted=diag::redactPrivateText(QStringLiteral("192.168.1.42 aa:bb:cc:dd:ee:ff 0000:01:00.0 Xid 154\nSerial: secret-123\n/home/private/file"));
    CHECK(!redacted.contains(QStringLiteral("192.168.1.42")) && !redacted.contains(QStringLiteral("aa:bb:cc:dd:ee:ff")) && !redacted.contains(QStringLiteral("secret-123")) && !redacted.contains(QStringLiteral("/home/private")));
    CHECK(redacted.contains(QStringLiteral("0000:01:00.0")) && redacted.contains(QStringLiteral("Xid 154")));
    // IPv6 is masked, times, ACPI IDs and C++/Rust paths are not.
    const auto v6=diag::redactPrivateText(QStringLiteral("inet6 fe80::1ff:fe23:4567:890a/64 2001:db8:85a3:0:0:8a2e:370:7334 at 2026-11-24 18:22:05 acpi PNP0C14:00: core::panicking std::string"));
    CHECK(!v6.contains(QStringLiteral("fe80::1ff")) && !v6.contains(QStringLiteral("2001:db8")));
    CHECK(v6.contains(QStringLiteral("18:22:05")) && v6.contains(QStringLiteral("PNP0C14:00:")) && v6.contains(QStringLiteral("core::panicking")) && v6.contains(QStringLiteral("std::string")));
    const auto ids=diag::redactPrivateText(QStringLiteral("Boot ID: 12345678-1234-1234-1234-123456789012\nWMI GUID: 87654321-4321-4321-4321-210987654321"));
    CHECK(!ids.contains(QStringLiteral("12345678-1234-1234-1234-123456789012")));
    CHECK(ids.contains(QStringLiteral("87654321-4321-4321-4321-210987654321")));
    const auto sourcePrivacy=diag::redactPrivateText(QStringLiteral("$ uname -a\nLinux secret-host 6.11.0 #1 SMP\nPARTUUID=ab12cd34-12ab-34cd-56ef-123456789abc\nMachine-ID: ab12cd3412ab34cd56ef123456789abc\nGPU UUID: GPU-11111111-2222-3333-4444-555555555555\nSSID=private-network\nID_SERIAL_SHORT=private-serial\n/run/media/private-user/disk\nWMI GUID: 87654321-4321-4321-4321-210987654321\n0000:01:00.0 Xid 154 MCE\n"));
    for(const auto &secret:{QStringLiteral("secret-host"),QStringLiteral("ab12cd34-12ab-34cd-56ef-123456789abc"),QStringLiteral("ab12cd3412ab34cd56ef123456789abc"),QStringLiteral("GPU-11111111-2222-3333-4444-555555555555"),QStringLiteral("private-network"),QStringLiteral("private-serial"),QStringLiteral("private-user")}) CHECK(!sourcePrivacy.contains(secret));
    CHECK(sourcePrivacy.contains(QStringLiteral("WMI GUID: 87654321-4321-4321-4321-210987654321")) && sourcePrivacy.contains(QStringLiteral("0000:01:00.0 Xid 154 MCE")));
    diag::Report original; original.add(Level::Attention,QStringLiteral("excluded evidence"));
    auto selected=original; selected.findings.clear();
    diag::Table privateTable; privateTable.headers={QStringLiteral("Serial Number"),QStringLiteral("PCI")}; privateTable.add({QStringLiteral("secret-456"),QStringLiteral("0000:01:00.0")}); selected.tables.append(privateTable);
    diag::redactPrivateFields(selected);
    CHECK(!selected.toTextWithVerdict(original).contains(QStringLiteral("secret-456")));
    CHECK(selected.toTextWithVerdict(original).contains(original.headline()));
    CHECK(!selected.toTextWithVerdict(original).contains(QStringLiteral("excluded evidence")));
    diag::Report sourceScoped;
    diag::Table sourceFields;
    sourceFields.headers = {QStringLiteral("Localized identity"), QStringLiteral("Kernel message")};
    sourceFields.privateColumns = {0};
    sourceFields.add({QStringLiteral("a1b2c3d4"), QStringLiteral("Xid 154")});
    sourceScoped.tables << sourceFields;
    sourceScoped.privatePaths << QStringLiteral("/mnt/private-volume");
    sourceScoped.raw = QStringLiteral("path=/mnt/private-volume");
    sourceScoped.add(Level::Info, QStringLiteral("Mounted at /mnt/private-volume"));
    const auto archive = QJsonDocument(diag::archiveSnapshot(sourceScoped)).toJson(QJsonDocument::Compact);
    CHECK(!archive.contains("private-volume") && !archive.contains("a1b2c3d4"));
    diag::redactPrivateFields(sourceScoped);
    CHECK(!sourceScoped.raw.contains(QStringLiteral("private-volume")));
    CHECK(sourceScoped.tables.first().rows.first() == QStringList({QStringLiteral("[redacted]"), QStringLiteral("Xid 154")}));
    put(QStringLiteral("/sys/class/drm/card1-eDP-1/edid"), broken);
    const diag::Report badDisp = diagnostictools::displays(root);
    CHECK(badDisp.tables.size() >= 3 && badDisp.tables.at(2).levels.value(1) == Level::Review);
    put(QStringLiteral("/sys/class/drm/card1-eDP-1/edid"), e);

    // ── NVIDIA options: hex and decimal, Y and 1 are the same value.
    CHECK(diagnostictools::sameValue(QStringLiteral("0x02"), QStringLiteral("2")));
    CHECK(diagnostictools::sameValue(QStringLiteral("Y"), QStringLiteral("1")));
    CHECK(!diagnostictools::sameValue(QStringLiteral("1"), QStringLiteral("0")));
    const auto opts = diagnostictools::parseNvidiaOptionsText(QStringLiteral(
        "# options nvidia NVreg_Bad=3\noptions nvidia NVreg_DynamicPowerManagement=0x02 NVreg_Test=\"foo\" # c\noptions nvidia-drm modeset=1\noptions amdgpu x=1\n"));
    CHECK(opts.size() == 3 && opts.value(QStringLiteral("nvidia_drm/modeset")) == QStringLiteral("1") && opts.value(QStringLiteral("nvidia/NVreg_Test")) == QStringLiteral("foo"));
    put(QStringLiteral("/etc/modprobe.d/nvidia.conf"), "options nvidia NVreg_DynamicPowerManagement=0x02\noptions nvidia_drm modeset=1\n");
    put(QStringLiteral("/sys/module/nvidia/version"), "580.95\n");
    put(QStringLiteral("/sys/module/nvidia_drm/parameters/modeset"), "Y\n");
    put(QStringLiteral("/proc/driver/nvidia/params"), "DynamicPowerManagement: 2\nPreserveVideoMemoryAllocations: 0\n");
    CHECK(diagnostictools::nvidiaParams(root).verdict() == Level::Ok);  // 0x02 = 2, modeset 1 = Y
    put(QStringLiteral("/proc/driver/nvidia/params"), "DynamicPowerManagement: 3\n");
    CHECK(has(diagnostictools::nvidiaParams(root), Level::Review, QStringLiteral("runs with 3")));

    // ── Hardware snapshot: a device that lost its driver.
    const QJsonObject before{{"host/kernel", "6.12"}, {"pci/0000:01:00.0/vendor", "0x10de"}, {"pci/0000:01:00.0/device", "0x2c58"},
                             {"pci/0000:01:00.0/driver", "nvidia"}, {"pci/0000:01:00.0/current_link_speed", "2.5 GT/s PCIe"}, {"module/nvidia", "580"}};
    QJsonObject after = before;
    after.remove(QStringLiteral("pci/0000:01:00.0/driver"));
    after.insert(QStringLiteral("pci/0000:01:00.0/current_link_speed"), QStringLiteral("16.0 GT/s PCIe"));  // dynamic: ignored
    after.insert(QStringLiteral("host/kernel"), QStringLiteral("6.13"));
    after.remove(QStringLiteral("module/nvidia"));
    const diag::Report snap = diagnostictools::hardwareSnapshot(before, QStringLiteral("then"), after);
    CHECK(has(snap, Level::Review, QStringLiteral("lost its driver")));
    CHECK(has(snap, Level::Info, QStringLiteral("6.12 → 6.13")));
    CHECK(diagnostictools::hardwareSnapshot(before, QStringLiteral("then"), before).verdict() == Level::Ok);
    // Boot parameters stay readable; identifying values are hidden.
    const QString bp = diagnostictools::bootParameters(QStringLiteral(
        "BOOT_IMAGE=/vmlinuz root=UUID=0b0e6a1c-1111-2222-3333-444455556666 rw quiet amd_pstate=active nvidia_drm.modeset=1 "
        "rd.luks.uuid=abcd resume=/dev/nvme0n1p2 mitigations=off zswap.compressor=zstd foo=00:11:22:33:44:55"));
    CHECK(bp.contains("amd_pstate=active") && bp.contains("zswap.compressor=zstd") && bp.contains("mitigations=off") && bp.contains("rw"));
    CHECK(bp.contains("root=…") && bp.contains("rd.luks.uuid=…") && bp.contains("resume=…") && bp.contains("foo=…") && !bp.contains("0b0e6a1c"));
    // An old baseline that hid a value is not a change once the value becomes visible; module versions are shown.
    QJsonObject oldBase = before, newNow = before;
    oldBase.insert("host/cmdline", "amd_pstate=… quiet"); newNow.insert("host/cmdline", "amd_pstate=active quiet");
    oldBase.insert("module/ahci", "loaded"); newNow.insert("module/ahci", "in-tree");
    newNow.insert("module/zenpower", "0.5.0"); newNow.insert("module/btusb", "in-tree"); newNow.insert("host/kernel", "6.13");
    const diag::Report hs = diagnostictools::hardwareSnapshot(oldBase, QStringLiteral("then"), newNow);
    CHECK(!has(hs, Level::Info, QStringLiteral("Boot Parameters changed")));
    bool sawZen = false, sawAhci = false, sawBt = false;
    for (const auto &table : hs.tables) for (const auto &row : table.rows) {
        if (row.value(0) == "zenpower") sawZen = row.value(3) == "0.5.0";
        if (row.value(0) == "ahci") sawAhci = true;
        if (row.value(0) == "btusb") sawBt = row.value(3) == "in-tree (kernel 6.13)";
    }
    CHECK(sawZen && !sawAhci && sawBt);

    // ── S04: a GPU trained narrower than both ends allow.
    const QString gpu = QStringLiteral("/sys/devices/pci0000:00/0000:01:00.0");
    put(gpu + QStringLiteral("/power/runtime_status"), "active\n");
    put(gpu + QStringLiteral("/vendor"), "0x10de\n");
    put(gpu + QStringLiteral("/device"), "0x2c58\n");
    CHECK(QDir().mkpath(root + QStringLiteral("/sys/bus/pci/devices")));
    CHECK(QFile::link(root + gpu, root + QStringLiteral("/sys/bus/pci/devices/0000:01:00.0")));
    put(QStringLiteral("/sys/bus/pci/devices/0000:01:00.0/current_link_speed"), "16.0 GT/s PCIe\n");
    put(QStringLiteral("/sys/bus/pci/devices/0000:01:00.0/max_link_speed"), "32.0 GT/s PCIe\n");
    put(QStringLiteral("/sys/bus/pci/devices/0000:01:00.0/current_link_width"), "8\n");
    put(QStringLiteral("/sys/bus/pci/devices/0000:01:00.0/max_link_width"), "16\n");
    put(QStringLiteral("/sys/bus/pci/devices/0000:01:00.0/class"), "0x030000\n");
    put(QStringLiteral("/sys/bus/pci/devices/0000:01:00.0/aer_dev_correctable"), "RxErr 0\nTOTAL_ERR_COR 3\n");
    CHECK(systemdiagnostics::linkSpeed(QStringLiteral("32.0 GT/s PCIe")) == 32.0);
    const diag::Report links = systemdiagnostics::pcieLinks(root);
    CHECK(has(links, Level::Review, QStringLiteral("x8 of a possible x16")));
    CHECK(links.tables.first().rows.first().value(5).startsWith(QStringLiteral("3")));

    CHECK(links.tables.size() >= 2 && !links.tables.at(1).rows.isEmpty());
    if (links.tables.size() >= 2 && !links.tables.at(1).rows.isEmpty()) {
        CHECK(links.tables.at(1).rows.first().value(1) == QStringLiteral("—"));
        CHECK(links.tables.at(1).rows.first().value(2) == QStringLiteral("32 GT/s ×16"));
    }

    // Same-boot comparisons require both endpoint identity fields; changed identity is not a link delta.
    put(QStringLiteral("/proc/sys/kernel/random/boot_id"), "fixture-boot\n");
    put(QStringLiteral("/sys/bus/pci/devices/0000:01:00.0/vendor"), "0x10de\n");
    put(QStringLiteral("/sys/bus/pci/devices/0000:01:00.0/device"), "0x2c58\n");
    QJsonObject sample;
    systemdiagnostics::pcieLinks(root, {}, &sample);
    CHECK(sample.value(QStringLiteral("boot")).toString() == QStringLiteral("fixture-boot"));
    put(QStringLiteral("/sys/bus/pci/devices/0000:01:00.0/current_link_width"), "16\n");
    const auto changedLink = systemdiagnostics::pcieLinks(root, sample);
    CHECK(changedLink.tables.last().rows.first().value(3).contains(QStringLiteral("Link reading changed")));
    put(QStringLiteral("/sys/bus/pci/devices/0000:01:00.0/device"), "0x2c59\n");
    const auto changedEndpoint = systemdiagnostics::pcieLinks(root, sample);
    CHECK(changedEndpoint.tables.last().rows.first().value(3).contains(QStringLiteral("Endpoint identity changed")));
    put(gpu + QStringLiteral("/device"), "0x2c58\n");
    CHECK(QDir().rename(root + gpu, root + gpu + QStringLiteral("-detached")));
    put(gpu + QStringLiteral("/vendor"), "0x10de\n"); put(gpu + QStringLiteral("/device"), "0x2c58\n");
    put(gpu + QStringLiteral("/class"), "0x030000\n"); put(gpu + QStringLiteral("/power/runtime_status"), "active\n");
    put(gpu + QStringLiteral("/current_link_speed"), "16.0 GT/s PCIe\n"); put(gpu + QStringLiteral("/max_link_speed"), "32.0 GT/s PCIe\n");
    put(gpu + QStringLiteral("/current_link_width"), "16\n"); put(gpu + QStringLiteral("/max_link_width"), "16\n");
    const auto replacement = systemdiagnostics::pcieLinks(root, sample);
    CHECK(replacement.tables.last().rows.first().value(3).contains(QStringLiteral("Endpoint identity changed")));
    put(gpu + QStringLiteral("/power/runtime_status"), "suspended\n");
    QJsonObject suspended;
    const auto sleeping = systemdiagnostics::pcieLinks(root, sample, &suspended);
    CHECK(suspended.value(QStringLiteral("links")).toObject().isEmpty());
    CHECK(has(sleeping, Level::Unknown, QStringLiteral("link not probed")));
    put(gpu + QStringLiteral("/power/runtime_status"), "active\n");
    put(QStringLiteral("/proc/sys/kernel/random/boot_id"), "new-boot\n");
    CHECK(systemdiagnostics::pcieLinks(root, sample).tables.size() == 3);

    // ── S10: zram and zswap together.
    put(QStringLiteral("/proc/meminfo"), "MemTotal:       32000000 kB\nMemAvailable:   20000000 kB\nSwapTotal: 8000000 kB\nSwapFree: 8000000 kB\n");
    put(QStringLiteral("/sys/block/zram0/disksize"), "8589934592\n");
    put(QStringLiteral("/sys/block/zram0/mm_stat"), "1000000 250000 300000 0 0 0 0 0\n");
    put(QStringLiteral("/sys/block/zram0/comp_algorithm"), "lzo [zstd]\n");
    put(QStringLiteral("/sys/module/zswap/parameters/enabled"), "Y\n");
    put(QStringLiteral("/proc/pressure/memory"), "some avg10=0.00 avg60=0.50 avg300=0.10 total=1\nfull avg10=0.00 avg60=0.10 avg300=0.00 total=1\n");
    put(QStringLiteral("/proc/swaps"), "Filename Type Size Used Priority\n/dev/zram0 partition 8000000 0 100\n");
    CHECK(has(systemdiagnostics::memory(root), Level::Review, QStringLiteral("zswap and zram")));
    put(QStringLiteral("/proc/swaps"), "Filename Type Size Used Priority\n/swapfile file 8000000 0 -2\n");
    CHECK(!has(systemdiagnostics::memory(root), Level::Review, QStringLiteral("zswap and zram")));
    put(QStringLiteral("/proc/swaps"), "Filename Type Size Used Priority\n/dev/zram0 partition broken 0 100\n");
    CHECK(!has(systemdiagnostics::memory(root), Level::Review, QStringLiteral("zswap and zram")));
    put(QStringLiteral("/proc/meminfo"), "MemTotal: 32000000 kB\nMemAvailable: 20000000 MB\nSwapTotal: 8000000 kB\nSwapFree: 8000000 kB\n");
    CHECK(has(systemdiagnostics::memory(root), Level::Unknown, QStringLiteral("RAM availability is incomplete")));
    put(QStringLiteral("/proc/meminfo"), "MemTotal: 32000000 kB\nMemAvailable: 20000000 kB\nSwapTotal: 8000000 kB\nSwapFree: 8000000 kB\n");
    put(QStringLiteral("/proc/swaps"), "Filename Type Size Used Priority\n/dev/zram0 partition 8000000 0 100\n");

    put(QStringLiteral("/sys/devices/system/cpu/cpu0/cache/index3/level"), "3");
    put(QStringLiteral("/sys/devices/system/cpu/cpu0/cache/index3/type"), "Unified");
    put(QStringLiteral("/sys/devices/system/cpu/cpu0/cache/index3/shared_cpu_list"), "0-3");
    QByteArray dimm(34,0);dimm[0]=17;dimm[1]=34;dimm[13]=0x20;
    put(QStringLiteral("/sys/firmware/dmi/entries/17-0/raw"),dimm);
    auto memory=systemdiagnostics::memory(root); bool cacheFound=false,dimmFound=false;
    for(const auto &t:memory.tables){if(t.title==QStringLiteral("CPU Cache Sharing Domains")){CHECK(t.rows.first().value(3)==QStringLiteral("0-3"));cacheFound=true;}if(t.title==QStringLiteral("SMBIOS Memory Devices")){CHECK(t.rows.first().value(2)==QStringLiteral("8192"));dimmFound=true;}}
    CHECK(cacheFound&&dimmFound);
    QByteArray acpi(36,0);acpi.replace(0,4,"DSDT");acpi[4]=36;acpi[8]=2;
    put(QStringLiteral("/sys/firmware/acpi/tables/DSDT"),acpi);
    bool acpiFound=false;const auto firmware=systemdiagnostics::firmware(root);
    for(const auto &t:firmware.tables)if(t.title==QStringLiteral("ACPI Tables Present")){CHECK(t.rows.first().value(1)==QStringLiteral("DSDT"));acpiFound=true;}
    CHECK(acpiFound);

    // S11 uses supplied saved log records and fake sysfs only; detached GUIDs
    // remain candidates, and a legacy boot prefix cannot join current evidence.
    const QString fwBoot = QStringLiteral("12345678-1234-1234-1234-123456789abc");
    const QString guid = QStringLiteral("887B54E3-DDDC-4B2C-8B88-68A26A8835D0");
    put(QStringLiteral("/proc/sys/kernel/random/boot_id"), fwBoot.toLatin1());
    const QString fwDev = QStringLiteral("/sys/devices/platform/PNP0C14:00/") + guid;
    put(fwDev + QStringLiteral("/modalias"), "wmi:887B54E3-DDDC-4B2C-8B88-68A26A8835D0");
    CHECK(QDir().mkpath(root + QStringLiteral("/sys/bus/wmi/devices")));
    CHECK(QDir().mkpath(root + QStringLiteral("/sys/bus/wmi/drivers/lenovo-wmi")));
    CHECK(QFile::link(root + fwDev, root + QStringLiteral("/sys/bus/wmi/devices/") + guid));
    CHECK(QFile::link(root + QStringLiteral("/sys/bus/wmi/drivers/lenovo-wmi"), root + fwDev + QStringLiteral("/driver")));
    CHECK(::symlink(QFile::encodeName(root + fwDev + QStringLiteral("-missing")).constData(),
                    QFile::encodeName(root + QStringLiteral("/sys/bus/wmi/devices/") + guid + QStringLiteral("-1")).constData()) == 0);
    CHECK(QDir().mkpath(root + QStringLiteral("/sys/bus/acpi/devices")));
    // Real layout: the ACPI node lives under LNXSYSTM:00; the platform device links to it with firmware_node.
    CHECK(QDir().mkpath(root + QStringLiteral("/sys/devices/LNXSYSTM:00/PNP0C14:00")));
    CHECK(QFile::link(root + QStringLiteral("/sys/devices/LNXSYSTM:00/PNP0C14:00"), root + QStringLiteral("/sys/bus/acpi/devices/PNP0C14:00")));
    CHECK(QFile::link(root + QStringLiteral("/sys/devices/LNXSYSTM:00/PNP0C14:00"), root + QStringLiteral("/sys/devices/platform/PNP0C14:00/firmware_node")));
    const QList<diagsrc::LogLine> fwLogs{
        {"t",fwBoot.left(8),"warn",{},{},"wmi: " + guid,1},
        {"t",fwBoot,"warn",{},{},"wmi: " + guid,2},
        {"t",fwBoot,"warn",{},{},"lenovo-wmi: event",3},
        {"t",fwBoot,"warn",{},{},"ACPI: PNP0C14:00: event",4},
        {"t",fwBoot,"warn",{},{},"unrelated: event",5}};
    const auto fwReport = systemdiagnostics::firmware(root, fwLogs);
    bool wmiFound=false, evidenceFound=false;
    for (const auto &table : fwReport.tables) {
        if (table.title == QStringLiteral("WMI Objects")) {
            wmiFound=true; CHECK(table.rows.size()==2);
            for (const auto &row:table.rows) CHECK(row.value(5)==QStringLiteral("2"));
        }
        if (table.title == QStringLiteral("ACPI / WMI Kernel Evidence")) {
            evidenceFound=true; CHECK(table.rows.size()==3);
            for (const auto &row:table.rows) {
                CHECK(row.value(1)==fwBoot);
                if (row.value(2)==QStringLiteral("2")) CHECK(row.value(5)==QStringLiteral("2"));
                if (row.value(2)==QStringLiteral("3")) CHECK(row.value(3)==QStringLiteral("Driver prefix candidate"));
            }
        }
    }
    CHECK(wmiFound && evidenceFound);
    bool treeFound=false;
    for (const auto &table : fwReport.tables) if (table.viewKind == QStringLiteral("acpi-wmi-tree")) {
        treeFound=true; int acpiRow=-1, wmiRow=-1;
        for (int i=0;i<table.rows.size();++i) {
            if (table.rows[i].value(0)==QStringLiteral("PNP0C14:00")) acpiRow=i;
            if (table.rows[i].value(0)==guid) wmiRow=i;
        }
        CHECK(acpiRow>=0 && wmiRow>=0 && table.parents.value(wmiRow)==acpiRow);
    }
    CHECK(treeFound);
    // Restore the fixture boot used by subsequent topology tests.
    put(QStringLiteral("/proc/sys/kernel/random/boot_id"), "new-boot\n");

    // ── S13: the charger's PD profiles.
    put(QStringLiteral("/sys/class/typec/port0/power_role"), "[sink] source\n");
    put(QStringLiteral("/sys/class/typec/port0-partner/usb_power_delivery_revision"), "3.0\n");
    put(QStringLiteral("/sys/class/usb_power_delivery/pd1/source-capabilities/1:fixed_supply/voltage"), "5000mV\n");
    put(QStringLiteral("/sys/class/usb_power_delivery/pd1/source-capabilities/1:fixed_supply/maximum_current"), "3000mA\n");
    put(QStringLiteral("/sys/class/usb_power_delivery/pd1/source-capabilities/2:fixed_supply/voltage"), "20000mV\n");
    put(QStringLiteral("/sys/class/usb_power_delivery/pd1/source-capabilities/2:fixed_supply/maximum_current"), "2250mA\n");
    QFile::link(root + QStringLiteral("/sys/class/usb_power_delivery/pd1"), root + QStringLiteral("/sys/class/typec/port0-partner/usb_power_delivery"));
    put(QStringLiteral("/sys/class/power_supply/BAT0/type"), "Battery\n");
    put(QStringLiteral("/sys/class/power_supply/BAT0/energy_full"), "70000000\n");
    put(QStringLiteral("/sys/class/power_supply/BAT0/energy_full_design"), "99900000\n");
    put(QStringLiteral("/sys/class/power_supply/BAT0/status"), "Charging\n");
    const diag::Report ch = systemdiagnostics::charging(root);
    CHECK(has(ch, Level::Review, QStringLiteral("45 W")));
    CHECK(has(ch, Level::Review, QStringLiteral("70 %")));
    bool profilesFound=false;for(const auto &t:ch.tables)if(t.title==QStringLiteral("USB Power Delivery Source Profiles")){CHECK(t.rows.size()==2);CHECK(t.rows.at(1).value(5)==QStringLiteral("45"));profilesFound=true;}
    CHECK(profilesFound);
    put(QStringLiteral("/sys/class/usb_power_delivery/pd1/source-capabilities/3:fixed_supply/voltage"), "48000mV\n");
    const auto partial=systemdiagnostics::charging(root);
    CHECK(!has(partial,Level::Review,QStringLiteral("45 W")));
    bool partialPort=false;
    for(const auto &table:partial.tables)if(table.viewKind==QStringLiteral("pd-ports")){
        CHECK(table.rows.first().value(4)==QStringLiteral("Maximum unknown (incomplete profiles)"));
        CHECK(table.levels.first()==Level::Unknown);partialPort=true;
    }
    CHECK(partialPort);
    CHECK(QDir(root+QStringLiteral("/sys/class/usb_power_delivery/pd1/source-capabilities/3:fixed_supply")).removeRecursively());
    put(QStringLiteral("/sys/class/typec/port0/power_role"), "sink [source]\n");
    CHECK(!has(systemdiagnostics::charging(root),Level::Review,QStringLiteral("45 W")));
    put(QStringLiteral("/sys/class/typec/port0/power_role"), "[sink] source\n");
    put(QStringLiteral("/sys/class/typec/port0/power_operation_mode"), "usb_power_delivery\n");
    put(QStringLiteral("/sys/class/usb_power_delivery/pd1/source-capabilities/3:battery/minimum_voltage"), "9000mV\n");
    put(QStringLiteral("/sys/class/usb_power_delivery/pd1/source-capabilities/3:battery/maximum_voltage"), "20000mV\n");
    put(QStringLiteral("/sys/class/usb_power_delivery/pd1/source-capabilities/3:battery/maximum_power"), "60000mW\n");
    const auto batteryPdo=systemdiagnostics::charging(root);
    bool batteryPdoFound=false,modeFound=false;
    for(const auto &t:batteryPdo.tables){if(t.viewKind==QStringLiteral("pd-profiles"))for(const auto &row:t.rows){if(row.value(1)==QStringLiteral("3:battery")){CHECK(row.value(5)==QStringLiteral("60"));CHECK(row.value(4)==QStringLiteral("unknown"));batteryPdoFound=true;}}
        if(t.viewKind==QStringLiteral("pd-ports")){CHECK(t.rows.first().value(5)==QStringLiteral("usb_power_delivery"));modeFound=true;}}
    CHECK(batteryPdoFound&&modeFound);
    QDir typec(root+QStringLiteral("/sys/class/typec"));
    CHECK(typec.rename(QStringLiteral("port0-partner"),QStringLiteral("detached-partner")));
    const auto disconnected=systemdiagnostics::charging(root);
    for(const auto &table:disconnected.tables) CHECK(table.viewKind!=QStringLiteral("pd-profiles"));
    CHECK(typec.rename(QStringLiteral("detached-partner"),QStringLiteral("port0-partner")));


    // New topology and measurement fixtures: explicit relationships only, no added link probes.
    const QString bridge = QStringLiteral("/sys/devices/pci0000:00/0000:00:1c.0");
    const QString endpoint = bridge + QStringLiteral("/0000:06:00.0");
    put(bridge + QStringLiteral("/vendor"), "0x8086");
    put(endpoint + QStringLiteral("/vendor"), "0x1234");
    put(endpoint + QStringLiteral("/device"), "0x5678");
    CHECK(QFile::link(root + bridge, root + QStringLiteral("/sys/bus/pci/devices/0000:00:1c.0")));
    CHECK(QFile::link(root + endpoint, root + QStringLiteral("/sys/bus/pci/devices/0000:06:00.0")));
    const auto tree = systemdiagnostics::deviceTopology(root);
    CHECK(tree.parents.size() == tree.rows.size());
    int child = -1;
    for (int i = 0; i < tree.rows.size(); ++i) if (tree.rows[i].value(0) == QStringLiteral("0000:06:00.0")) child = i;
    CHECK(child >= 0 && tree.parents.at(child) >= 0 && tree.parents.at(child) < child);
    CHECK(tree.rows.at(tree.parents.at(child)).value(0) == QStringLiteral("0000:00:1c.0"));
    CHECK(tree.rows.at(child).value(5) == QStringLiteral("unknown"));
    CHECK(!tree.rowKeys.at(child).isEmpty());
    HierarchyViewModel hierarchy(tree);
    const auto childIndex = hierarchy.byKey(tree.rowKeys.at(child));
    CHECK(childIndex.isValid()); CHECK(childIndex.parent().data().toString() == QStringLiteral("0000:00:1c.0"));
    auto duplicate = tree; duplicate.rowKeys[tree.parents.at(child)] = tree.rowKeys.at(child);
    HierarchyViewModel ambiguous(duplicate); CHECK(!ambiguous.byKey(tree.rowKeys.at(child)).isValid());
    put(QStringLiteral("/sys/class/power_supply/USB0/type"), "USB_PD");
    put(QStringLiteral("/sys/class/power_supply/USB0/online"), "1");
    put(QStringLiteral("/sys/class/power_supply/USB0/voltage_now"), "20000000");
    put(QStringLiteral("/sys/class/power_supply/USB0/current_max"), "3000000");
    auto chargeRow = [](const diag::Report &r) { for (const auto &table : r.tables) if (table.title == QStringLiteral("Supply Measurements and Reported Limits")) for (const auto &row : table.rows) if (row.value(0) == QStringLiteral("USB0")) return row; return QStringList{}; };
    CHECK(chargeRow(systemdiagnostics::charging(root)).value(5) == QStringLiteral("unknown"));
    put(QStringLiteral("/sys/class/power_supply/USB0/current_now"), "-1000000");
    CHECK(chargeRow(systemdiagnostics::charging(root)).value(5) == QStringLiteral("-20"));
    QTemporaryDir empty;
    CHECK(empty.isValid());
    CHECK(systemdiagnostics::memory(empty.path()).verdict() == Level::Unknown);
    CHECK(systemdiagnostics::powerLimits(empty.path()).verdict() == Level::Unknown);
    // Structured history ignores row ordering, preserves multiplicity and does not retain raw/fix or binary data.
    diag::Report saved; saved.code = QStringLiteral("fixture"); saved.raw = QStringLiteral("private raw marker");
    saved.add(Level::Review, QStringLiteral("Example")).setFix(QStringLiteral("fix"), QJsonObject{{"secret", "private fix marker"}});
    diag::Table history; history.title = QStringLiteral("Values"); history.headers = {QStringLiteral("Serial Number"), QStringLiteral("Value")};
    history.add({QStringLiteral("private serial marker"), QStringLiteral("one")}); history.add({QStringLiteral("private serial marker"), QStringLiteral("two")}); saved.tables << history;
    diag::Table binary; binary.title = QStringLiteral("Localized binary"); binary.privateBinary = true; binary.add({QStringLiteral("private binary marker")}); saved.tables << binary;
    const auto record = diag::archiveSnapshot(saved);
    CHECK(!QJsonDocument(record).toJson().contains("private"));
    std::swap(saved.tables[0].rows[0], saved.tables[0].rows[1]);
    CHECK(diag::archiveDiff(record, diag::archiveSnapshot(saved)).isEmpty());
    saved.tables[0].rows[0][1] = QStringLiteral("three");
    CHECK(diag::archiveDiff(record, diag::archiveSnapshot(saved)).size() == 2);
    CHECK(!diag::archiveDiff({}, record).isEmpty());

    // ── S15: two PulseAudio servers.
    put(QStringLiteral("/proc/asound/cards"), " 0 [Generic        ]: HDA-Intel - HD-Audio Generic\n");
    put(QStringLiteral("/proc/101/comm"), "pulseaudio\n");
    put(QStringLiteral("/proc/102/comm"), "pipewire-pulse\n");
    put(QStringLiteral("/proc/103/comm"), "pipewire\n");
    put(QStringLiteral("/proc/104/comm"), "wireplumber\n");
    CHECK(has(systemdiagnostics::audio({}, root), Level::Attention, QStringLiteral("both running")));

    // ── S18: zstd firmware with a kernel that cannot read it; an option Centurion needs is off.
    put(QStringLiteral("/lib/firmware/mediatek/x.bin.zst"), "z");
    const QString config = QStringLiteral("CONFIG_PCIEAER=y\n# CONFIG_PSTORE is not set\nCONFIG_NFT_QUEUE=m\n");
    CHECK(systemdiagnostics::configValue(config, QStringLiteral("CONFIG_NFT_QUEUE")) == QStringLiteral("m"));
    CHECK(systemdiagnostics::configValue(config, QStringLiteral("CONFIG_PSTORE")) == QStringLiteral("n"));
    CHECK(systemdiagnostics::configValue(config, QStringLiteral("CONFIG_ZRAM")).isEmpty());
    const QList<diagsrc::LogLine> klog{{"t", "b1", "warn", {}, {}, "mt7925e 0000:03:00.0: Direct firmware load for mediatek/missing.bin failed with error -2", 5}};
    const diag::Report k = systemdiagnostics::kernelDeps(config, klog, root);
    CHECK(has(k, Level::Attention, QStringLiteral("zstd")));
    CHECK(has(k, Level::Review, QStringLiteral("CONFIG_PSTORE")));
    CHECK(has(k, Level::Review, QStringLiteral("not installed")));
    CHECK(has(systemdiagnostics::kernelDeps({}, {}, root), Level::Unknown, QStringLiteral("not readable")));
    CHECK(has(systemdiagnostics::kernelDeps({}, {}, root), Level::Review, QStringLiteral("no driver")));  // the fake GPU above
    return 0;
}
