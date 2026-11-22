// System tools reports over a fake sysfs / procfs tree and synthetic EDIDs: no real hardware is read.
#include <cmath>
#include "../src/diagnostictools.h"
#include "../src/systemdiagnostics.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QTemporaryDir>

#define CHECK(x) do { if (!(x)) { qCritical() << "FAILED" << #x << "line" << __LINE__; return 1; } } while (false)

using diag::Level;

static QString root;
static void put(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(root + path).path());
    QFile f(root + path);
    f.open(QIODevice::WriteOnly);
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
    CHECK(diagnostictools::decodeEdid(e.left(128)).truncated);
    put(QStringLiteral("/sys/class/drm/card1-eDP-1/status"), "connected\n");
    put(QStringLiteral("/sys/class/drm/card1-eDP-1/edid"), e);
    const diag::Report disp = diagnostictools::displays(root);
    CHECK(has(disp, Level::Info, QStringLiteral("48–240")));
    CHECK(disp.tables.size() >= 2 && disp.tables.at(1).rows.at(0).value(4).contains(QStringLiteral("800 nits")));

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

    // ── S04: a GPU trained narrower than both ends allow.
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

    // ── S10: zram and zswap together.
    put(QStringLiteral("/proc/meminfo"), "MemTotal:       32000000 kB\nMemAvailable:   20000000 kB\nSwapTotal: 8000000 kB\nSwapFree: 8000000 kB\n");
    put(QStringLiteral("/sys/block/zram0/disksize"), "8589934592\n");
    put(QStringLiteral("/sys/block/zram0/mm_stat"), "1000000 250000 300000 0 0 0 0 0\n");
    put(QStringLiteral("/sys/block/zram0/comp_algorithm"), "lzo [zstd]\n");
    put(QStringLiteral("/sys/module/zswap/parameters/enabled"), "Y\n");
    put(QStringLiteral("/proc/pressure/memory"), "some avg10=0.00 avg60=0.50 avg300=0.10 total=1\nfull avg10=0.00 avg60=0.10 avg300=0.00 total=1\n");
    CHECK(has(systemdiagnostics::memory(root), Level::Review, QStringLiteral("zswap and zram")));

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
