#include "toolcatalog.h"

#include <QCoreApplication>
#include <QHash>
#include <QRegularExpression>

namespace toolcatalog {

namespace {

QString tx(const char *s) { return QCoreApplication::translate("toolcatalog", s); }

QStringList words(const char *list) { return QString::fromUtf8(list).split(QLatin1Char('|'), Qt::SkipEmptyParts); }

const QHash<QString, Info> &tools() {
    using A = Area;
    static const QHash<QString, Info> t = {
        // CPU & memory
        {"CPU", {A::Cpu, 0, words("cpu|işlemci|processor|frekans|frequency|governor|cpufreq|pstate|boost|cpupower|lscpu|çekirdek|core")}},
        {"Memory", {A::Cpu, 0, words("ram|bellek|swap|zram|numa|free")}},
        {"Memory Architecture Explorer", {A::Cpu, 0, words("ram|bellek|swap|zram|zswap|huge pages|hugepage|numa|l3|memory pressure|psi|S10")}},
        {"DMI", {A::Cpu, NeedsRoot, words("dmidecode|bios|anakart|motherboard|board|ram modülü|memory module|seri numarası|serial")}},
        // Graphics & display
        {"NVIDIA", {A::Graphics, MayWakeGpu, words("nvidia-smi|gpu|ekran kartı|graphics card|xid|gsp|sürücü|driver")}},
        {"NVIDIA module parameters", {A::Graphics, 0, words("nvidia|modprobe|modül|module|parametre|parameter|gsp|nvreg|preserve video memory|uyku|S06")}},
        {"Graphics", {A::Graphics, MayWakeGpu, words("vulkan|opengl|glx|mesa|oyun|game|dxvk|gpu")}},
        {"EDID / VRR", {A::Graphics, ChangesSettings,
                        words("monitör|monitor|ekran|display|vrr|freesync|g-sync|gsync|hdr|refresh|yenileme hızı|hz|titreme|flicker|"
                              "parlaklık|brightness|edid|panel|S03")}},
        // Storage
        {"Storage", {A::Storage, 0, words("disk|ssd|nvme|lsblk|df|dosya sistemi|filesystem|f2fs|btrfs|ext4|scheduler|mount|bölüm|partition")}},
        {"SMART details", {A::Storage, NeedsRoot, words("smart|smartctl|disk sağlığı|drive health|nvme|wear|aşınma|ssd|bad sector")}},
        {"Storage tuning", {A::Storage, ChangesSettings, words("io scheduler|readahead|disk|ssd|nvme|trim|ayar")}},
        // Network & wireless
        {"Wi-Fi / Bluetooth Firmware Doctor",
         {A::Network, 0, words("wifi|wi-fi|wlan|kablosuz|wireless|bluetooth|bt|firmware missing|firmware eksik|iwlwifi|mt7925|"
                               "rtw89|rfkill|bağlantı kopuyor|disconnect|internet|S07")}},
        {"Network tuning", {A::Network, ChangesSettings, words("ağ|network|tcp|bbr|gecikme|latency|ayar")}},
        // Power & thermal
        {"Sensors", {A::Power, 0, words("sıcaklık|temperature|fan|voltaj|voltage|hwmon|lm-sensors|sensör|watt")}},
        {"Thermal", {A::Power, 0, words("sıcaklık|temperature|termal|thermal zone|trip|ısınma|overheating")}},
        {"Battery", {A::Power, 0, words("pil|batarya|battery|şarj|charge|cycle|döngü|kapasite|capacity|wear")}},
        {"Power Limit Source Map",
         {A::Power, 0, words("güç|power|ppt|tdp|rapl|watt|limit|platform profile|throttling|kısılma|yavaşlama|S08")}},
        {"USB-C PD / Charger Inspector",
         {A::Power, 0, words("şarj aleti|adaptör|charger|usb-c|type-c|pd|power delivery|watt|şarj olmuyor|S13")}},
        // Devices & buses
        {"PCI", {A::Devices, 0, words("pci|lspci|aygıt|device|sürücü|driver|gpu|nvme")}},
        {"PCIe detail", {A::Devices, NeedsRoot, words("pcie|lspci -vv|aspm|aer|link|bağlantı")}},
        {"PCIe Link Negotiation Lab",
         {A::Devices, 0, words("pcie|link|hız|speed|gen|lane|şerit|aspm|aer|xid 79|fallen off the bus|bant genişliği|bandwidth|S04")}},
        {"USB", {A::Devices, 0, words("usb|lsusb|port|hub|cihaz|device|bağlantı kopması|disconnect")}},
        // Firmware & kernel
        {"Kernel", {A::Firmware, 0, words("kernel|çekirdek|uname|lsmod|modül|module|cmdline|boot parameters|açılış parametreleri")}},
        {"Kernel Feature & Firmware Check",
         {A::Firmware, 0, words("kernel config|firmware missing|firmware eksik|modül|module|driver yok|missing driver|desteklenmiyor|unsupported|S18")}},
        {"ACPI / WMI Firmware Browser", {A::Firmware, 0, words("acpi|wmi|firmware|bios|lenovo|legion|dsdt|S11")}},
        {"Firmware updates", {A::Firmware, UsesInternet, words("fwupd|fwupdmgr|lvfs|bios update|bios güncelleme|firmware güncelleme|güncelleme|update")}},
        {"Security", {A::Firmware, 0, words("güvenlik|security|secure boot|mitigation|spectre|lockdown|taint|microcode|mikrokod")}},
        {"Kernel tuning", {A::Firmware, ChangesSettings, words("sysctl|scheduler|zamanlayıcı|eevdf|kernel|çekirdek|ayar")}},
        // System & configuration
        {"Hardware snapshot",
         {A::System, 0, words("baseline|referans|karşılaştır|compare|değişiklik|change|güncelleme sonrası|after update|snapshot|S01")}},
        {"Configuration files",
         {A::System, ChangesSettings, words("config|yapılandırma|ayar dosyası|configuration|modprobe.d|sysctl.d|grub|düzenle|edit")}},
        {"PipeWire Audio Doctor",
         {A::System, ChangesSettings, words("ses|audio|sound|pipewire|pulseaudio|wireplumber|mikrofon|microphone|hoparlör|speaker|cızırtı|crackling|S15")}},
    };
    return t;
}

}  // namespace

QList<Area> areas() {
    return {Area::Cpu, Area::Graphics, Area::Storage, Area::Network, Area::Power, Area::Devices, Area::Firmware, Area::System};
}

QString areaName(Area a) {
    switch (a) {
    case Area::Cpu: return tx(QT_TRANSLATE_NOOP("toolcatalog", "CPU & Memory"));
    case Area::Graphics: return tx(QT_TRANSLATE_NOOP("toolcatalog", "Graphics & Display"));
    case Area::Storage: return tx(QT_TRANSLATE_NOOP("toolcatalog", "Storage"));
    case Area::Network: return tx(QT_TRANSLATE_NOOP("toolcatalog", "Network & Wireless"));
    case Area::Power: return tx(QT_TRANSLATE_NOOP("toolcatalog", "Power & Thermal"));
    case Area::Devices: return tx(QT_TRANSLATE_NOOP("toolcatalog", "Devices & Buses"));
    case Area::Firmware: return tx(QT_TRANSLATE_NOOP("toolcatalog", "Firmware & Kernel"));
    case Area::System: return tx(QT_TRANSLATE_NOOP("toolcatalog", "System & Configuration"));
    }
    return {};
}

Info tool(const QString &key) { return tools().value(key); }

QStringList pageKeywords(const QString &key) {
    static const QHash<QString, QStringList> p = {
        {"overview", words("genel bakış|özet|summary|overview|durum|status")},
        {"monitor", words("xid|gsp|mce|aer|hata|error|olay|event|çökme|crash|uyarı|alert|nvidia|lockup|oom|donma|freeze")},
        {"evidence", words("xid|mce|aer|f2fs|kanıt|evidence|firmware|radyo|radio|wifi")},
        {"investigations", words("triage|sorun|problem|h18|başla|start|regression|nvme|usb|uyku|sleep|çökme|crash|incident|not|ecc|gpu memory")},
        {"boot", words("boot|açılış|önyükleme|çökme|crash|kapanma|shutdown|donma|freeze|güncelleme sonrası|after update")},
        {"capabilities", words("eksik özellik|missing feature|capability|destek|support|desteklenmiyor|unsupported|neden yok")},
        {"curves", words("curve optimizer|co|undervolt|kararlılık|stability|mce|eğri")},
        {"klog", words("dmesg|kernel log|çekirdek günlüğü|journal|warning|uyarı|hata")},
        {"kernelparams", words("sysctl|scheduler|zamanlayıcı|eevdf|debugfs|kernel parametreleri|kernel parameters")},
        {"sleep", words("uyku|suspend|resume|uyanma|wake|s0ix|s3|hibernate|kapak|lid")},
        {"battery", words("pil|batarya|battery|drain|tüketim|şarj|charge|ömür")},
        {"dgpu", words("dgpu|nvidia|d3cold|runtime pm|uyumuyor|uyanık|awake|hibrit|hybrid")},
        {"cooling", words("soğutma|cooling|fan|sıcaklık|temperature|termal macun|thermal paste|ısınma")},
        {"smart", words("disk sağlığı|drive health|smart|ssd|nvme|aşınma|wear")},
        {"network", words("firewall|güvenlik duvarı|port|bağlantı|connection|trafik|traffic|ağ|network")},
        {"backup", words("yedek|backup|snapshot|geri yükleme|restore|imaj|image")},
        {"tools", words("araçlar|tools|sistem araçları")},
    };
    return p.value(key);
}

QStringList badges(int flags) {
    QStringList b;
    if (flags & ChangesSettings) b << tx(QT_TRANSLATE_NOOP("toolcatalog", "Can change settings"));
    if (flags & MayWakeGpu) b << tx(QT_TRANSLATE_NOOP("toolcatalog", "May wake the dGPU"));
    if (flags & NeedsRoot) b << tx(QT_TRANSLATE_NOOP("toolcatalog", "Needs root"));
    if (flags & UsesInternet) b << tx(QT_TRANSLATE_NOOP("toolcatalog", "Uses the internet"));
    return b;
}

QString normalize(const QString &s) {
    QString out;
    out.reserve(s.size());
    for (const QChar c : s.normalized(QString::NormalizationForm_D)) {
        if (c.category() == QChar::Mark_NonSpacing) continue;
        if (c == QChar(0x0131) || c == QChar(0x0130)) out += QLatin1Char('i');  // ı, İ
        else out += c.toLower();
    }
    return out;
}

bool matches(const QString &query, const QString &label, const QStringList &keywords) {
    const QStringList qs = normalize(query).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (qs.isEmpty()) return true;
    const QString hay = normalize(label + QLatin1Char('|') + keywords.join(QLatin1Char('|')));
    for (const QString &w : qs)
        if (!hay.contains(w)) return false;
    return true;
}

QStringList relatedFor(const QString &triageKey) {
    static const QHash<QString, QStringList> r = {
        {"gpu", {"page:monitor", "page:evidence", "tool:NVIDIA module parameters", "tool:PCIe Link Negotiation Lab", "page:dgpu", "page:klog"}},
        {"cpu", {"page:curves", "page:monitor", "tool:CPU", "tool:Power Limit Source Map", "page:boot"}},
        {"crash", {"page:boot", "page:klog", "tool:Hardware snapshot", "tool:Kernel Feature & Firmware Check"}},
        {"storage", {"page:smart", "tool:Storage", "tool:PCIe Link Negotiation Lab", "page:klog"}},
        {"radio", {"tool:Wi-Fi / Bluetooth Firmware Doctor", "tool:Kernel Feature & Firmware Check", "page:network"}},
        {"sleep", {"page:sleep", "page:dgpu", "page:battery"}},
        {"usb", {"tool:USB", "tool:USB-C PD / Charger Inspector", "tool:PCI"}},
    };
    return r.value(triageKey);
}

QString explainTerms(const QString &text) {
    struct Term { const char *name, *about; };
    static const Term TERMS[] = {
        {"AER", QT_TRANSLATE_NOOP("toolcatalog", "PCIe Advanced Error Reporting: the link's own error counters. A few corrected errors are common; a growing count or uncorrected errors are not.")},
        {"ASPM", QT_TRANSLATE_NOOP("toolcatalog", "PCIe Active State Power Management: lets a link drop to a low-power state when idle. Saves power; on some devices it causes errors or stalls.")},
        {"GSP", QT_TRANSLATE_NOOP("toolcatalog", "GPU System Processor: firmware running on newer NVIDIA GPUs. GSP timeouts often show up as freezes or black screens.")},
        {"D3cold", QT_TRANSLATE_NOOP("toolcatalog", "The deepest PCIe power state: the device is switched off. A sleeping dGPU is in D3cold.")},
        {"Xid", QT_TRANSLATE_NOOP("toolcatalog", "An error code the NVIDIA driver writes to the kernel log; the number says what failed.")},
        {"MCE", QT_TRANSLATE_NOOP("toolcatalog", "Machine Check Exception: the CPU reports a hardware error, e.g. from too low a voltage.")},
        {"EDID", QT_TRANSLATE_NOOP("toolcatalog", "The data a display sends about itself: name, modes, refresh range, HDR support.")},
        {"VRR", QT_TRANSLATE_NOOP("toolcatalog", "Variable refresh rate (FreeSync / G-SYNC): the display follows the game's frame rate.")},
        {"PPT", QT_TRANSLATE_NOOP("toolcatalog", "Package Power Tracking: the firmware's power limits for the CPU package.")},
        {"RAPL", QT_TRANSLATE_NOOP("toolcatalog", "Running Average Power Limit: the CPU's own power meter and limits.")},
        {"NUMA", QT_TRANSLATE_NOOP("toolcatalog", "Non-Uniform Memory Access: memory is closer to some cores than to others.")},
        {"zram", QT_TRANSLATE_NOOP("toolcatalog", "Compressed swap in RAM: more usable memory for some CPU time.")},
        {"SMART", QT_TRANSLATE_NOOP("toolcatalog", "A drive's own health record: wear, errors, temperature.")},
        {"EEVDF", QT_TRANSLATE_NOOP("toolcatalog", "The Linux CPU scheduler since 6.6: decides which task runs next.")},
    };
    QStringList out;
    for (const Term &t : TERMS) {
        const QRegularExpression re(QStringLiteral("\\b%1\\b").arg(QLatin1String(t.name)), QRegularExpression::CaseInsensitiveOption);
        if (re.match(text).hasMatch()) out << QStringLiteral("%1 — %2").arg(QLatin1String(t.name), tx(t.about));
    }
    return out.join(QLatin1Char('\n'));
}

}  // namespace toolcatalog
