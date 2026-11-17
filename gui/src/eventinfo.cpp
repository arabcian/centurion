#include "eventinfo.h"

#include <QCoreApplication>
#include <QRegularExpression>

namespace eventinfo {
namespace {

QString tr(const char *s) { return QCoreApplication::translate("eventinfo", s); }

QString cap(const QString &text, const char *pattern, int group = 1) {
    const QRegularExpressionMatch m = QRegularExpression(QLatin1String(pattern)).match(text);
    return m.hasMatch() ? m.captured(group).trimmed() : QString();
}

// NVIDIA's Xid catalog, grouped by what the user can do about them.
enum class XidGroup { App, Bus, Firmware, Memory, Display, Video, Recovery, Other };
XidGroup xidGroup(int code) {
    switch (code) {
    case 8: case 13: case 31: case 43: case 44: case 45: case 69: case 109: return XidGroup::App;
    case 32: case 79: return XidGroup::Bus;
    case 38: case 61: case 62: case 119: case 120: case 143: case 175: return XidGroup::Firmware;
    case 48: case 63: case 64: case 92: case 94: case 95: case 140: return XidGroup::Memory;
    case 56: case 57: return XidGroup::Display;
    case 68: return XidGroup::Video;
    case 154: return XidGroup::Recovery;
    default: return XidGroup::Other;
    }
}

void xid(int code, const QString &text, Info &i) {
    const QString name = cap(text, R"(name=([^,]+))"), pid = cap(text, R"(pid=(\d+))"), pci = cap(text, R"(PCI:([0-9a-fA-F:.]+))");
    if (!name.isEmpty() && name != QLatin1String("<unknown>"))
        i.facts << tr("Process using the GPU: %1%2").arg(name, pid.isEmpty() ? QString() : tr(" (pid %1)").arg(pid));
    if (!pci.isEmpty()) i.facts << tr("GPU address: %1").arg(pci);
    i.facts << tr("Xid code: %1").arg(code);
    switch (xidGroup(code)) {
    case XidGroup::App:
        i.meaning = tr("The GPU stopped or faulted while running work for one program. The driver usually kills that program's GPU "
                       "context and the rest of the system keeps working.");
        i.causes = {tr("A bug in the game or application (or its shader cache)"), tr("An unstable GPU overclock, undervolt or memory clock"),
                    tr("A driver bug, especially right after a driver or kernel update")};
        i.steps = {tr("Note which program was running (see the details below) and whether the event repeats with the same program only."),
                   tr("If a custom GPU curve is active, reset it to stock and run the same workload again."),
                   tr("Clear the program's shader cache (e.g. ~/.cache/nvidia, Steam's shadercache folder of that game)."),
                   tr("Update the NVIDIA driver; if the problem started with an update, try the previous version.")};
        i.urgency = tr("Usually not dangerous on its own; worth acting on if it repeats.");
        break;
    case XidGroup::Bus:
        i.meaning = tr("The GPU stopped answering on the PCIe bus (\"fallen off the bus\") or sent corrupted data. Graphics usually "
                       "freeze until a reboot.");
        i.causes = {tr("An unstable GPU curve, overclock or undervolt"), tr("Power delivery: load spikes, a weak supply, or a battery-only power limit"),
                    tr("Overheating"), tr("On laptops: the dGPU failing to wake from its power-saving state (D3cold)"),
                    tr("A loose card, riser or power cable (desktops)")};
        i.steps = {tr("Reset any custom GPU curve to stock and see whether it comes back."),
                   tr("Check GPU temperatures under load (Cooling page)."),
                   tr("On a laptop open dGPU power: if the event follows idle periods, the runtime power management is the suspect."),
                   tr("Update the BIOS/EC firmware and the NVIDIA driver."),
                   tr("Desktops: reseat the card and its power cables.")};
        i.urgency = tr("Serious: the GPU had to be reset or the session lost graphics. Save your work and reboot.");
        break;
    case XidGroup::Firmware:
        i.meaning = tr("The GPU's internal controller or its GSP firmware stopped responding to the driver.");
        i.causes = {tr("A driver / GSP firmware bug"), tr("An unstable GPU curve or memory clock"), tr("Overheating or power problems")};
        i.steps = {tr("Update the NVIDIA driver (GSP firmware ships with it)."),
                   tr("Reset any custom GPU curve to stock."),
                   tr("With the proprietary (closed) kernel modules, NVreg_EnableGpuFirmware=0 disables GSP for a test; the open modules require GSP."),
                   tr("If it happens after resume or long idle, check the dGPU power page.")};
        i.urgency = tr("Serious: the GPU may need a reboot to work again.");
        break;
    case XidGroup::Memory:
        i.meaning = tr("The GPU reported a video-memory (VRAM) error, or retired a page / remapped a row because of one.");
        i.causes = {tr("A VRAM overclock that is too high"), tr("Overheating memory chips"), tr("Failing video memory")};
        i.steps = {tr("Remove any memory overclock and re-test."),
                   tr("Run Centurion's GPU stress test with the VRAM check."),
                   tr("If errors persist at stock clocks, the card may need service.")};
        i.urgency = tr("Serious if it repeats at stock clocks.");
        break;
    case XidGroup::Display:
        i.meaning = tr("The display engine reported an error while driving a screen.");
        i.causes = {tr("A cable, dock or adapter (especially DisplayPort MST)"), tr("A display driver bug"), tr("An unusual refresh rate or mode")};
        i.steps = {tr("Try another cable or port and the screen's standard mode."), tr("Update the driver and dock firmware.")};
        i.urgency = tr("Usually limited to the display output.");
        break;
    case XidGroup::Video:
        i.meaning = tr("The video decoder (NVDEC) reported an error while decoding a video.");
        i.causes = {tr("A corrupt or unusual video stream"), tr("A driver bug in hardware decoding")};
        i.steps = {tr("Note which player or browser was decoding; try software decoding to compare."), tr("Update the driver.")};
        i.urgency = tr("Not dangerous; affects video playback.");
        break;
    case XidGroup::Recovery:
        i.meaning = tr("The driver states which recovery the GPU needs after an earlier error (a GPU reset, or a reboot). It is a "
                       "consequence, not the cause.");
        i.causes = {tr("The Xid logged just before this one")};
        i.steps = {tr("Look at the previous Xid in the list (the title names it when known) and follow its advice."),
                   tr("Do the recovery the record asks for: usually a reboot.")};
        i.urgency = tr("The GPU is not in a usable state until the recovery is done.");
        break;
    case XidGroup::Other:
        i.meaning = tr("The NVIDIA driver reported a GPU error that is not in Centurion's table.");
        i.causes = {tr("See NVIDIA's Xid catalog for this code")};
        i.steps = {tr("Check whether it repeats and with which program."), tr("Reset custom GPU curves to stock if any are active.")};
        i.urgency = tr("Unknown — judge by how often it repeats.");
        break;
    }
}

}  // namespace

Info explain(const QJsonObject &e) {
    const QString kind = e.value(QStringLiteral("kind")).toString().toLower();
    const int code = e.value(QStringLiteral("code")).toInt();
    const QString text = e.value(QStringLiteral("text")).toString();
    const QString title = e.value(QStringLiteral("title")).toString();
    Info i;
    if (kind == QLatin1String("xid")) {
        xid(code ? code : cap(title, R"(Xid (\d+))").toInt(), text, i);
    } else if (kind == QLatin1String("gsp")) {
        i.meaning = tr("The NVIDIA GPU's GSP firmware stopped answering the driver (heartbeat timeout).");
        i.causes = {tr("A driver / GSP firmware bug"), tr("The dGPU waking from a power-saving state"), tr("An unstable GPU curve")};
        i.steps = {tr("Update the NVIDIA driver."), tr("Reset custom GPU curves to stock."),
                   tr("With the proprietary (closed) kernel modules, NVreg_EnableGpuFirmware=0 disables GSP for a test."),
                   tr("On laptops check the dGPU power page.")};
        i.urgency = tr("Serious: graphics may freeze; a reboot is usually needed.");
    } else if (kind == QLatin1String("mce")) {
        const bool corrected = !e.value(QStringLiteral("level")).toString().contains(QLatin1String("critical"));
        i.meaning = corrected ? tr("The processor detected an internal error and corrected it (cache, core or memory controller). "
                                   "Nothing was lost, but corrected errors are an early sign of instability.")
                              : tr("The processor detected an internal error it could not correct. The affected process or the whole "
                                   "system may have stopped.");
        i.causes = {tr("An unstable Curve Optimizer / undervolt (cores at low load or in boost)"),
                    tr("RAM timings or XMP/EXPO profile at the edge of stability"), tr("High temperatures"), tr("Rarely, failing hardware")};
        i.steps = {tr("If a CPU curve or undervolt is active, back it off (Ryzen Curve / Intel Undervolt) and watch for repeats."),
                   tr("If the RAM runs an XMP/EXPO profile or tuned timings, test at stock and run a memory test."),
                   tr("Check CPU temperatures under load."),
                   tr("rasdaemon (ras-mc-ctl --summary) decodes machine checks in more detail.")};
        if (const QString bank = cap(text, R"(Bank (\d+))"); !bank.isEmpty()) {
            i.facts << tr("MCA bank: %1").arg(bank);
            i.facts << tr("On AMD Zen the low banks are core units (sensitive to the curve), higher ones are L3, fabric and memory controllers (RAM).");
        }
        if (const QString cpu = cap(text, R"(CPU (\d+))"); !cpu.isEmpty()) i.facts << tr("Reported by CPU %1").arg(cpu);
        i.urgency = corrected ? tr("Not immediately dangerous; repeats mean the system is not fully stable.")
                              : tr("Serious: data in flight may have been lost.");
    } else if (kind == QLatin1String("aer")) {
        const bool corrected = title.contains(QLatin1String("corrected"), Qt::CaseInsensitive) && !title.contains(QLatin1String("uncorrect"), Qt::CaseInsensitive);
        i.meaning = corrected ? tr("A PCIe link had transmission errors that the hardware fixed by retrying. Harmless once, but a "
                                   "flood slows the device and fills the log.")
                              : tr("A PCIe device reported an error the link could not correct; the device may have been reset.");
        i.causes = {tr("Link power saving (ASPM) on that port"), tr("Signal problems: riser, slot, Thunderbolt/USB4 dock"),
                    tr("Device or BIOS firmware bugs")};
        i.steps = {tr("Find the device: lspci -s <address> (see below)."),
                   tr("Update the BIOS and the device's firmware."),
                   tr("For a test, boot with pcie_aspm.policy=performance (or pcie_aspm=off); if the errors stop, ASPM is the cause."),
                   tr("pci=noaer only hides the reports; it does not fix the link.")};
        // "pcieport <root port>: AER: … received from <device>": the device that erred is the one after "from".
        QString dev = cap(text, R"(from ([0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]))");
        if (dev.isEmpty()) dev = cap(text, R"(([0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]))");
        if (!dev.isEmpty()) i.facts << tr("PCIe device: %1").arg(dev);
        if (const QString type = cap(text, R"(type=([A-Za-z ]+?)[,)])"); !type.isEmpty()) i.facts << tr("Error layer: %1").arg(type);
        i.urgency = corrected ? tr("Low, unless it floods.") : tr("Serious for the affected device.");
    } else if (kind == QLatin1String("lockup")) {
        i.meaning = text.contains(QLatin1String("rcu"), Qt::CaseInsensitive)
            ? tr("A CPU did not pass through a scheduling point for a long time (RCU stall): something kept it busy in the kernel.")
            : tr("A CPU core was stuck in the kernel for many seconds without letting anything else run (soft / hard lockup).");
        i.causes = {tr("A driver stuck waiting for hardware (GPU, storage, Wi-Fi)"), tr("An unstable CPU curve or undervolt"),
                    tr("A kernel bug")};
        i.steps = {tr("Open the kernel log around the event: the lines after it name the function and module that was stuck."),
                   tr("If a CPU curve is active, back it off."),
                   tr("If it happens around suspend/resume, see Sleep & wake."),
                   tr("Update the kernel.")};
        const QString proc = cap(text, R"(stuck for \d+s! \[([^:\]]+))");
        if (!proc.isEmpty()) i.facts << tr("Task running at the time: %1").arg(proc);
        if (const QString cpu = cap(text, R"(CPU#?(\d+))"); !cpu.isEmpty()) i.facts << tr("CPU: %1").arg(cpu);
        if (const QString secs = cap(text, R"(stuck for (\d+)s)"); !secs.isEmpty()) i.facts << tr("Stuck for: %1 s").arg(secs);
        i.urgency = tr("Serious: the system may have frozen briefly or completely.");
    } else if (kind == QLatin1String("nvme")) {
        i.meaning = tr("An NVMe drive did not answer in time and the kernel aborted the request or reset the controller.");
        i.causes = {tr("Drive firmware bugs with power-saving states (APST)"), tr("Overheating drive"), tr("A failing drive"),
                    tr("PCIe link problems")};
        i.steps = {tr("Back up important data now."),
                   tr("Open Drive health and check media errors, temperature and the error log."),
                   tr("Update the drive firmware (fwupdmgr, or the vendor's tool)."),
                   tr("For a test, boot with nvme_core.default_ps_max_latency_us=0 (disables APST); if the timeouts stop, power saving is the cause.")};
        if (const QString dev = cap(text, R"(^nvme (nvme\d+))"); !dev.isEmpty()) i.facts << tr("Drive: %1").arg(dev);
        i.urgency = title.contains(QLatin1String("down")) ? tr("Critical: the drive is not reachable; writes may be lost.")
                                                          : tr("Serious if it repeats.");
    } else if (kind == QLatin1String("fs")) {
        i.meaning = tr("A file system found damaged data or metadata. Depending on the file system it may now be read-only.");
        i.causes = {tr("A failing or disconnecting drive"), tr("Unstable RAM (data corrupted before it was written)"),
                    tr("An earlier crash or power loss")};
        QStringList steps{tr("Back up important files now and avoid large writes.")};
        if (text.contains(QLatin1String("BTRFS")))
            steps << tr("Run btrfs scrub start / and check btrfs device stats /: they show which files are affected.")
                  << tr("Checksum errors on a healthy drive point at RAM: run a memory test.");
        else if (text.contains(QLatin1String("XFS")))
            steps << tr("Run xfs_repair on the unmounted file system (from a live USB).");
        else
            steps << tr("Run e2fsck -f on the unmounted file system (from a live USB, or schedule a check at boot).");
        steps << tr("Check the drive in Drive health.");
        i.steps = steps;
        if (const QString dev = cap(text, R"(\(device ([^)\s,:]+))"); !dev.isEmpty()) i.facts << tr("Device: %1").arg(dev);
        i.urgency = tr("Serious: data may be damaged.");
    } else if (kind == QLatin1String("igpu") || kind == QLatin1String("amdgpu")) {
        const bool amd = kind == QLatin1String("amdgpu") || text.contains(QLatin1String("amdgpu"));
        i.meaning = tr("The integrated GPU hung while running a job and the driver reset it. Usually the screen freezes for a moment.");
        i.causes = {tr("A driver or Mesa bug triggered by a program"), tr("Outdated GPU firmware (linux-firmware)"),
                    amd ? tr("Waking from suspend") : tr("Panel Self Refresh (display power saving)")};
        i.steps = {tr("Update the kernel, Mesa and linux-firmware."), tr("Note which program was running (see below)."),
                   amd ? tr("If it follows suspend, check Sleep & wake.") : tr("If the screen flickers or freezes, test i915.enable_psr=0.")};
        QString proc = cap(text, R"(GPU HANG: [^,]*, in ([^\s\[]+) \[\d+\])");
        if (proc.isEmpty()) proc = cap(text, R"(process ([^\s]+) pid)");
        if (!proc.isEmpty()) i.facts << tr("Program: %1").arg(proc);
        i.urgency = tr("Annoying rather than dangerous, unless it repeats.");
    } else if (kind == QLatin1String("wifi")) {
        i.meaning = tr("The Wi-Fi adapter's firmware crashed and the driver restarted it; the connection drops for a moment.");
        i.causes = {tr("Firmware bugs (often fixed in newer linux-firmware)"), tr("Power saving of the adapter"),
                    tr("Bluetooth coexistence on combo cards")};
        QStringList steps{tr("Update linux-firmware and the kernel."),
                          tr("Turn Wi-Fi power saving off for a test (NetworkManager: wifi.powersave = 2).")};
        if (text.contains(QLatin1String("mt79"))) steps << tr("MediaTek MT7921/7922: mt7921e.disable_aspm=1 helps on many laptops.");
        if (text.contains(QLatin1String("iwlwifi"))) steps << tr("Intel: iwlwifi.power_save=0 and iwlmvm.power_scheme=1 disable its power saving.");
        i.steps = steps;
        if (const QString drv = cap(text, R"(^(\w+))"); !drv.isEmpty()) i.facts << tr("Driver: %1").arg(drv);
        i.urgency = tr("Not dangerous; costs connectivity.");
    } else if (kind == QLatin1String("thermal")) {
        const bool critical = title.contains(QLatin1String("Critical"), Qt::CaseInsensitive);
        i.meaning = critical ? tr("A sensor reached its critical temperature; the system shuts down to protect the hardware.")
                             : tr("The CPU reached its temperature limit and was slowed down (throttled).");
        i.causes = {tr("Dust in the heatsink or fans"), tr("A fan curve that reacts too late"), tr("Dried thermal paste"),
                    tr("Power limits higher than the cooling can handle")};
        i.steps = {tr("Open Cooling: check fan speeds and temperatures under load."), tr("Lower the power limit or the performance profile."),
                   tr("Clean the fans and heatsink; consider new thermal paste.")};
        if (const QString cpu = cap(text, R"(CPU(\d+))"); !cpu.isEmpty()) i.facts << tr("CPU: %1").arg(cpu);
        i.urgency = critical ? tr("Critical.") : tr("Costs performance; harmless to the hardware.");
    } else if (kind == QLatin1String("oom")) {
        i.meaning = tr("Memory (RAM plus swap) ran out and the kernel killed a process to free some.");
        i.causes = {tr("A program using far more memory than usual (a leak)"), tr("Large parallel builds (many compiler jobs)"),
                    tr("Many browser tabs or virtual machines"), tr("No or too little swap / zram")};
        i.steps = {tr("See which process was killed (below) and whether it is a repeat offender."),
                   tr("Add zram or a swap file so the system slows down instead of killing programs."),
                   tr("Limit parallel build jobs (Gentoo: MAKEOPTS; Arch: MAKEFLAGS in makepkg.conf)."),
                   tr("systemd-oomd or earlyoom act earlier and pick the culprit more predictably.")};
        const QString name = cap(text, R"(Killed process \d+ \(([^)]+)\))");
        const QString pid = cap(text, R"(Killed process (\d+))");
        if (!name.isEmpty()) i.facts << tr("Killed: %1 (pid %2)").arg(name, pid);
        if (const QString rss = cap(text, R"(anon-rss:(\d+)kB)"); !rss.isEmpty())
            i.facts << tr("Its memory: %1 MiB").arg(rss.toLongLong() / 1024);
        i.urgency = tr("Unsaved work in the killed program is lost.");
    } else if (kind == QLatin1String("oops")) {
        const bool warning = title.startsWith(QLatin1String("Kernel warning"));
        i.meaning = warning ? tr("Kernel code hit an unexpected condition and printed a warning with a stack trace. The system "
                                 "usually continues normally.")
                            : tr("The kernel hit a bug (an oops). The affected task was killed and the kernel may be in an "
                                 "inconsistent state.");
        i.causes = {tr("A bug in the named driver or module"), tr("An out-of-tree module (NVIDIA, ZFS, VirtualBox) not matching the kernel"),
                    tr("Memory corruption from unstable hardware")};
        i.steps = {tr("Open the kernel log around the event to read the full trace (module, function)."),
                   tr("Update the kernel or the module named; rebuild out-of-tree modules after a kernel update."),
                   warning ? tr("A one-off warning can be ignored; report repeats to the module's bug tracker with the trace.")
                           : tr("Save your work and reboot soon.")};
        if (const QString mod = cap(title, R"( in (\S+)$)"); !mod.isEmpty()) i.facts << tr("Module: %1").arg(mod);
        if (const QString at = cap(text, R"( at ([\w/.-]+:\d+))"); !at.isEmpty()) i.facts << tr("Source location: %1").arg(at);
        if (const QString fn = cap(text, R"(:\d+ ([A-Za-z_][\w.]*)\+0x)"); !fn.isEmpty()) i.facts << tr("Function: %1").arg(fn);
        if (const QString what = cap(text, R"(^BUG: ([^,]+))"); !what.isEmpty()) i.facts << tr("Bug type: %1").arg(what);
        i.urgency = warning ? tr("Usually harmless once.") : tr("Serious: reboot soon.");
    } else if (kind == QLatin1String("d3cold")) {
        i.meaning = tr("A device (usually the laptop's dGPU) could not be woken from its deepest power-saving state. A hang often follows.");
        i.causes = {tr("Firmware (ACPI) bugs in runtime power management"), tr("A driver bug")};
        i.steps = {tr("Open dGPU power and review its runtime power management settings."),
                   tr("Update the BIOS/EC firmware and the GPU driver."),
                   tr("For a test, set NVreg_DynamicPowerManagement=0x00 (dGPU stays powered).")};
        i.urgency = tr("Serious: likely to freeze the system.");
    } else {
        i.meaning = tr("A kernel event Centurion does not have a specific explanation for.");
        i.steps = {tr("Review the raw log and check whether the event repeats before changing system settings.")};
    }
    return i;
}

QString html(const QJsonObject &e, const QString &headline) {
    const Info i = explain(e);
    auto list = [](const QStringList &items, const char *tag) {
        if (items.isEmpty()) return QString();
        QString s = QStringLiteral("<%1 style='margin-top:2px'>").arg(QLatin1String(tag));
        for (const QString &x : items) s += QStringLiteral("<li>%1</li>").arg(x.toHtmlEscaped());
        return s + QStringLiteral("</%1>").arg(QLatin1String(tag));
    };
    QString out = QStringLiteral("<h3 style='margin:0'>%1</h3>").arg(headline.toHtmlEscaped());
    if (!i.urgency.isEmpty()) out += QStringLiteral("<p><i>%1</i></p>").arg(i.urgency.toHtmlEscaped());
    out += QStringLiteral("<p>%1</p>").arg(i.meaning.toHtmlEscaped());
    if (!i.facts.isEmpty()) out += QStringLiteral("<b>%1</b>").arg(tr("From this record")) + list(i.facts, "ul");
    if (!i.causes.isEmpty()) out += QStringLiteral("<b>%1</b>").arg(tr("Usual causes")) + list(i.causes, "ul");
    if (!i.steps.isEmpty()) out += QStringLiteral("<b>%1</b>").arg(tr("What to do")) + list(i.steps, "ol");
    const QString raw = e.value(QStringLiteral("text")).toString();
    if (!raw.isEmpty())
        out += QStringLiteral("<b>%1</b><pre style='white-space:pre-wrap'>%2</pre>").arg(tr("Kernel record"), raw.toHtmlEscaped());
    return out;
}

QJsonObject classifyLine(const QString &m) {
    auto ev = [&m](const char *kind, const char *level, const QString &title, int code = 0) {
        QJsonObject o{{QStringLiteral("kind"), QLatin1String(kind)}, {QStringLiteral("level"), QLatin1String(level)},
                      {QStringLiteral("title"), title}, {QStringLiteral("text"), m}};
        if (code) o.insert(QStringLiteral("code"), code);
        return o;
    };
    auto has = [&m](const char *s) { return m.contains(QLatin1String(s)); };
    if (const QRegularExpressionMatch x = QRegularExpression(QStringLiteral(R"(NVRM: Xid \([^)]*\): (\d+))")).match(m); x.hasMatch())
        return ev("xid", "error", QStringLiteral("Xid %1").arg(x.captured(1)), x.captured(1).toInt());
    if (has("NVRM") && (has("heartbeat timed out") || has("GSP Timeout"))) return ev("gsp", "critical", QStringLiteral("GSP timeout"));
    if (has("[Hardware Error]") || has("Machine check events logged") || has("Machine Check Exception") || has("Machine Check:"))
        return ev("mce", has("Uncorrected") || has("|UE|") || has("Exception") ? "critical" : "warn", QStringLiteral("Machine check"));
    if (has("AER:") || has("PCIe Bus Error")) {
        if (has("Corrected error") || has("severity=Correct")) return ev("aer", "warn", QStringLiteral("PCIe corrected error"));
        if (has("Uncorrect") || has("Fatal") || has("severity=")) return ev("aer", "critical", QStringLiteral("PCIe uncorrectable error"));
        return {};   // the detail lines of a report ("device [8086:…] error status/mask")
    }
    if (has("soft lockup") || has("hard LOCKUP") || has("detected stalls") || has("self-detected stall"))
        return ev("lockup", "critical", QStringLiteral("CPU lockup / RCU stall"));
    if (has("Unable to change power state from D3cold to D0")) return ev("d3cold", "critical", QStringLiteral("D3cold"));
    if (has("amdgpu") && ((has("ring") && has("timeout")) || has("GPU reset begin"))) return ev("amdgpu", "error", QStringLiteral("AMD GPU hang"));
    if (m.startsWith(QLatin1String("nvme nvme")) && (has("timeout") || has("resetting") || has("controller is down")))
        return ev("nvme", "error", has("controller is down") ? QStringLiteral("NVMe controller is down") : QStringLiteral("NVMe timeout"));
    // Only the kernel's own error reports: "BTRFS info … scrub finished with 0 errors" is not one.
    if (has("EXT4-fs error") || m.startsWith(QLatin1String("BTRFS error")) || (m.startsWith(QLatin1String("BTRFS warning")) && has("csum failed"))
        || (m.startsWith(QLatin1String("XFS")) && (has("Corruption") || has("corruption detected"))))
        return ev("fs", "critical", QStringLiteral("File system error"));
    if ((has("i915") && has("GPU HANG")) || (m.startsWith(QLatin1String("xe ")) && (has("Timedout job") || has("trying reset"))))
        return ev("igpu", "error", QStringLiteral("Intel GPU hang"));
    if ((has("iwlwifi") && has("Microcode SW error")) || (has("mt79") && has("Message") && has("timeout"))
        || (has("firmware crashed") && (has("ath9k") || has("ath10k") || has("ath11k") || has("ath12k"))))
        return ev("wifi", "warn", QStringLiteral("Wi-Fi firmware error"));
    if (has("critical temperature reached")) return ev("thermal", "critical", QStringLiteral("Critical temperature"));
    if (has("Package temperature above threshold") || has("Core temperature above threshold"))
        return ev("thermal", "warn", QStringLiteral("Temperature above threshold"));
    if (has("Out of memory:") && has("Kill")) return ev("oom", "warn", QStringLiteral("Out of memory"));
    if (m.startsWith(QLatin1String("WARNING: CPU:"))) {
        const QString mod = cap(m, R"(\[([\w-]+)\]\s*$)");
        return ev("oops", "error", mod.isEmpty() ? QStringLiteral("Kernel warning") : QStringLiteral("Kernel warning in %1").arg(mod));
    }
    if (m.startsWith(QLatin1String("Oops:")) || m.startsWith(QLatin1String("BUG:"))) return ev("oops", "error", QStringLiteral("Kernel oops / bug"));
    return {};
}

}  // namespace eventinfo
