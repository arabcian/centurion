//! Read-only, on-demand Health Evidence Lab. No MSR/SMU/EC/PCI config writes,
//! no GPU wake-up queries, no background polling and no persistent root state.
//! Evidence is scoped to this boot and the bounded /dev/kmsg scan.
use serde_json::{json, Value};
use std::collections::{BTreeMap, BTreeSet};
use std::path::Path;

fn read(path: &Path) -> String {
    std::fs::read_to_string(path).unwrap_or_default().trim().to_owned()
}

fn cpu_topology(root: &Path) -> Vec<Value> {
    let base = root.join("sys/devices/system/cpu");
    let mut cpus = Vec::new();
    if let Ok(entries) = std::fs::read_dir(base) {
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().into_owned();
            let Some(cpu) = name.strip_prefix("cpu").and_then(|n| n.parse::<u32>().ok()) else { continue };
            let dir = entry.path();
            let package = read(&dir.join("topology/physical_package_id"));
            let core = read(&dir.join("topology/core_id"));
            let die = read(&dir.join("topology/die_id"));
            let siblings = read(&dir.join("topology/thread_siblings_list"));
            let mut l3_group = String::new();
            for i in 0..16 {
                let c = dir.join(format!("cache/index{i}"));
                if !c.is_dir() { continue; }
                if read(&c.join("level")) == "3" {
                    l3_group = read(&c.join("shared_cpu_list"));
                    break;
                }
            }
            // An L3-sharing domain can correspond to a CCD on some AMD CPUs,
            // but it is NOT itself proof of a firmware CCD index.
            cpus.push(json!({"logical_cpu":cpu, "package":package, "core_id":core,
                "die_id":die, "threads":siblings, "l3_shared_cpus":l3_group}));
        }
    }
    cpus.sort_by_key(|v| v["logical_cpu"].as_u64().unwrap_or(u64::MAX));
    cpus
}

/// File system types the running kernel can mount from a block device (/proc/filesystems without
/// "nodev"), plus ZFS, whose datasets are nodev but live on disks too.
fn disk_fs_types(filesystems: &str) -> BTreeSet<String> {
    let mut types: BTreeSet<String> = filesystems.lines()
        .filter(|l| !l.starts_with("nodev")).map(|l| l.trim().to_owned()).filter(|t| !t.is_empty()).collect();
    if filesystems.lines().any(|l| l.split_whitespace().last() == Some("zfs")) { types.insert("zfs".into()); }
    types
}

/// Every mounted disk file system (ext4, btrfs, xfs, f2fs, vfat, exfat, ntfs3, bcachefs, zfs, …) of
/// /proc/mounts. Pseudo and network file systems (proc, tmpfs, overlay, nfs) are not disk volumes and are left out.
fn fs_mounts(content: &str, types: &BTreeSet<String>) -> Vec<Value> {
    let mut mounts = Vec::new();
    for line in content.lines() {
        let fields: Vec<_> = line.split_whitespace().collect();
        if fields.len() >= 4 && types.contains(fields[2]) {
            // /proc/mounts escapes spaces and tabs in paths as octal (\040, \011).
            let unescape = |s: &str| s.replace("\\040", " ").replace("\\011", "\t").replace("\\134", "\\");
            mounts.push(json!({"source":unescape(fields[0]),"mount":unescape(fields[1]),"fstype":fields[2],"options":fields[3]}));
        }
        if mounts.len() >= 128 { break; }
    }
    mounts
}

/// The component a kernel log record belongs to, for the This Boot tabs that have no event kind of their own:
/// (component, kind, level, title). Records the other tabs read (Xid, machine checks, AER, file systems,
/// radios) are not repeated here. Matching is on the kernel's own wording; nothing is inferred.
fn component_of(msg: &str) -> Option<(&'static str, &'static str, &'static str, String)> {
    if let Some(e) = crate::health::classify(msg) {
        let title = e["title"].as_str().unwrap_or("").to_owned();
        let level = match e["level"].as_str() { Some("critical") => "critical", Some("warn") => "warn", _ => "error" };
        return match e["kind"].as_str().unwrap_or("") {
            "lockup" => Some(("kernel", "lockup", level, title)),
            "oops" => Some(("kernel", "oops", level, title)),
            "nvme" => Some(("storage", "nvme", level, title)),
            "amdgpu" => Some(("gpu", "amdgpu", level, title)),
            "igpu" => Some(("gpu", "igpu", level, title)),
            "thermal" => Some(("power", "thermal", level, title)),
            "d3cold" => Some(("power", "d3cold", level, title)),
            "oom" => Some(("memory", "oom", level, title)),
            _ => None,
        };
    }
    let has = |s: &str| msg.contains(s);
    let lower = msg.to_ascii_lowercase();
    let low = |s: &str| lower.contains(s);
    let c = |component, kind, level, title: &str| Some((component, kind, level, title.to_owned()));
    // Kernel
    if has("INFO: task ") && has("blocked for more than") { return c("kernel", "hung_task", "error", "Task blocked (hung task)"); }
    if msg.starts_with("kernel BUG at") { return c("kernel", "oops", "error", "Kernel BUG"); }
    if has("list_add corruption") || has("list_del corruption") { return c("kernel", "corruption", "critical", "Kernel list corruption"); }
    if msg.starts_with("UBSAN:") { return c("kernel", "ubsan", "warn", "Undefined behaviour in kernel code (UBSAN)"); }
    if has("general protection fault") { return c("kernel", "oops", "error", "General protection fault"); }
    // Storage: block layer, SATA, SCSI, RAID, device mapper, SD / eMMC
    if has("I/O error, dev ") || has("critical medium error, dev ") || has("critical target error, dev ") || has("timeout error, dev ") {
        return c("storage", "blk_io", "critical", "Block device I/O error");
    }
    if has("Buffer I/O error on dev") { return c("storage", "blk_io", "critical", "Buffer I/O error"); }
    if msg.starts_with("ata") && (has("exception Emask") || has("failed command") || has("COMRESET failed") || has("link is slow to respond")
        || has("SError:") || has("hardreset failed")) { return c("storage", "ata", "error", "SATA link / command error"); }
    if has("Sense Key : Medium Error") || has("Sense Key : Hardware Error") { return c("storage", "medium", "critical", "Disk reported a medium / hardware error"); }
    if msg.starts_with("md/raid") && (has("Disk failure") || has("not operational") || has("Operation continuing on")) {
        return c("storage", "raid", "critical", "RAID member failed (array degraded)");
    }
    if msg.starts_with("device-mapper:") && (low(" error") || low("failed")) { return c("storage", "dm", "error", "Device mapper error"); }
    if (msg.starts_with("mmc") || msg.starts_with("mmcblk")) && (low("error") || low("timeout")) { return c("storage", "mmc", "error", "SD / eMMC error"); }
    // GPU (AMD / Intel / nouveau; NVIDIA's proprietary driver has its own tab)
    if has("amdgpu") && (has("page fault") || has("VM_L2_PROTECTION_FAULT")) { return c("gpu", "gpu_fault", "error", "AMD GPU page fault"); }
    if has("i915") && has("Resetting") { return c("gpu", "igpu", "error", "Intel GPU engine reset"); }
    if msg.starts_with("nouveau") && (low("fault") || low("error")) { return c("gpu", "nouveau", "error", "nouveau GPU error"); }
    if has("[drm") && has("*ERROR*") { return c("gpu", "drm_error", "error", "DRM (display / GPU driver) error"); }
    // Power and thermal
    if has("Core temperature above threshold") { return c("power", "thermal", "warn", "CPU core temperature above threshold (throttling)"); }
    if msg.starts_with("ACPI BIOS Error") || msg.starts_with("ACPI Error") || msg.starts_with("ACPI Exception") {
        return c("power", "acpi", "warn", "ACPI firmware error");
    }
    if msg.starts_with("ACPI Warning") { return c("power", "acpi_warning", "warn", "ACPI firmware warning"); }
    if msg.starts_with("PM: ") && (low("failed") || low("error")) { return c("power", "pm", "error", "Suspend / resume step failed"); }
    // Memory
    if has("Memory cgroup out of memory") { return c("memory", "oom", "warn", "Out of memory in a cgroup: a process was killed"); }
    if has("page allocation failure") { return c("memory", "alloc", "warn", "Kernel memory allocation failure"); }
    if msg.starts_with("Memory failure:") || has("Memory failure:") { return c("memory", "hwpoison", "critical", "Hardware memory error (page taken offline)"); }
    if has("EDAC") && (has(" CE ") || has(" UE ") || has("Corrected") || has("Uncorrected")) {
        return c("memory", if has(" UE ") || has("Uncorrected") { "edac_ue" } else { "edac_ce" },
                 if has(" UE ") || has("Uncorrected") { "critical" } else { "warn" }, "ECC memory error (EDAC)");
    }
    // USB
    if msg.starts_with("usb ") || msg.starts_with("hub ") || low("xhci") || low("ucsi") {
        if has("over-current") { return c("usb", "overcurrent", "error", "USB port over-current"); }
        if has("device descriptor read") || has("device not accepting address") || has("unable to enumerate USB device")
            || has("can't set config") || has("Cannot enable") { return c("usb", "enumerate", "warn", "USB device failed to enumerate"); }
        if has("HC died") || has("not responding") || has("Host halt failed") || has("Abort failed") || has("host is dying") {
            return c("usb", "xhci", "critical", "USB host controller stopped responding");
        }
        if has("hub_ext_port_status failed") || has("hub_port_status failed") { return c("usb", "hub", "warn", "USB hub port status failed"); }
        if low("ucsi") && (low("error") || low("failed")) { return c("usb", "typec", "warn", "USB-C controller (UCSI) error"); }
    }
    // Audio
    if lower.starts_with("snd_") || lower.starts_with("snd-") || low("sof-audio") || lower.starts_with("sof") || lower.starts_with("hdaudio") {
        if low("azx_get_response timeout") || low("no response from codec") || (low("codec_read") && low("no response")) {
            return c("audio", "codec_timeout", "warn", "Audio codec did not respond");
        }
        if low("firmware") && (low("fail") || low("error")) { return c("audio", "firmware", "error", "Audio DSP firmware failure"); }
        if low("error") || low("failed") || low("timeout") { return c("audio", "error", "warn", "Audio driver error"); }
    }
    // Firmware files and drivers
    if has("Direct firmware load for") && has("failed") { return c("firmware", "fw_missing", "warn", "Firmware file not found (the driver may try another)"); }
    if low("failed to load firmware") || low("firmware: failed to load") || low("request_firmware failed") {
        return c("firmware", "fw_failed", "error", "Firmware failed to load");
    }
    if has("probe") && has("failed with error") { return c("firmware", "probe", "warn", "Driver failed to initialise a device"); }
    if has("Unknown symbol") || has("disagrees about version of symbol") { return c("firmware", "module", "error", "Kernel module does not match the kernel"); }
    None
}

/// The device a record names, as the kernel prints it: "usb 1-2", "snd_hda_intel 0000:00:1f.3", "ata1.00",
/// the disk after "dev ". Empty when the record names none.
fn component_device(msg: &str) -> String {
    if let Some(task) = msg.split_once("INFO: task ").and_then(|x| x.1.split_once(" blocked")) { return task.0.to_owned(); }
    if let Some(rest) = msg.split_once(" dev ").map(|x| x.1) {
        let d: String = rest.chars().take_while(|c| c.is_ascii_alphanumeric() || *c == '-' || *c == '_').collect();
        if !d.is_empty() { return d; }
    }
    // Words that open a record without naming a device.
    const NOT_DEVICES: &[&str] = &["INFO:", "PM:", "UBSAN:", "Memory", "Oops:", "BUG:", "WARNING:", "watchdog:", "rcu:", "ACPI", "kernel", "Out"];
    let tokens: Vec<&str> = msg.split_whitespace().take(2).collect();
    match tokens.as_slice() {
        [a, ..] if NOT_DEVICES.contains(a) => String::new(),
        [a, b, ..] if b.ends_with(':') && !a.ends_with(':') => format!("{a} {}", b.trim_end_matches(':')),
        [a, ..] if a.ends_with(':') && a.len() > 1 => a.trim_end_matches(':').to_owned(),
        _ => String::new(),
    }
}

/// Kernel log records (warning or worse, plus classified faults) grouped by component, newest 200 per component.
fn components(records: &[Value]) -> Value {
    let mut by: BTreeMap<&str, Vec<Value>> = BTreeMap::new();
    for r in records {
        let msg = r["msg"].as_str().unwrap_or("");
        let first = msg.split(" ⏎ ").next().unwrap_or(msg);
        let Some((component, kind, level, title)) = component_of(first) else { continue };
        by.entry(component).or_default().push(json!({"kind":kind,"level":level,"title":title,
            "dev":component_device(first),"text":first.chars().take(300).collect::<String>(),"ts_us":r["ts_us"],"seq":r["seq"]}));
    }
    let mut out = serde_json::Map::new();
    for c in ["kernel", "storage", "gpu", "power", "memory", "usb", "audio", "firmware"] {
        let mut v = by.remove(c).unwrap_or_default();
        if v.len() > 200 { v.drain(..v.len() - 200); }
        out.insert(c.into(), Value::Array(v));
    }
    Value::Object(out)
}

/// Time window is 5 minutes, within the same boot. Cases are correlation only;
/// Xid 154 is a recovery directive and cannot identify the triggering failure.
fn xid_cases(events: &[Value]) -> Vec<Value> {
    let mut cases = Vec::new();
    for (i, event) in events.iter().enumerate() {
        if event["kind"] != "xid" || event["code"] != 154 { continue; }
        let ts = event["ts_us"].as_u64().unwrap_or(0);
        let pci = event["pci"].as_str().unwrap_or("");
        let mut evidence = Vec::new();
        for prev in events[..i].iter().rev() {
            let pts = prev["ts_us"].as_u64().unwrap_or(0);
            if pts > ts || ts - pts > 300_000_000 { continue; }
            let kind = prev["kind"].as_str().unwrap_or("");
            if !["xid", "gsp", "aer", "d3cold"].contains(&kind) ||
                (kind == "xid" && prev["code"] == 154) { continue; }
            let other_pci = prev["pci"].as_str().unwrap_or("");
            if kind == "xid" && !pci.is_empty() && !other_pci.is_empty() && pci != other_pci { continue; }
            evidence.push(json!({"ts_us":pts,"kind":kind,"code":prev["code"],
                "title":prev["title"],"pci":other_pci,
                "scope": if kind == "xid" && !pci.is_empty() && pci == other_pci {"same GPU"} else {"device unverified"}}));
            if evidence.len() == 16 { break; }
        }
        evidence.reverse();
        cases.push(json!({"ts_us":ts,"pci":pci,"xid":154,"preceding":evidence,
            "assessment": if evidence.is_empty() {"No earlier related event was captured; cause unknown"}
                          else {"Temporal correlation only; no root cause established"}}));
    }
    cases
}

fn build(events: &[Value], topology: &[Value], aer: &[Value], mounts: &[Value]) -> Value {
    let by_cpu: BTreeMap<u64, &Value> = topology.iter().filter_map(|v| Some((v["logical_cpu"].as_u64()?, v))).collect();
    let mut mce = Vec::new();
    let mut fs = Vec::new();    // F2FS and every other file system's integrity errors (File Systems tab)
    let mut radio = Vec::new();
    let mut radio_groups: BTreeMap<String, usize> = BTreeMap::new();
    let mut xid = Vec::new();   // every NVIDIA Xid of the scan window, for the per-code view (H04, H16)
    let mut seen = BTreeSet::new();
    for event in events {
        match event["kind"].as_str().unwrap_or("") {
            "mce" => {
                let cpu = event["cpu"].as_u64();
                let mapped = cpu.and_then(|id| by_cpu.get(&id));
                mce.push(json!({"ts_us":event["ts_us"],"text":event["text"],
                    "severity":event["level"],"cpu":event["cpu"],"bank":event["bank"],
                    "apic_id":event["apic_id"],"topology":mapped,
                    "confidence":if mapped.is_some() {"logical CPU mapped to current topology; CCD not verified"}
                        else {"Unknown (missing or unmapped CPU identity)"}}));
            }
            "f2fs" | "fs" => fs.push(event.clone()),
            "xid" | "gsp" if xid.len() < 200 => xid.push(json!({"ts_us":event["ts_us"],"kind":event["kind"],"code":event["code"],
                "pci":event["pci"],"level":event["level"],"title":event["title"],"text":event["text"]})),
            "wifi" | "bluetooth" => {
                let kind = event["kind"].as_str().unwrap_or("unknown");
                let text = event["text"].as_str().unwrap_or("");
                let tokens: Vec<&str> = text.split_whitespace().collect();
                let device = if kind == "bluetooth" {
                    tokens.iter().find(|word| word.starts_with("hci") && word.ends_with(':'))
                        .map(|word| word.trim_end_matches(':').to_string())
                        .unwrap_or_else(|| "unknown controller".to_string())
                } else {
                    let driver = tokens.first().copied().unwrap_or("unknown driver");
                    let pci = tokens.iter().find(|word| word.starts_with("0000:") && word.contains('.'));
                    format!("{driver} {}", pci.map_or("unknown PCI device", |v| v.trim_end_matches(':')))
                };
                *radio_groups.entry(format!("{kind}: {device}")).or_default() += 1;
                radio.push(event.clone());
            },
            _ => {}
        }
    }
    for item in aer {
        if let Some(dev) = item["dev"].as_str() { seen.insert(dev.to_string()); }
    }
    let radio_summary: Vec<Value> = radio_groups.into_iter().map(|(device, errors)| json!({"device":device,"errors":errors})).collect();
    json!({"mce":mce,"topology":topology,"aer":aer,"xid_cases":xid_cases(events),"xid":xid,
        "fs_events":fs,"fs_mounts":mounts,"radio_events":radio,"radio_summary":radio_summary,
        "aer_devices":seen.len(), "event_count":events.len(),
        "limitations":"Latest 300 classified events from current boot; absence of recorded events does not prove health. No CCD index inferred from L3 topology."})
}

pub fn report() -> Value {
    let scan = crate::health::scan(0);
    if scan["ok"] != true { return scan; }
    let events = scan["events"].as_array().cloned().unwrap_or_default();
    let aer = scan["aer"].as_array().cloned().unwrap_or_default();
    let root = Path::new("/");
    let types = disk_fs_types(&read(&root.join("proc/filesystems")));
    let mut result = build(&events, &cpu_topology(root), &aer, &fs_mounts(&read(&root.join("proc/mounts")), &types));
    result["fs_supported"] = json!(types);
    let klog = crate::health::klog(0);
    result["components"] = components(klog["records"].as_array().map_or(&[][..], |v| v.as_slice()));
    if klog["ok"] != true { result["components_error"] = klog["error"].clone(); }
    result["ok"] = json!(true);
    result["boot_id"] = scan["boot_id"].clone();
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn cpu_identifiers_do_not_infer_ccd() {
        let events = vec![json!({"kind":"mce","cpu":9,"bank":5,"ts_us":100,"level":"warn","text":"MCE"}),
                          json!({"kind":"mce","apic_id":5,"ts_us":101,"text":"MCE"})];
        let topo = vec![json!({"logical_cpu":9,"core_id":"1","l3_shared_cpus":"8-15"})];
        let r = build(&events, &topo, &[], &[]);
        assert_eq!(r["mce"][0]["topology"]["core_id"], "1");
        assert!(r["mce"][0]["confidence"].as_str().unwrap().contains("not verified"));
        assert!(r["mce"][1]["topology"].is_null());
    }
    #[test]
    fn xid_154_only_preceding_same_gpu() {
        let events = vec![json!({"kind":"xid","code":79,"pci":"0000:01:00","ts_us":2_000_000,"title":"Xid 79"}),
            json!({"kind":"xid","code":31,"pci":"0000:02:00","ts_us":3_000_000,"title":"Xid 31"}),
            json!({"kind":"xid","code":154,"pci":"0000:01:00","ts_us":4_000_000})];
        let c = xid_cases(&events);
        assert_eq!(c[0]["preceding"].as_array().unwrap().len(), 1);
        assert_eq!(c[0]["preceding"][0]["code"], 79);
        assert!(c[0]["assessment"].as_str().unwrap().contains("correlation"));
        let r = build(&events, &[], &[], &[]);
        assert_eq!(r["xid"].as_array().unwrap().len(), 3);
        assert_eq!(r["xid"][0]["code"], 79);
    }
    #[test]
    fn radio_ledger_groups_device_without_inventing_hardware_ids() {
        let events = vec![
            json!({"kind":"wifi","text":"mt7925e 0000:03:00.0: firmware timeout"}),
            json!({"kind":"wifi","text":"mt7925e 0000:03:00.0: firmware timeout"}),
            json!({"kind":"bluetooth","text":"Bluetooth: hci0: command tx timeout"})];
        let result = build(&events, &[], &[], &[]);
        assert_eq!(result["radio_summary"][0]["device"], "bluetooth: hci0");
        assert_eq!(result["radio_summary"][1]["errors"], 2);
    }
    #[test]
    fn components_group_kernel_records_without_repeating_other_tabs() {
        let rec = |msg: &str| json!({"msg":msg,"ts_us":1,"seq":1,"level":3});
        let records = vec![
            rec("INFO: task kworker/u16:3:123 blocked for more than 122 seconds."),
            rec("watchdog: BUG: soft lockup - CPU#3 stuck for 22s!"),
            rec("I/O error, dev sda, sector 2048 op 0x0:(READ) flags 0x0 phys_seg 1 prio class 0"),
            rec("ata1.00: exception Emask 0x0 SAct 0x0 SErr 0x0 action 0x6 frozen"),
            rec("nvme nvme0: I/O 123 (I/O Cmd) QID 5 timeout, aborting"),
            rec("md/raid1:md0: Disk failure on sdb1, disabling device."),
            rec("amdgpu 0000:03:00.0: amdgpu: [gfxhub] page fault (src_id:0 ring:24 vmid:3 pasid:32769)"),
            rec("CPU3: Core temperature above threshold, cpu clock throttled (total events = 5)"),
            rec("ACPI BIOS Error (bug): Could not resolve symbol [\\_SB.PCI0.GPP0.SWUS], AE_NOT_FOUND (20240322/dswload2-162)"),
            rec("EDAC MC0: 1 CE on mc#0csrow#0channel#1 (csrow:0 channel:1 page:0x1 offset:0x0 grain:64 syndrome:0x0)"),
            rec("Memory cgroup out of memory: Killed process 4242 (chrome) total-vm:1kB"),
            rec("usb 1-2: device descriptor read/64, error -71"),
            rec("usb usb1-port3: over-current condition"),
            rec("xhci_hcd 0000:00:14.0: xHCI host controller not responding, assume dead"),
            rec("snd_hda_intel 0000:00:1f.3: azx_get_response timeout, switching to polling mode: last cmd=0x20270503"),
            rec("iwlwifi 0000:00:14.3: Direct firmware load for iwlwifi-so-a0-gf-a0-89.ucode failed with error -2"),
            rec("i2c_hid_acpi: probe of i2c-ELAN0001:00 failed with error -110"),
            // Read by their own tabs: not repeated.
            rec("NVRM: Xid (PCI:0000:01:00): 79, pid='<unknown>', name=<unknown>, GPU has fallen off the bus."),
            rec("EXT4-fs error (device sda1): ext4_lookup:1855: inode #2: comm ls: bad extra_isize"),
            rec("mce: [Hardware Error]: Machine check events logged"),
            // Ordinary chatter: not a fault of any component.
            rec("usb 1-2: new high-speed USB device number 3 using xhci_hcd"),
            rec("ACPI: \\_SB_.PLTF.C000: Found 2 idle states"),
        ];
        let c = components(&records);
        let kinds = |comp: &str| c[comp].as_array().unwrap().iter().map(|e| e["kind"].as_str().unwrap().to_owned()).collect::<Vec<_>>();
        assert_eq!(kinds("kernel"), ["hung_task", "lockup"]);
        assert_eq!(c["kernel"][0]["dev"], "kworker/u16:3:123");
        assert_eq!(kinds("storage"), ["blk_io", "ata", "nvme", "raid"]);
        assert_eq!(c["storage"][0]["dev"], "sda");
        assert_eq!(c["storage"][1]["dev"], "ata1.00");
        assert_eq!(kinds("gpu"), ["gpu_fault"]);
        assert_eq!(kinds("power"), ["thermal", "acpi"]);
        assert_eq!(c["power"][1]["dev"], "");
        assert_eq!(kinds("memory"), ["edac_ce", "oom"]);
        assert_eq!(c["memory"][0]["dev"], "EDAC MC0");
        assert_eq!(kinds("usb"), ["enumerate", "overcurrent", "xhci"]);
        assert_eq!(c["usb"][0]["dev"], "usb 1-2");
        assert_eq!(kinds("audio"), ["codec_timeout"]);
        assert_eq!(c["audio"][0]["dev"], "snd_hda_intel 0000:00:1f.3");
        assert_eq!(kinds("firmware"), ["fw_missing", "probe"]);
    }
    #[test]
    fn every_disk_file_system_is_a_volume() {
        let types = disk_fs_types("nodev\tsysfs\nnodev\ttmpfs\n\text4\n\tbtrfs\n\tvfat\n\tf2fs\nnodev\tzfs\n");
        assert!(types.contains("ext4") && types.contains("zfs") && !types.contains("tmpfs"));
        let m = fs_mounts("/dev/nvme0n1p2 / btrfs rw,subvol=/@ 0 0\n/dev/nvme0n1p2 /home btrfs rw,subvol=/@home 0 0\n\
            tmpfs /tmp tmpfs rw 0 0\n/dev/nvme0n1p1 /boot/efi vfat rw 0 0\n/dev/sda1 /mnt/My\\040Disk ext4 rw 0 0\nrpool/data /data zfs rw 0 0\n", &types);
        let got: Vec<_> = m.iter().map(|v| (v["fstype"].as_str().unwrap(), v["mount"].as_str().unwrap())).collect();
        assert_eq!(got, [("btrfs", "/"), ("btrfs", "/home"), ("vfat", "/boot/efi"), ("ext4", "/mnt/My Disk"), ("zfs", "/data")]);
        let r = build(&[json!({"kind":"fs","text":"EXT4-fs error (device sda1): x"}), json!({"kind":"f2fs","text":"F2FS-fs (dm-0): corrupt"})], &[], &[], &m);
        assert_eq!(r["fs_events"].as_array().unwrap().len(), 2);
        assert_eq!(r["fs_mounts"].as_array().unwrap().len(), 5);
    }
    #[test]
    fn silent_data_stays_unknown() {
        let r = build(&[], &[], &[], &[]);
        assert_eq!(r["mce"].as_array().unwrap().len(), 0);
        assert!(r["limitations"].as_str().unwrap().contains("does not prove"));
    }
}
