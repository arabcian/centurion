#include "systools.h"
#include "diagnostictools.h"
#include "diagreportview.h"
#include "systemdiagnostics.h"

#include "lazywidget.h"
#include "privileged.h"
#include "smartpage.h"
#include "configpage.h"
#include "panelcontrols.h"
#include "pipewireconfig.h"
#include "sysadmin.h"
#include "theme.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCompleter>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QTextBrowser>
#include <QTextDocument>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#include <iterator>

namespace {

constexpr int CMD_TIMEOUT_MS = 20000;
constexpr qsizetype MAX_OUT = 1 << 20;

// A command is argv; argv[0] may be a pseudo-command:
//   @file <path>      contents of a file
//   @dir <path>       contents of every file in a directory
//   @glob <pattern>   every file matching (dir/sub/name; sub and name may contain *)
//   @pcilink          PCIe link speed/width of GPUs and NVMe drives (sysfs)
//   @tainted          /proc/sys/kernel/tainted decoded into its flags
//   @microcode        distinct microcode revisions of the CPUs
struct Spec {
    const char *title, *about;
    QList<QStringList> cmds;
    const char *rootTool = nullptr;  // tune-helper {"op":"tool"} instead of cmds
    bool manual = false;             // runs only on "Run again" (it may reach the network)
    bool search = false;             // a find box for the output (pages with long reports)
};

// GUI captions are Title Case, but historic tool keys are persistent identifiers.
// Keep those keys unchanged: they index the catalog, favorites, recent tools,
// related diagnostics and the system-action bar (SMART, PCIe ASPM, fwupd).
QString stableToolKey(const char *caption) {
    const QString display = QString::fromLatin1(caption);
    if (display == QLatin1String("Hardware Snapshot")) return QStringLiteral("Hardware snapshot");
    if (display == QLatin1String("NVIDIA Module Parameters")) return QStringLiteral("NVIDIA module parameters");
    if (display == QLatin1String("PCIe Detail")) return QStringLiteral("PCIe detail");
    if (display == QLatin1String("SMART Details")) return QStringLiteral("SMART details");
    if (display == QLatin1String("Firmware Updates")) return QStringLiteral("Firmware updates");
    return display;
}

// The package that provides each tool, per distribution family (Gentoo first: it is the project's own).
struct ToolPkg { const char *tool, *gentoo, *arch, *fedora, *debian; };
const ToolPkg TOOL_PKGS[] = {
    {"sensors", "sys-apps/lm-sensors", "lm_sensors", "lm_sensors", "lm-sensors"},
    {"lscpu", "sys-apps/util-linux", "util-linux", "util-linux", "util-linux"},
    {"cpupower", "sys-power/cpupower", "cpupower", "kernel-tools", "linux-cpupower"},
    {"numactl", "sys-process/numactl", "numactl", "numactl", "numactl"},
    {"lspci", "sys-apps/pciutils", "pciutils", "pciutils", "pciutils"},
    {"lsusb", "sys-apps/usbutils", "usbutils", "usbutils", "usbutils"},
    {"lsblk", "sys-apps/util-linux", "util-linux", "util-linux", "util-linux"},
    {"nvidia-smi", "x11-drivers/nvidia-drivers", "nvidia-utils", "xorg-x11-drv-nvidia-cuda", "nvidia-smi"},
    {"vulkaninfo", "dev-util/vulkan-tools", "vulkan-tools", "vulkan-tools", "vulkan-tools"},
    {"glxinfo", "x11-apps/mesa-progs", "mesa-utils", "glx-utils", "mesa-utils"},
    {"lsmod", "sys-apps/kmod", "kmod", "kmod", "kmod"},
    {"swapon", "sys-apps/util-linux", "util-linux", "util-linux", "util-linux"},
    {"free", "sys-process/procps", "procps-ng", "procps-ng", "procps"},
    {"df", "sys-apps/coreutils", "coreutils", "coreutils", "coreutils"},
    {"mokutil", "sys-boot/mokutil", "mokutil", "mokutil", "mokutil"},
    {"fwupdmgr", "sys-apps/fwupd", "fwupd", "fwupd", "fwupd"},
    {"memtester", "sys-apps/memtester", "memtester", "memtester", "memtester"},
    {"age", "app-crypt/age", "age", "age", "age"},
    {"pkexec", "sys-auth/polkit", "polkit", "polkit", "pkexec"},
    {"nft", "net-firewall/nftables", "nftables", "nftables", "nftables"},
    {"acpi_call", "sys-power/acpi_call", "acpi_call-dkms", "", "acpi-call-dkms"},  // Fedora: not packaged
};

// "gentoo", "arch", "fedora" (Fedora and the RHEL family), "debian" (Debian and Ubuntu), or "" for any other
// distribution. Read from the ID and ID_LIKE lines of /etc/os-release.
QString distroFamily() {
    static const QHash<QString, QString> FAMILY = {{"gentoo", "gentoo"}, {"arch", "arch"}, {"fedora", "fedora"}, {"rhel", "fedora"},
                                                   {"centos", "fedora"}, {"debian", "debian"}, {"ubuntu", "debian"}};
    static const QString family = [] {
        QStringList ids;
        QFile f(QStringLiteral("/etc/os-release"));
        if (f.open(QIODevice::ReadOnly))
            for (const QString &line : QString::fromUtf8(f.readAll()).split(QLatin1Char('\n')))
                if (line.startsWith(QLatin1String("ID=")) || line.startsWith(QLatin1String("ID_LIKE=")))
                    ids << line.section(QLatin1Char('='), 1).remove(QLatin1Char('"')).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const QString &id : ids)
            if (FAMILY.contains(id)) return FAMILY.value(id);
        return QString();
    }();
    return family;
}

// The command that installs the tool on this distribution, e.g. "emerge sys-apps/lm-sensors". Only the tool's
// name when the distribution or the tool is not known.
QString installHint(const QString &tool) {
    const QString fam = distroFamily();
    for (const ToolPkg &p : TOOL_PKGS) {
        if (tool != QLatin1String(p.tool)) continue;
        auto pick = [&](const char *pkg, const char *cmd) {   // "" = no package in that family's repositories
            return *pkg ? QStringLiteral("%1 %2").arg(QLatin1String(cmd), QLatin1String(pkg)) : tool;
        };
        if (fam == QLatin1String("gentoo")) return pick(p.gentoo, "emerge");
        if (fam == QLatin1String("arch")) return pick(p.arch, "pacman -S");
        if (fam == QLatin1String("fedora")) return pick(p.fedora, "dnf install");
        if (fam == QLatin1String("debian")) return pick(p.debian, "apt install");
        break;
    }
    return tool;
}

QString findExe(const QString &name) {
    QString p = QStandardPaths::findExecutable(name);
    if (p.isEmpty())  // user PATH often lacks sbin; Gentoo puts nvidia-smi in /opt/bin
        p = QStandardPaths::findExecutable(name, {"/usr/local/sbin", "/usr/local/bin", "/usr/sbin", "/usr/bin",
                                                  "/sbin", "/bin", "/opt/bin"});
    return p;
}

QString readFile(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.read(MAX_OUT)) : QStringLiteral("(cannot read %1)\n").arg(path);
}

QString pciLinks() {
    QString out;
    const QDir d(QStringLiteral("/sys/bus/pci/devices"));
    for (const QString &e : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System)) {
        const QString p = d.filePath(e) + QLatin1Char('/');
        const QString cls = readFile(p + "class").trimmed();
        const char *what = cls.startsWith("0x03") ? "GPU" : cls.startsWith("0x0108") ? "NVMe" : nullptr;
        if (!what || !QFile::exists(p + "current_link_speed")) continue;
        auto rd = [&](const char *f) { return readFile(p + QLatin1String(f)).trimmed(); };
        const QString speed = rd("current_link_speed"), width = rd("current_link_width");
        const QString maxSpeed = rd("max_link_speed"), maxWidth = rd("max_link_width");
        out += QStringLiteral("%1  %2  now %3 x%4  (max %5 x%6)\n")
                   .arg(e, QString::fromLatin1(what).leftJustified(4), speed, width, maxSpeed, maxWidth);
        // Fewer lanes than the device supports is a warning. A lower speed is only a note: an idle link
        // downclocks on purpose, so the speed is checked again under load.
        if (width.toInt() > 0 && maxWidth.toInt() > width.toInt())
            out += QCoreApplication::translate("systools", "    ! x%1 of the x%2 the device supports: the slot, a riser or a bifurcation setting may limit the lanes. Check again under load.\n").arg(width, maxWidth);
        else if (!speed.isEmpty() && !maxSpeed.isEmpty() && speed != maxSpeed)
            out += QCoreApplication::translate("systools", "    note: a lower speed at idle is normal; check again under load.\n");
    }
    return out.isEmpty() ? QStringLiteral("(no PCIe link information)\n") : out;
}

// Kernel taint flags, bit 0 first (include/linux/kernel.h, admin-guide/tainted-kernels.rst).
struct TaintBit { char letter; const char *text; };
const TaintBit TAINT_BITS[] = {
    {'P', QT_TRANSLATE_NOOP("systools", "proprietary module loaded")},
    {'F', QT_TRANSLATE_NOOP("systools", "module force-loaded")},
    {'S', QT_TRANSLATE_NOOP("systools", "SMP kernel on a CPU not designed for SMP")},
    {'R', QT_TRANSLATE_NOOP("systools", "module forcibly removed")},
    {'M', QT_TRANSLATE_NOOP("systools", "machine check exception")},
    {'B', QT_TRANSLATE_NOOP("systools", "bad page referenced")},
    {'U', QT_TRANSLATE_NOOP("systools", "tainted by user space request")},
    {'D', QT_TRANSLATE_NOOP("systools", "an oops or BUG happened earlier")},
    {'A', QT_TRANSLATE_NOOP("systools", "ACPI table overridden")},
    {'W', QT_TRANSLATE_NOOP("systools", "a kernel warning happened earlier")},
    {'C', QT_TRANSLATE_NOOP("systools", "staging driver loaded")},
    {'I', QT_TRANSLATE_NOOP("systools", "firmware bug workaround applied")},
    {'O', QT_TRANSLATE_NOOP("systools", "out-of-tree module loaded")},
    {'E', QT_TRANSLATE_NOOP("systools", "unsigned module loaded")},
    {'L', QT_TRANSLATE_NOOP("systools", "soft lockup happened")},
    {'K', QT_TRANSLATE_NOOP("systools", "kernel live-patched")},
    {'X', QT_TRANSLATE_NOOP("systools", "auxiliary taint (distribution)")},
    {'T', QT_TRANSLATE_NOOP("systools", "built with the randstruct plugin")},
    {'N', QT_TRANSLATE_NOOP("systools", "kernel self-test taint")},
};

// /proc/sys/kernel/tainted as the number and one line per flag set.
QString taintedText() {
    const QString raw = readFile(QStringLiteral("/proc/sys/kernel/tainted")).trimmed();
    bool ok = false;
    const qulonglong mask = raw.toULongLong(&ok);
    if (!ok) return raw + QLatin1Char('\n');
    if (mask == 0) return QStringLiteral("0\n") + QCoreApplication::translate("systools", "not tainted: nothing unusual has been loaded or has happened") + QLatin1Char('\n');
    QString out = QStringLiteral("%1\n").arg(mask);
    for (int bit = 0; bit < 64; ++bit) {
        if (!(mask & (qulonglong(1) << bit))) continue;
        if (bit < int(std::size(TAINT_BITS)))
            out += QStringLiteral("  bit %1 %2: %3\n").arg(bit).arg(QChar(TAINT_BITS[bit].letter)).arg(QCoreApplication::translate("systools", TAINT_BITS[bit].text));
        else
            out += QStringLiteral("  bit %1: %2\n").arg(bit).arg(QCoreApplication::translate("systools", "unknown flag"));
    }
    return out;
}

// One line per distinct microcode revision in /proc/cpuinfo (every thread repeats it).
QString microcodeText() {
    QStringList revs;
    for (const QString &line : readFile(QStringLiteral("/proc/cpuinfo")).split(QLatin1Char('\n'))) {
        if (!line.startsWith(QLatin1String("microcode"))) continue;
        const QString rev = line.section(QLatin1Char(':'), 1).trimmed();
        if (!revs.contains(rev)) revs << rev;
    }
    return revs.isEmpty() ? QCoreApplication::translate("systools", "(no microcode information)\n")
                          : QStringLiteral("microcode: %1\n").arg(revs.join(QLatin1String(", ")));
}

// "Serial Number: X", "UUID: X", "POWER_SUPPLY_SERIAL_NUMBER=X", "GPU UUID : X"…
// (dmidecode, smartctl, nvme, nvidia-smi, battery). Placeholder values that
// identify nothing are left visible.
const QRegularExpression SERIAL_RE(
    QStringLiteral(R"(^([^:=\n]*?(?:[Ss]erial|SERIAL|UUID|[Uu]uid|Asset Tag)[^:=\n]*?\s*[:=]\s*)(\S[^\n]*?)\s*$)"),
    QRegularExpression::MultilineOption);
const QSet<QString> PLACEHOLDERS = {"Not Specified", "Not Present", "None", "Unknown", "N/A", "Not Available",
                                    "To Be Filled By O.E.M.", "Default string", "Default String", "0", "00000000", ""};

QString tl(const char *s) { return QCoreApplication::translate("systools", s); }

QJsonObject req(const char *op, std::initializer_list<std::pair<QString, QJsonValue>> args = {}) {
    QJsonObject o{{QStringLiteral("op"), QLatin1String(op)}};
    for (const auto &[k, v] : args) o.insert(k, v);
    return o;
}

QStringList dirNames(const QString &dir, const QStringList &filters = {}) {
    return QDir(dir).entryList(filters, QDir::Dirs | QDir::NoDotAndDotDot | QDir::System, QDir::Name);
}

// The changes a tool page offers, under its toolbar (nullptr: none). Each goes through sysadmin-helper
// (administrator password) and reads the page again when it succeeded.
QWidget *actionBar(const QString &key, QWidget *page, const std::function<void()> &rerun) {
    auto *bar = new QWidget;
    auto *h = new QHBoxLayout(bar);
    h->setContentsMargins(0, 0, 0, 0);
    auto *label = new QLabel(tl("Change:"));
    label->setProperty("role", "muted");
    h->addWidget(label);
    auto after = [rerun](bool ok) { if (ok) rerun(); };
    auto button = [&](const QString &text, const QString &tip, std::function<void()> f) {
        auto *b = new QPushButton(text);
        b->setToolTip(tip);
        QObject::connect(b, &QPushButton::clicked, page, std::move(f));
        h->addWidget(b);
        return b;
    };
    auto act = [page, after](const QString &title, const QString &confirm, const QJsonObject &r) { sysadmin::act(page, title, confirm, r, after); };
    if (key == QLatin1String("CPU")) {
        const bool boost = QFile::exists(QStringLiteral("/sys/devices/system/cpu/cpufreq/boost")) ||
                           QFile::exists(QStringLiteral("/sys/devices/system/cpu/cpufreq/policy0/boost"));
        if (boost) {
            button(tl("Boost on"), tl("Lets the cores run above their base clock (until reboot)."), [act] {
                act(tl("CPU boost"), QString(), req("sysfs_set", {{"key", "cpu_boost"}, {"value", "1"}})); });
            button(tl("Boost off"), tl("Holds the cores at their base clock: cooler and quieter, much slower (until reboot)."), [act] {
                act(tl("CPU boost"), tl("Turn CPU boost off until the next boot?"), req("sysfs_set", {{"key", "cpu_boost"}, {"value", "0"}})); });
        }
        button(tl("Remove clock cap"), tl("Sets every policy's scaling_max_freq back to the hardware maximum (until something writes it again)."), [act] {
            act(tl("CPU clock cap"), tl("Raise scaling_max_freq to the hardware maximum on every CPU policy?"), req("sysfs_set", {{"key", "cpu_uncap"}})); });
    } else if (key == QLatin1String("Memory")) {
        button(tl("Drop caches"), tl("Writes the page cache back and empties it (sync; drop_caches=3). Only useful before a measurement: the next reads come from disk."), [act] {
            act(tl("Drop caches"), tl("Empty the page cache, dentries and inodes? Programs get slower for a while as files are read from disk again."), req("drop_caches")); });
        if (QFile::exists(QStringLiteral("/sys/module/zswap/parameters/enabled"))) {
            const bool on = readFile(QStringLiteral("/sys/module/zswap/parameters/enabled")).trimmed() == QLatin1String("Y");
            button(on ? tl("zswap off") : tl("zswap on"), tl("Switches the compressed swap cache until the next boot (zswap.enabled= on the kernel command line keeps it)."), [act, on] {
                act(tl("zswap"), QString(), req("sysfs_set", {{"key", "zswap"}, {"value", on ? "N" : "Y"}})); });
        }
    } else if (key == QLatin1String("Storage")) {
        button(tl("TRIM all filesystems"), tl("fstrim -av: tells the SSDs which blocks are free. Safe; may take a minute."), [act] {
            act(tl("TRIM"), tl("Run fstrim on every mounted filesystem that supports it?"), req("fstrim")); });
    } else if (key == QLatin1String("SMART details")) {
        auto *dev = new QComboBox;
        for (const QString &d : dirNames(QStringLiteral("/sys/block"), {QStringLiteral("nvme*"), QStringLiteral("sd*")})) dev->addItem(d);
        if (!dev->count()) { delete bar; return nullptr; }
        h->addWidget(dev);
        for (const char *kind : {"short", "long"}) {
            const bool shortTest = QLatin1String(kind) == QLatin1String("short");
            button(shortTest ? tl("Short self-test") : tl("Long self-test"),
                   shortTest ? tl("A couple of minutes; the drive keeps working. The result appears here (and in Drive health) when it ends.")
                             : tl("Reads the whole surface: from tens of minutes to hours. The drive keeps working, a bit slower."),
                   [act, dev, kind, shortTest] {
                act(tl("SMART self-test"), (shortTest ? tl("Start a short self-test on %1?") : tl("Start a long self-test on %1?")).arg(dev->currentText()),
                    req("smart_test", {{"device", dev->currentText()}, {"kind", QLatin1String(kind)}}));
            });
        }
    } else if (key == QLatin1String("USB")) {
        auto *dev = new QComboBox;
        for (const QString &d : dirNames(QStringLiteral("/sys/bus/usb/devices"))) {
            if (d.contains(QLatin1Char(':')) || !QFile::exists(QStringLiteral("/sys/bus/usb/devices/%1/power/control").arg(d))) continue;
            const QString base = QStringLiteral("/sys/bus/usb/devices/%1/").arg(d);
            QString name = readFile(base + QStringLiteral("product")).trimmed();
            if (name.startsWith(QLatin1String("(cannot"))) name.clear();
            const QString ctl = readFile(base + QStringLiteral("power/control")).trimmed();
            dev->addItem(QStringLiteral("%1  %2  [%3]").arg(d, name, ctl), d);
        }
        if (!dev->count()) { delete bar; return nullptr; }
        h->addWidget(dev, 1);
        button(tl("Allow autosuspend"), tl("power/control = auto: the device sleeps when idle (until reboot or re-plug)."), [act, dev] {
            act(tl("USB power"), QString(), req("usb_power", {{"device", dev->currentData().toString()}, {"control", "auto"}})); });
        button(tl("Keep powered"), tl("power/control = on: never suspended. Fixes a mouse or dongle that lags after idle (until reboot or re-plug)."), [act, dev] {
            act(tl("USB power"), QString(), req("usb_power", {{"device", dev->currentData().toString()}, {"control", "on"}})); });
    } else if (key == QLatin1String("PCIe detail")) {
        const QString path = QStringLiteral("/sys/module/pcie_aspm/parameters/policy");
        if (!QFile::exists(path)) { delete bar; return nullptr; }
        auto *pol = new QComboBox;
        QString now;
        for (const QString &c : readFile(path).split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            const QString v = QString(c).remove(QLatin1Char('[')).remove(QLatin1Char(']')).trimmed();
            pol->addItem(v);
            if (c.startsWith(QLatin1Char('['))) now = v;
        }
        pol->setCurrentText(now);
        h->addWidget(new QLabel(tl("ASPM policy")));
        h->addWidget(pol);
        button(tl("Apply"), tl("PCIe link power management until reboot (pcie_aspm.policy= keeps it). performance: lowest latency; powersupersave: most saving."), [act, pol] {
            act(tl("ASPM policy"), tl("Set the PCIe ASPM policy to %1 until the next boot?").arg(pol->currentText()), req("sysfs_set", {{"key", "pcie_aspm"}, {"value", pol->currentText()}})); });
    } else if (key == QLatin1String("Kernel")) {
        auto *mod = new QLineEdit;
        mod->setPlaceholderText(tl("module name"));
        mod->setFixedWidth(200);
        auto *comp = new QCompleter(dirNames(QStringLiteral("/sys/module")), mod);
        comp->setCaseSensitivity(Qt::CaseInsensitive);
        mod->setCompleter(comp);
        h->addWidget(mod);
        button(tl("Load"), tl("modprobe <name>: until the next boot (Configuration files → modules-load keeps it)."), [act, mod] {
            if (!mod->text().trimmed().isEmpty()) act(tl("Load module"), QString(), req("module", {{"name", mod->text().trimmed()}, {"load", true}})); });
        button(tl("Unload"), tl("modprobe -r <name>: refused while something uses the module."), [act, mod] {
            if (!mod->text().trimmed().isEmpty())
                act(tl("Unload module"), tl("Unload %1? Devices it drives stop working until it is loaded again.").arg(mod->text().trimmed()),
                    req("module", {{"name", mod->text().trimmed()}, {"load", false}})); });
    } else if (key == QLatin1String("Firmware updates")) {
        // fwupd asks for authorisation itself (its own polkit action): run as the user.
        button(tl("Install updates…"), tl("fwupdmgr update: installs the updates listed above; most apply at the next reboot."), [page, rerun] {
            if (QMessageBox::question(page, tl("Firmware Updates"), tl("Install every firmware update fwupd offers? Keep the charger connected; "
                                      "do not switch off while an update is written."), QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
                return;
            const QString exe = findExe(QStringLiteral("fwupdmgr"));
            if (exe.isEmpty()) { QMessageBox::warning(page, tl("Firmware Updates"), tl("fwupdmgr is not installed (%1).").arg(installHint(QStringLiteral("fwupdmgr")))); return; }
            auto *p = new QProcess(page);
            p->setProcessChannelMode(QProcess::MergedChannels);
            QObject::connect(p, &QProcess::finished, page, [p, page, rerun](int code, QProcess::ExitStatus) {
                QMessageBox box(code == 0 ? QMessageBox::Information : QMessageBox::Warning, tl("Firmware Updates"),
                                code == 0 ? tl("fwupdmgr finished.") : tl("fwupdmgr reported a problem (exit %1).").arg(code), QMessageBox::Ok, page);
                box.setDetailedText(QString::fromUtf8(p->readAll()));
                box.exec();
                p->deleteLater();
                rerun();
            });
            p->start(exe, {QStringLiteral("update"), QStringLiteral("-y"), QStringLiteral("--no-reboot-check")});
        });
    } else {
        delete bar;
        return nullptr;
    }
    h->addStretch(1);
    return bar;
}

class ToolPage : public QWidget {
public:
    explicit ToolPage(const Spec &s) : spec_(s) {
        auto *v = new QVBoxLayout(this);
        v->setContentsMargins(8, 6, 8, 6);
        auto *top = new QHBoxLayout;
        auto *about = new QLabel(QCoreApplication::translate("systools", s.about));
        about->setWordWrap(true);
        about->setProperty("role", "muted");
        top->addWidget(about, 1);
        if (s.search) {
            find_ = new QLineEdit;
            find_->setPlaceholderText(QCoreApplication::translate("systools", "Find in this output…"));
            find_->setToolTip(QCoreApplication::translate("systools", "Every match is highlighted; Enter jumps to the next one."));
            find_->setClearButtonEnabled(true);
            find_->setFixedWidth(220);
            findCount_ = new QLabel;
            findCount_->setProperty("role", "muted");
            findCount_->setMinimumWidth(findCount_->fontMetrics().horizontalAdvance(QStringLiteral("0000/0000")));
            top->addWidget(find_);
            top->addWidget(findCount_);
        }
        serialsBtn_ = new QPushButton(QStringLiteral("Show serials"));
        serialsBtn_->setToolTip(QStringLiteral("Serial numbers and UUIDs are hidden; click one in the output to show just that one."));
        serialsBtn_->setVisible(false);
        top->addWidget(serialsBtn_);
        auto *run = new QPushButton(QStringLiteral("Run again"));
        auto *copy = new QPushButton(QStringLiteral("Copy"));
        top->addWidget(run);
        top->addWidget(copy);
        v->addLayout(top);
        if (QWidget *bar = actionBar(stableToolKey(s.title), this, [this] { ran_ = false; start(); })) v->addWidget(bar);
        text_ = new QTextBrowser;
        text_->setOpenLinks(false);  // anchors are serial toggles, not links
        text_->setLineWrapMode(QTextEdit::NoWrap);
        text_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        v->addWidget(text_, 1);
        if (s.manual) text_->setPlainText(QCoreApplication::translate("systools", "Not run yet: press \"Run again\" to read this page."));
        connect(text_, &QTextBrowser::anchorClicked, this, [this](const QUrl &u) {
            bool ok = false;
            const int i = u.toString().mid(2).toInt(&ok);  // "s:<index>"
            if (!ok) return;
            if (shown_.contains(i)) shown_.remove(i); else shown_.insert(i);
            render();
        });
        connect(serialsBtn_, &QPushButton::clicked, this, [this] {
            // Any hidden → show all; all shown → hide all.
            if (shown_.size() < serials_) { for (int i = 0; i < serials_; ++i) shown_.insert(i); }
            else shown_.clear();
            render();
        });
        if (find_) {
            connect(find_, &QLineEdit::textChanged, this, [this] { findNext(true); });
            connect(find_, &QLineEdit::returnPressed, this, [this] { findNext(false); });
        }
        connect(run, &QPushButton::clicked, this, [this] { start(); });
        connect(copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(text_->toPlainText()); });
    }

protected:
    void showEvent(QShowEvent *e) override {
        QWidget::showEvent(e);
        if (!ran_ && !spec_.manual) start();  // lazy: nothing runs until the page is opened
    }

private:
    void start() {
        if (busy_) return;
        ran_ = busy_ = true;
        out_.clear();
        text_->setPlainText(QStringLiteral("Running…"));
        serialsBtn_->setVisible(false);
        if (spec_.rootTool) {
            privileged::run(privileged::helperPath(QStringLiteral("tune-helper")),
                            QJsonObject{{"op", "tool"}, {"tool", QString::fromLatin1(spec_.rootTool)}}, this,
                            [this](const privileged::Result &r) {
                busy_ = false;
                setOutput(r.ok() ? r.json.value("output").toString() : QStringLiteral("Not run: ") + r.message());
            }, 120000);
            return;
        }
        step(0);
    }

    void finish() {
        busy_ = false;
        setOutput(out_);
    }

    void setOutput(const QString &raw) {
        raw_ = raw;
        shown_.clear();  // a fresh run starts hidden again
        render(false);
    }

    // Output as HTML with every serial behind a click-to-toggle anchor.
    // Copy takes the text as displayed, so hidden serials stay hidden there too.
    void render(bool keepScroll = true) {
        const int vs = text_->verticalScrollBar()->value(), hs = text_->horizontalScrollBar()->value();
        QString html = QStringLiteral("<pre style='margin:0'>");
        qsizetype at = 0;
        int n = 0;
        auto it = SERIAL_RE.globalMatch(raw_);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const QString val = m.captured(2);
            if (PLACEHOLDERS.contains(val) || val.startsWith(QLatin1String("Not ")) || val.startsWith(QLatin1String("To Be Filled"))) continue;
            html += raw_.mid(at, m.capturedStart(2) - at).toHtmlEscaped();
            const bool show = shown_.contains(n);
            html += QStringLiteral("<a href='s:%1' style='color:%2; text-decoration:none'>%3</a>")
                        .arg(n).arg(QString::fromLatin1(show ? theme::ACCENT : theme::MUTED))
                        .arg(show ? val.toHtmlEscaped() : QStringLiteral("••••••••  [hidden]"));
            at = m.capturedEnd(2);
            ++n;
        }
        html += raw_.mid(at).toHtmlEscaped() + QStringLiteral("</pre>");
        serials_ = n;
        text_->setHtml(html);
        serialsBtn_->setVisible(n > 0);
        serialsBtn_->setText(shown_.size() < n ? QStringLiteral("Show serials") : QStringLiteral("Hide serials"));
        if (keepScroll) { text_->verticalScrollBar()->setValue(vs); text_->horizontalScrollBar()->setValue(hs); }
        highlight();   // setHtml dropped the previous highlights
    }

    // Jump to the next match of the find box (from the top when `restart`), wrapping at the end.
    void findNext(bool restart) {
        const QString q = find_->text();
        QTextCursor c = text_->textCursor();
        if (restart) c.movePosition(QTextCursor::Start);
        else c.setPosition(c.selectionEnd());
        if (!q.isEmpty()) {
            QTextCursor hit = text_->document()->find(q, c);
            if (hit.isNull()) hit = text_->document()->find(q, 0);   // wrap
            if (!hit.isNull()) c = hit;
        }
        if (q.isEmpty() || c.selectedText().isEmpty()) c.clearSelection();
        text_->setTextCursor(c);
        if (c.hasSelection()) text_->ensureCursorVisible();
        highlight();
    }

    // Every match of the find box gets a highlight; the current one (the cursor's selection) a stronger one.
    void highlight() {
        if (!find_) return;
        QList<QTextEdit::ExtraSelection> marks;
        const QString q = find_->text();
        const QTextCursor cur = text_->textCursor();
        int n = 0, at = 0;
        if (!q.isEmpty()) {
            for (QTextCursor c = text_->document()->find(q, 0); !c.isNull(); c = text_->document()->find(q, c)) {
                ++n;
                const bool current = c.selectionStart() == cur.selectionStart() && c.selectionEnd() == cur.selectionEnd();
                if (current) at = n;
                QTextEdit::ExtraSelection m;
                m.cursor = c;
                m.format.setBackground(QColor(QLatin1String(current ? theme::ACCENT : theme::ACCENT_SOFT)));
                m.format.setForeground(QColor(QLatin1String(current ? theme::BG0 : theme::FG)));
                marks << m;
            }
        }
        text_->setExtraSelections(marks);
        findCount_->setText(q.isEmpty() ? QString()
                            : n ? QStringLiteral("%1/%2").arg(at ? QString::number(at) : QStringLiteral("–")).arg(n)
                                : QCoreApplication::translate("systools", "no match"));
    }

    void step(int i) {
        if (i >= spec_.cmds.size()) { finish(); return; }
        const QStringList c = spec_.cmds.at(i);
        const QString shown = c.join(QLatin1Char(' '));
        if (c.first() == QLatin1String("@file")) { out_ += "$ cat " + c.at(1) + '\n' + readFile(c.at(1)) + '\n'; step(i + 1); return; }
        if (c.first() == QLatin1String("@pcilink")) { out_ += QStringLiteral("$ PCIe links (sysfs)\n") + pciLinks() + '\n'; step(i + 1); return; }
        if (c.first() == QLatin1String("@glob")) {
            const QString pat = c.at(1);
            const QString dir = pat.section('/', 0, -3), sub = pat.section('/', -2, -2), name = pat.section('/', -1);
            int found = 0;
            for (const QString &e : QDir(dir).entryList({sub}, QDir::Dirs | QDir::NoDotAndDotDot))
                for (const QString &f : QDir(dir + '/' + e).entryList({name}, QDir::Files)) {
                    out_ += "$ cat " + dir + '/' + e + '/' + f + '\n' + readFile(dir + '/' + e + '/' + f) + '\n';
                    ++found;
                }
            // A page must never stay blank: say that nothing matched.
            if (!found) out_ += "$ cat " + pat + '\n' + QCoreApplication::translate("systools", "(no matching files)") + "\n\n";
            step(i + 1);
            return;
        }
        if (c.first() == QLatin1String("@dir")) {
            const QString dir = c.at(1);
            for (const QString &f : QDir(dir).entryList(QDir::Files, QDir::Name))
                out_ += "$ cat " + dir + '/' + f + '\n' + readFile(dir + '/' + f) + '\n';
            step(i + 1);
            return;
        }
        if (c.first() == QLatin1String("@tainted")) { out_ += QStringLiteral("$ cat /proc/sys/kernel/tainted (decoded)\n") + taintedText() + '\n'; step(i + 1); return; }
        if (c.first() == QLatin1String("@microcode")) { out_ += QStringLiteral("$ /proc/cpuinfo (microcode)\n") + microcodeText() + '\n'; step(i + 1); return; }
        const QString exe = findExe(c.first());
        if (exe.isEmpty()) {
            out_ += "$ " + shown + '\n' + QCoreApplication::translate("systools", "(not installed — %1)").arg(installHint(c.first())) + "\n\n";
            step(i + 1);
            return;
        }
        auto *p = new QProcess(this);
        p->setProcessChannelMode(QProcess::MergedChannels);
        QPointer<QProcess> guard(p);
        connect(p, &QProcess::finished, this, [this, p, i, shown](int, QProcess::ExitStatus) {
            out_ += "$ " + shown + '\n' + QString::fromUtf8(p->readAll().left(MAX_OUT)) + '\n';
            p->deleteLater();
            step(i + 1);
        });
        connect(p, &QProcess::errorOccurred, this, [this, p, i, shown](QProcess::ProcessError e) {
            if (e != QProcess::FailedToStart) return;
            out_ += "$ " + shown + "\n(failed to start)\n\n";
            p->deleteLater();
            step(i + 1);
        });
        QTimer::singleShot(CMD_TIMEOUT_MS, p, [guard] { if (guard && guard->state() != QProcess::NotRunning) guard->kill(); });
        p->start(exe, c.mid(1));
    }

    Spec spec_;
    QTextBrowser *text_;
    QPushButton *serialsBtn_;
    QLineEdit *find_ = nullptr;
    QLabel *findCount_ = nullptr;
    QString out_, raw_;
    QSet<int> shown_;
    int serials_ = 0;
    bool ran_ = false, busy_ = false;
};

// An interpreted page (S01 … S18): a diag::Report instead of raw output. Extra commands of its spec run
// first (asynchronously, bounded); their output feeds the report (S15) or its technical details.
class DiagToolPage : public QWidget {
public:
    DiagToolPage(const QString &code, const QList<QStringList> &cmds, const QString &about) : code_(code), cmds_(cmds) {
        auto *v = new QVBoxLayout(this);
        v->setContentsMargins(8, 6, 8, 6);
        auto *top = new QHBoxLayout;
        auto *a = new QLabel(about);
        a->setWordWrap(true);
        a->setProperty("role", "muted");
        top->addWidget(a, 1);
        if (code_ == QLatin1String("S01")) {
            auto *save = new QPushButton(QCoreApplication::translate("systools", "Save baseline"));
            save->setToolTip(QCoreApplication::translate("systools", "Keep this inventory as the reference to compare with later (owner-only file)."));
            top->addWidget(save);
            connect(save, &QPushButton::clicked, this, [this] {
                const QString err = diagnostictools::saveHardwareBaseline();
                status_->setText(err.isEmpty() ? QCoreApplication::translate("systools", "Baseline saved.") : err);
                start();
            });
        }
        if (code_ == QLatin1String("S15")) {
            auto *settings = new QPushButton(QCoreApplication::translate("systools", "Settings…"));
            settings->setToolTip(QCoreApplication::translate("systools", "Edit well-known PipeWire and WirePlumber options, with a hint for each."));
            top->addWidget(settings);
            connect(settings, &QPushButton::clicked, this, [this] { PipewireConfigDialog(this).exec(); start(); });
        }
        if (code_ == QLatin1String("S03")) {
            auto *panel = new QPushButton(QCoreApplication::translate("systools", "Panel controls…"));
            panel->setToolTip(QCoreApplication::translate("systools", "Set the built-in screen's brightness and refresh rate."));
            top->addWidget(panel);
            connect(panel, &QPushButton::clicked, this, [this] { PanelControlsDialog(this).exec(); start(); });
        }
        when_ = new QLabel;
        when_->setProperty("role", "muted");
        top->addWidget(when_);
        run_ = new QPushButton(QCoreApplication::translate("systools", "Run again"));
        top->addWidget(run_);
        v->addLayout(top);
        status_ = new QLabel;
        status_->setProperty("role", "muted");
        v->addWidget(status_);
        view_ = new DiagReportView;
        v->addWidget(view_, 1);
        connect(run_, &QPushButton::clicked, this, [this] { start(); });
        connect(view_, &DiagReportView::fixApplied, this, [this] { start(); });
    }

protected:
    void showEvent(QShowEvent *e) override {
        QWidget::showEvent(e);
        if (!ran_) start();
    }

private:
    void start() {
        if (busy_) return;
        ran_ = busy_ = true;
        run_->setEnabled(false);
        outputs_.clear();
        view_->setMessage(QCoreApplication::translate("systools", "Reading…"));
        step(1);  // cmds_[0] is the @diag marker
    }
    void step(int i) {
        if (i >= cmds_.size()) { finish(); return; }
        const QStringList c = cmds_.at(i);
        const QString key = c.join(QLatin1Char(' '));
        const QString exe = findExe(c.first());
        if (exe.isEmpty()) { outputs_.insert(key, QString()); step(i + 1); return; }
        auto *p = new QProcess(this);
        p->setProcessChannelMode(QProcess::MergedChannels);
        QPointer<QProcess> guard(p);
        connect(p, &QProcess::finished, this, [this, p, i, key](int code, QProcess::ExitStatus st) {
            outputs_.insert(key, st == QProcess::NormalExit && code == 0 ? QString::fromUtf8(p->readAll().left(MAX_OUT)) : QString());
            p->deleteLater();
            step(i + 1);
        });
        connect(p, &QProcess::errorOccurred, this, [this, p, i, key](QProcess::ProcessError e) {
            if (e != QProcess::FailedToStart) return;
            outputs_.insert(key, QString());
            p->deleteLater();
            step(i + 1);
        });
        QTimer::singleShot(CMD_TIMEOUT_MS, p, [guard] { if (guard && guard->state() != QProcess::NotRunning) guard->kill(); });
        p->start(exe, c.mid(1));
    }
    void finish() {
        busy_ = false;
        run_->setEnabled(true);
        diag::Report r;
        if (code_ == QLatin1String("S01")) {
            QString at, err;
            const QJsonObject base = diagnostictools::loadHardwareBaseline(&at, &err);
            r = diagnostictools::hardwareSnapshot(base, at, diagnostictools::snapshot());
            if (base.isEmpty() && !err.isEmpty()) status_->setText(err);
        } else if (code_ == QLatin1String("S03")) r = diagnostictools::displays();
        else if (code_ == QLatin1String("S06")) r = diagnostictools::nvidiaParams();
        else if (code_ == QLatin1String("S15")) r = systemdiagnostics::audio(outputs_);
        else r = systemdiagnostics::byCode(code_);
        if (code_ != QLatin1String("S15"))
            for (auto it = outputs_.cbegin(); it != outputs_.cend(); ++it)
                r.raw += QStringLiteral("\n$ %1\n%2").arg(it.key(), it.value().isEmpty() ? QCoreApplication::translate("systools", "(not installed or failed)") : it.value());
        view_->setReport(r);
        when_->setText(QCoreApplication::translate("systools", "Read at %1").arg(QTime::currentTime().toString(QStringLiteral("HH:mm"))));
    }

    QString code_;
    QList<QStringList> cmds_;
    QHash<QString, QString> outputs_;
    QPushButton *run_;
    QLabel *status_, *when_;
    DiagReportView *view_;
    bool ran_ = false, busy_ = false;
};

} // namespace

namespace systools {

QString installCommand(const QString &tool) { return installHint(tool); }

void addPages(QTabWidget *tabs) {
    const Spec specs[] = {
        {QT_TRANSLATE_NOOP("systools", "PCIe Link Negotiation Lab"),
         QT_TRANSLATE_NOOP("systools", "S04: the link each PCIe device trained at against what it and its slot can do, with its error counters. Never retrains a link or changes ASPM."),
         {{"@diag", "S04"}}},
        {QT_TRANSLATE_NOOP("systools", "Wi-Fi / Bluetooth Firmware Doctor"),
         QT_TRANSLATE_NOOP("systools", "S07: wireless adapters, their drivers and radio switches, and firmware files the kernel could not load. No driver reload."),
         {{"@diag", "S07"}}},
        {QT_TRANSLATE_NOOP("systools", "Power Limit Source Map"),
         QT_TRANSLATE_NOOP("systools", "S08: firmware power targets (PPT), platform profile, cpufreq caps, RAPL and the power source. Nothing is written or woken."),
         {{"@diag", "S08"}}},
        {QT_TRANSLATE_NOOP("systools", "Memory Architecture Explorer"),
         QT_TRANSLATE_NOOP("systools", "S10: RAM, swap, zram / zswap, huge pages, memory pressure, NUMA and L3 domains, with what to change."),
         {{"@diag", "S10"}}},
        {QT_TRANSLATE_NOOP("systools", "ACPI / WMI Firmware Browser"),
         QT_TRANSLATE_NOOP("systools", "S11: WMI objects and their drivers, firmware settings and ACPI tables. Never calls a firmware method or reads a password."),
         {{"@diag", "S11"}}},
        {QT_TRANSLATE_NOOP("systools", "USB-C PD / Charger Inspector"),
         QT_TRANSLATE_NOOP("systools", "S13: the power profiles the charger offers, what the laptop draws, and the battery's condition."),
         {{"@diag", "S13"}}},
        {QT_TRANSLATE_NOOP("systools", "PipeWire Audio Doctor"),
         QT_TRANSLATE_NOOP("systools", "S15: sound cards and codecs, which sound servers run (and whether two fight), PipeWire's view. The report restarts nothing; Settings… edits PipeWire / WirePlumber options and restarts the sound servers when you apply."),
         {{"@diag", "S15"}, {"wpctl", "status"}, {"pactl", "info"}}},
        {QT_TRANSLATE_NOOP("systools", "Kernel Feature & Firmware Check"),
         QT_TRANSLATE_NOOP("systools", "S18: kernel options Centurion relies on, firmware the kernel could not load or decompress, devices without a driver."),
         {{"@diag", "S18"}}},
        {QT_TRANSLATE_NOOP("systools", "Hardware Snapshot"),
         QT_TRANSLATE_NOOP("systools", "S01: compare PCI devices, drivers, modules, kernel, BIOS, microcode and boot parameters with a baseline you saved."),
         {{"@diag", "S01"}}},
        {QT_TRANSLATE_NOOP("systools", "EDID / VRR"),
         QT_TRANSLATE_NOOP("systools", "S03: each connected display's identity, modes and variable-refresh range, size, bit depth, HDR and colour support from its EDID; damaged EDIDs; backlight devices. Panel controls… sets the built-in screen's brightness and refresh rate."),
         {{"@diag", "S03"}}},
        {QT_TRANSLATE_NOOP("systools", "NVIDIA Module Parameters"),
         QT_TRANSLATE_NOOP("systools", "S06: NVIDIA options configured against the values in effect, with what they do. Never wakes the GPU."),
         {{"@diag", "S06"}, {"modinfo", "-p", "nvidia"}}},
        {QT_TRANSLATE_NOOP("systools", "Sensors"), "lm-sensors: temperatures, fan speeds, voltages and power of every hwmon chip.", {{"sensors"}}},
        {QT_TRANSLATE_NOOP("systools", "CPU"), "Topology, caches, frequencies and the active cpufreq driver/governor.",
         {{"lscpu"}, {"lscpu", "-e"}, {"cpupower", "frequency-info"}}, nullptr, false, true},
        {QT_TRANSLATE_NOOP("systools", "Memory"), "Usage, swap/zram and NUMA layout.", {{"free", "-h"}, {"swapon", "--show"}, {"numactl", "-H"}}},
        {QT_TRANSLATE_NOOP("systools", "DMI"), "dmidecode (root): firmware/BIOS, board, and every memory module with its part number and speed.", {}, "dmidecode", false, true},
        {QT_TRANSLATE_NOOP("systools", "PCI"), "Devices with their kernel drivers, the bus tree, and the live PCIe link of GPUs and NVMe drives.",
         {{"lspci", "-nnk"}, {"lspci", "-tv"}, {"@pcilink"}}, nullptr, false, true},
        {QT_TRANSLATE_NOOP("systools", "PCIe Detail"), "lspci -vv (root): link capabilities/status, ASPM and AER capability of every device.", {}, "pcie", false, true},
        {QT_TRANSLATE_NOOP("systools", "USB"), "Connected USB devices and the port tree.", {{"lsusb"}, {"lsusb", "-t"}}},
        {QT_TRANSLATE_NOOP("systools", "Storage"), "Block devices, schedulers and mounted filesystems.",
         {{"lsblk", "-o", "NAME,MODEL,SIZE,TYPE,FSTYPE,MOUNTPOINTS,ROTA,SCHED"}, {"df", "-hT", "-x", "tmpfs", "-x", "devtmpfs"}}},
        {QT_TRANSLATE_NOOP("systools", "SMART Details"), "Drive health (root): smartctl -a, or nvme smart-log, for every disk.", {}, "smart", false, true},
        {QT_TRANSLATE_NOOP("systools", "NVIDIA"), "nvidia-smi -q: the full driver/GPU report. Wakes the dGPU if it is powered down.",
         {{"@file", "/proc/driver/nvidia/version"}, {"nvidia-smi", "-q"}}, nullptr, false, true},
        {QT_TRANSLATE_NOOP("systools", "Graphics"), "Vulkan and OpenGL drivers as games see them (may wake the dGPU).",
         {{"vulkaninfo", "--summary"}, {"glxinfo", "-B"}}, nullptr, false, true},
        {QT_TRANSLATE_NOOP("systools", "Kernel"), "Kernel version, boot command line and loaded modules.",
         {{"uname", "-a"}, {"@file", "/proc/cmdline"}, {"lsmod"}}, nullptr, false, true},
        {QT_TRANSLATE_NOOP("systools", "Battery"), "Every power supply as the kernel reports it (capacity, cycles, charge limits).",
         {{"@glob", "/sys/class/power_supply/*/uevent"}}, nullptr, false, true},
        {QT_TRANSLATE_NOOP("systools", "Thermal"),
         QT_TRANSLATE_NOOP("systools", "Every thermal zone: its type, current temperature and trip points. Temperatures are in millidegrees Celsius, as the kernel reports them."),
         {{"@glob", "/sys/class/thermal/thermal_zone*/type"}, {"@glob", "/sys/class/thermal/thermal_zone*/temp"}, {"@glob", "/sys/class/thermal/thermal_zone*/trip_point_*"}}},
        {QT_TRANSLATE_NOOP("systools", "Security"),
         QT_TRANSLATE_NOOP("systools", "Mitigations for CPU vulnerabilities, kernel lockdown, Secure Boot, the kernel taint flags and the microcode revision."),
         {{"@dir", "/sys/devices/system/cpu/vulnerabilities"}, {"@file", "/sys/kernel/security/lockdown"}, {"mokutil", "--sb-state"}, {"@tainted"}, {"@microcode"}}},
        {QT_TRANSLATE_NOOP("systools", "Firmware Updates"),
         QT_TRANSLATE_NOOP("systools", "fwupd: the devices it can update and the firmware updates it finds. Finding updates contacts the LVFS server on the internet, so this page runs only on \"Run again\"."),
         {{"fwupdmgr", "get-devices"}, {"fwupdmgr", "get-updates"}}, nullptr, true, true},
    };
    for (const Spec &s : specs) {
        // No ACPI thermal zone (common on AMD laptops; their temperatures are on the Sensors page): no Thermal page.
        if (QLatin1String(s.title) == QLatin1String("Thermal") &&
            QDir(QStringLiteral("/sys/class/thermal")).entryList({QStringLiteral("thermal_zone*")}, QDir::Dirs | QDir::NoDotAndDotDot).isEmpty())
            continue;
        const QString title = QCoreApplication::translate("systools", s.title);
        // The SMART summary goes in front of the raw output it summarizes.
        // "toolKey": stable historical identifier, independent of translated or re-cased tab text.
        if (s.rootTool && QLatin1String(s.rootTool) == QLatin1String("smart")) {
            auto *smart = new SmartPage;
            smart->setProperty("toolKey", QStringLiteral("SMART"));
            tabs->addTab(smart, QStringLiteral("SMART"));
        }
        // Built on first open: the pages are rarely visited, and every widget that exists at start-up is
        // matched against the application style sheet (theme::applySheet) even while the app sits in the tray.
        const bool diagPage = !s.cmds.isEmpty() && s.cmds.first().first() == QLatin1String("@diag");
        auto *page = new LazyWidget([s, diagPage]() -> QWidget * {
            if (diagPage) return new DiagToolPage(s.cmds.first().at(1), s.cmds, QCoreApplication::translate("systools", s.about));
            return new ToolPage(s);
        });
        page->setProperty("toolKey", stableToolKey(s.title));
        page->setProperty("toolAbout", QCoreApplication::translate("systools", s.about));  // the catalog's tooltip
        tabs->addTab(page, title);
    }
    auto *config = new LazyWidget([] { return new ConfigPage; });
    config->setProperty("toolKey", QStringLiteral("Configuration files"));
    tabs->addTab(config, QCoreApplication::translate("systools", "Configuration Files"));
}

} // namespace systools
