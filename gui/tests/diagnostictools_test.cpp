// Synthetic fixtures for S01 / S03 / S06: no sysfs, NVIDIA query, root, or GPU is required.
#include "../src/diagnostictools.h"
#include <QCoreApplication>
#include <QJsonObject>
#include <QByteArray>
#include <QDebug>
#define CHECK(x) do { if (!(x)) { qCritical() << "FAILED" << #x << "at line" << __LINE__; return 1; } } while (false)

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QByteArray edid(128, '\0');
    const QByteArray header = QByteArray::fromHex("00ffffffffffff00");
    for (int i=0; i<header.size(); ++i) edid[i] = header[i];
    // 1920 x 1080 @ 60 Hz; 148.5 MHz pixel clock, total 2200 x 1125.
    const unsigned char dtd[] = {0x02,0x3a,0x80,0x18,0x71,0x38,0x2d,0x40};
    for (int i=0; i<8; ++i) edid[54+i] = static_cast<char>(dtd[i]);
    int sum = 0;
    for (int i=0; i<127; ++i) sum += static_cast<unsigned char>(edid[i]);
    edid[127] = static_cast<char>((256 - sum%256)%256);
    QString report = diagnostictools::inspectEdidBytes(edid);
    CHECK(report.contains("valid"));
    CHECK(report.contains("1920×1080"));
    CHECK(report.contains("60.00 Hz"));
    CHECK(report.contains("not VRR"));
    edid[127] = static_cast<char>(edid.at(127) ^ 1);
    CHECK(diagnostictools::inspectEdidBytes(edid).contains("INVALID"));
    edid[126] = 1; // extension declared but missing
    CHECK(diagnostictools::inspectEdidBytes(edid).contains("missing/truncated"));
    CHECK(diagnostictools::inspectEdidBytes(QByteArray(32,'x')).contains("shorter"));

    auto opts = diagnostictools::parseNvidiaOptionsText(QStringLiteral(
        "# options nvidia NVreg_Bad=3\n"
        "options nvidia NVreg_CoherentGPUMemoryMode=1 NVreg_Test=\"foo\" # disabled\n"
        "options nvidia-drm modeset=1\n"
        "options amdgpu foobar=1\n"));
    CHECK(opts.size() == 3);
    CHECK(opts.value(QStringLiteral("nvidia/NVreg_CoherentGPUMemoryMode")) == QStringLiteral("1"));
    CHECK(opts.value(QStringLiteral("nvidia/NVreg_Test")) == QStringLiteral("foo"));
    CHECK(opts.value(QStringLiteral("nvidia_drm/modeset")) == QStringLiteral("1"));

    QJsonObject old{{QStringLiteral("pci/0000:01:00.0/driver"),QStringLiteral("nvidia")},
                    {QStringLiteral("host/kernel"),QStringLiteral("7.1")}};
    QJsonObject newer{{QStringLiteral("pci/0000:01:00.0/driver"),QStringLiteral("nouveau")},
                      {QStringLiteral("host/kernel"),QStringLiteral("7.1")},
                      {QStringLiteral("host/bios_version"),QStringLiteral("1.2")}};
    const QString difference = diagnostictools::snapshotDifferenceText(old, newer);
    CHECK(difference.contains("Changed fields: 2"));
    CHECK(difference.contains("before: nvidia"));
    CHECK(difference.contains("after : nouveau"));
    CHECK(difference.contains("+ host/bios_version"));
    return 0;
}
