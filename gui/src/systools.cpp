#include "systools.h"

#include "privileged.h"
#include "smartpage.h"
#include "theme.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QTextBrowser>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTabWidget>
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
};

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
        if (fam == QLatin1String("gentoo")) return QStringLiteral("emerge %1").arg(QLatin1String(p.gentoo));
        if (fam == QLatin1String("arch")) return QStringLiteral("pacman -S %1").arg(QLatin1String(p.arch));
        if (fam == QLatin1String("fedora")) return QStringLiteral("dnf install %1").arg(QLatin1String(p.fedora));
        if (fam == QLatin1String("debian")) return QStringLiteral("apt install %1").arg(QLatin1String(p.debian));
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
        serialsBtn_ = new QPushButton(QStringLiteral("Show serials"));
        serialsBtn_->setToolTip(QStringLiteral("Serial numbers and UUIDs are hidden; click one in the output to show just that one."));
        serialsBtn_->setVisible(false);
        top->addWidget(serialsBtn_);
        auto *run = new QPushButton(QStringLiteral("Run again"));
        auto *copy = new QPushButton(QStringLiteral("Copy"));
        top->addWidget(run);
        top->addWidget(copy);
        v->addLayout(top);
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
            for (const QString &e : QDir(dir).entryList({sub}, QDir::Dirs | QDir::NoDotAndDotDot))
                for (const QString &f : QDir(dir + '/' + e).entryList({name}, QDir::Files))
                    out_ += "$ cat " + dir + '/' + e + '/' + f + '\n' + readFile(dir + '/' + e + '/' + f) + '\n';
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
    QString out_, raw_;
    QSet<int> shown_;
    int serials_ = 0;
    bool ran_ = false, busy_ = false;
};

} // namespace

namespace systools {

void addPages(QTabWidget *tabs) {
    const Spec specs[] = {
        {"Sensors", "lm-sensors: temperatures, fan speeds, voltages and power of every hwmon chip.", {{"sensors"}}},
        {"CPU", "Topology, caches, frequencies and the active cpufreq driver/governor.",
         {{"lscpu"}, {"lscpu", "-e"}, {"cpupower", "frequency-info"}}},
        {"Memory", "Usage, swap/zram and NUMA layout.", {{"free", "-h"}, {"swapon", "--show"}, {"numactl", "-H"}}},
        {"DMI", "dmidecode (root): firmware/BIOS, board, and every memory module with its part number and speed.", {}, "dmidecode"},
        {"PCI", "Devices with their kernel drivers, the bus tree, and the live PCIe link of GPUs and NVMe drives.",
         {{"lspci", "-nnk"}, {"lspci", "-tv"}, {"@pcilink"}}},
        {"PCIe detail", "lspci -vv (root): link capabilities/status, ASPM and AER capability of every device.", {}, "pcie"},
        {"USB", "Connected USB devices and the port tree.", {{"lsusb"}, {"lsusb", "-t"}}},
        {"Storage", "Block devices, schedulers and mounted filesystems.",
         {{"lsblk", "-o", "NAME,MODEL,SIZE,TYPE,FSTYPE,MOUNTPOINTS,ROTA,SCHED"}, {"df", "-hT", "-x", "tmpfs", "-x", "devtmpfs"}}},
        {"SMART details", "Drive health (root): smartctl -a, or nvme smart-log, for every disk.", {}, "smart"},
        {"NVIDIA", "nvidia-smi -q: the full driver/GPU report. Wakes the dGPU if it is powered down.",
         {{"@file", "/proc/driver/nvidia/version"}, {"nvidia-smi", "-q"}}},
        {"Graphics", "Vulkan and OpenGL drivers as games see them (may wake the dGPU).",
         {{"vulkaninfo", "--summary"}, {"glxinfo", "-B"}}},
        {"Kernel", "Kernel version, boot command line and loaded modules.",
         {{"uname", "-a"}, {"@file", "/proc/cmdline"}, {"lsmod"}}},
        {"Battery", "Every power supply as the kernel reports it (capacity, cycles, charge limits).",
         {{"@glob", "/sys/class/power_supply/*/uevent"}}},
        {QT_TRANSLATE_NOOP("systools", "Thermal"),
         QT_TRANSLATE_NOOP("systools", "Every thermal zone: its type, current temperature and trip points. Temperatures are in millidegrees Celsius, as the kernel reports them."),
         {{"@glob", "/sys/class/thermal/thermal_zone*/type"}, {"@glob", "/sys/class/thermal/thermal_zone*/temp"}, {"@glob", "/sys/class/thermal/thermal_zone*/trip_point_*"}}},
        {QT_TRANSLATE_NOOP("systools", "Security"),
         QT_TRANSLATE_NOOP("systools", "Mitigations for CPU vulnerabilities, kernel lockdown, Secure Boot, the kernel taint flags and the microcode revision."),
         {{"@dir", "/sys/devices/system/cpu/vulnerabilities"}, {"@file", "/sys/kernel/security/lockdown"}, {"mokutil", "--sb-state"}, {"@tainted"}, {"@microcode"}}},
        {QT_TRANSLATE_NOOP("systools", "Firmware updates"),
         QT_TRANSLATE_NOOP("systools", "fwupd: the devices it can update and the firmware updates it finds. Finding updates contacts the LVFS server on the internet, so this page runs only on \"Run again\"."),
         {{"fwupdmgr", "get-devices"}, {"fwupdmgr", "get-updates"}}, nullptr, true},
    };
    for (const Spec &s : specs) {
        const QString title = QCoreApplication::translate("systools", s.title);
        // The SMART summary goes in front of the raw output it summarizes.
        if (s.rootTool && QLatin1String(s.rootTool) == QLatin1String("smart")) tabs->addTab(new SmartPage, QStringLiteral("SMART"));
        tabs->addTab(new ToolPage(s), title);
    }
}

} // namespace systools
