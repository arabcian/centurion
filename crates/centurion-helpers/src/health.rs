//! Health monitor: hardware/driver faults from the kernel log (/dev/kmsg)
//! plus the PCIe AER counters in sysfs.
//!
//! Reported: NVIDIA Xid (with what the code means; Xid 154 is only the
//! recovery action for the Xid before it), GSP timeouts, machine checks,
//! PCIe AER errors, soft/hard lockups and RCU stalls, amdgpu ring timeouts,
//! NVMe timeouts, file system errors, Intel iGPU hangs, Wi-Fi firmware
//! crashes, critical temperatures, OOM kills and kernel oops/BUG/WARNING.

use serde_json::{json, Value};
use std::io::Read;
use std::os::unix::fs::OpenOptionsExt;

const MAX_EVENTS: usize = 300;
const MAX_KLOG: usize = 4000;

/// (code, level, meaning). level: critical = the GPU needs a reset/reboot.
const XIDS: &[(u32, &str, &str)] = &[
    (8, "error", "GPU stopped processing (channel timeout)"),
    (13, "error", "Graphics engine exception (usually the application or an unstable clock)"),
    (31, "error", "GPU memory page fault (application, driver or unstable clock)"),
    (32, "error", "Invalid or corrupted push buffer stream"),
    (38, "error", "Driver firmware error"),
    (43, "warn", "GPU stopped processing a channel (application fault)"),
    (45, "warn", "Channel killed by the driver (follows another error or process exit)"),
    (48, "critical", "Double-bit ECC error"),
    (56, "error", "Display engine error"),
    (57, "error", "Error programming video memory interface"),
    (61, "critical", "Internal micro-controller breakpoint/warning (PMU)"),
    (62, "critical", "Internal micro-controller halt (PMU/GSP)"),
    (63, "critical", "ECC page retirement / row remapping event"),
    (64, "critical", "ECC page retirement / row remapper failure"),
    (68, "error", "Video processor exception"),
    (69, "error", "Graphics engine class error"),
    (74, "critical", "NVLink error"),
    (79, "critical", "GPU has fallen off the bus (power, PCIe link or firmware hang)"),
    (92, "critical", "High single-bit ECC error rate"),
    (94, "critical", "Contained ECC error"),
    (95, "critical", "Uncontained ECC error"),
    (109, "error", "Context switch timeout"),
    (119, "critical", "GSP RPC timeout (GPU firmware stopped answering)"),
    (120, "critical", "GSP firmware error"),
    (121, "critical", "C2C link error"),
    (140, "critical", "Unrecovered ECC error"),
    (143, "critical", "GPU initialization failure"),
    (154, "critical", "Recovery action required — the cause is the Xid logged just before"),
    (175, "critical", "GSP RPC timeout (extended)"),
];

pub fn xid_info(code: u32) -> (&'static str, &'static str) {
    XIDS.iter().find(|x| x.0 == code).map(|x| (x.1, x.2)).unwrap_or(("error", "NVIDIA driver error (see NVIDIA's Xid catalog)"))
}

/// Kernel virtual addresses (0xffff… / ffff… + 12 hex digits) replaced by "<addr>": this text goes to callers who
/// may not read the kernel log themselves (kernel.dmesg_restrict exists to keep exactly these away from them).
/// MCx_STATUS / MISC values (not in the kernel's address range) stay readable.
pub fn scrub(s: &str) -> String {
    let b = s.as_bytes();
    let (mut out, mut i) = (String::with_capacity(s.len()), 0);
    while i < b.len() {
        let st = if b[i..].starts_with(b"0x") { i + 2 } else { i };
        let boundary = i == 0 || !b[i - 1].is_ascii_hexdigit();
        let hex = b[st..].iter().take_while(|c| c.is_ascii_hexdigit()).count();
        if boundary && hex >= 16 && b[st..st + 4].eq_ignore_ascii_case(b"ffff") {
            out.push_str("<addr>");
            i = st + hex;
        } else {
            let ch = s[i..].chars().next().unwrap();
            out.push(ch);
            i += ch.len_utf8();
        }
    }
    out
}

/// Classifies one kernel log message; None = not interesting.
pub fn classify(msg: &str) -> Option<Value> {
    let clip = |s: &str| scrub(&s.chars().take(300).collect::<String>());
    if let Some(i) = msg.find("NVRM: Xid (") {
        let rest = &msg[i..];
        let after = rest.find("): ").map(|j| &rest[j + 3..])?;
        let code: u32 = after.split(|c: char| !c.is_ascii_digit()).next()?.parse().ok()?;
        let (level, meaning) = xid_info(code);
        let mut title = format!("Xid {code}: {meaning}");
        if code == 154 {
            if let Some(k) = after.find(" to ") { title = format!("Xid 154: recovery action → {}", clip(&after[k + 4..])); }
        }
        return Some(json!({"kind": "xid", "code": code, "level": level, "title": title, "text": clip(msg)}));
    }
    let has = |s: &str| msg.contains(s);
    let ev = |kind: &str, level: &str, title: &str| Some(json!({"kind": kind, "level": level, "title": title, "text": clip(msg)}));
    if has("NVRM") && (has("heartbeat timed out") || has("GSP Timeout") || has("_kgspIsHeartbeatTimedOut")) {
        return ev("gsp", "critical", "NVIDIA GSP firmware timeout");
    }
    if has("[Hardware Error]") || has("Machine check events logged") || has("Machine Check Exception") {
        // One machine check is printed as several records: the decoder's verdict line ("Corrected
        // error, …" / "Uncorrected, …"), the MCx_STATUS line (CE / UE flag) and detail lines (address,
        // IPID, syndrome, cache level, ext. error code; the kernel's own print adds TSC / PROCESSOR /
        // RIP). Only the verdict and status lines say what happened: every detail line of a corrected
        // error used to count as a critical machine check of its own (tray alerts, field stability).
        const DETAIL: &[&str] = &["Error Addr", "IPID:", "Syndrome:", "cache level", "Ext. Error Code", "TSC ", "PROCESSOR ", "RIP", "PPIN "];
        if !has("Machine Check") && !has("STATUS") && DETAIL.iter().any(|d| has(d)) { return None; }
        let corrected = has("Corrected") || has("events logged") || has("|CE|") || mce_status_corrected(msg);
        let level = if corrected && !has("Exception") { "warn" } else { "critical" };
        return ev("mce", level, "CPU machine check (unstable Curve Optimizer / RAM timings, or hardware)");
    }
    if has("AER:") || has("PCIe Bus Error") {
        if has("Corrected error") || has("severity=Correct") { return ev("aer", "warn", "PCIe corrected error"); }
        if has("Uncorrect") || has("Fatal") || has("severity=") { return ev("aer", "critical", "PCIe uncorrectable error"); }
        return None;
    }
    if has("soft lockup") || has("hard LOCKUP") || has("rcu_preempt detected stalls") || has("rcu_sched detected stalls")
        || has("self-detected stall") {
        return ev("lockup", "critical", "CPU lockup / RCU stall");
    }
    // The NVIDIA dGPU did not come back from D3cold (a hard hang follows on most machines).
    if has("Unable to change power state from D3cold to D0") {
        return ev("d3cold", "critical", "Device did not wake from D3cold (inaccessible)");
    }
    if has("amdgpu") && ((has("ring") && has("timeout")) || has("GPU reset begin")) {
        return ev("amdgpu", "error", "AMD iGPU hang / reset");
    }
    // Storage: NVMe controller trouble ("nvme nvme0: I/O 12 QID 3 timeout, aborting").
    if msg.starts_with("nvme nvme") {
        if has("controller is down") { return ev("nvme", "critical", "NVMe controller is down"); }
        if (has("I/O") && has("timeout")) || has("resetting controller") { return ev("nvme", "error", "NVMe I/O timeout / controller reset"); }
        return None;
    }
    // File systems: only the kernel's own error reports, not warnings or mount chatter.
    if has("EXT4-fs error") || msg.starts_with("BTRFS error") || (msg.starts_with("BTRFS warning") && has("csum failed"))
        || (msg.starts_with("XFS") && (has("Corruption") || has("corruption detected"))) {
        return ev("fs", "critical", "File system error (possible disk or data corruption)");
    }
    // Intel iGPU: i915 hang, xe job timeout / engine reset.
    if (has("i915") && has("GPU HANG")) || (msg.starts_with("xe ") && (has("Timedout job") || has("trying reset"))) {
        return ev("igpu", "error", "Intel iGPU hang / reset");
    }
    // Wi-Fi firmware crashes.
    if (has("iwlwifi") && has("Microcode SW error")) || (has("mt79") && has("Message") && has("timeout"))
        || (has("firmware crashed") && ["ath9k", "ath10k", "ath11k", "ath12k"].iter().any(|d| has(d))) {
        return ev("wifi", "warn", "Wi-Fi firmware error (the adapter resets itself)");
    }
    if has("critical temperature reached") { return ev("thermal", "critical", "Critical temperature reached (emergency shutdown)"); }
    if has("Package temperature above threshold") { return ev("thermal", "warn", "CPU package temperature above threshold (throttling)"); }
    if has("Out of memory:") && has("Kill") { return ev("oom", "warn", "Out of memory: a process was killed"); }
    // Kernel faults. Anchored at the start of the message: that is how the kernel prints them, and
    // "BUG:" / "Oops:" inside other text (driver messages) must not count.
    if msg.starts_with("Oops:") || msg.starts_with("BUG:") || msg.starts_with("WARNING: CPU:") {
        let what = if msg.starts_with("WARNING") { "Kernel warning" } else { "Kernel oops / bug" };
        let title = match module_tag(msg) { Some(m) => format!("{what} in {m}"), None => what.to_string() };
        return ev("oops", "error", &title);
    }
    None
}

/// The trailing "[module]" of a kernel message ("... foo+0x1/0x2 [amdgpu]"); "[#1]" and the like do not qualify.
fn module_tag(msg: &str) -> Option<&str> {
    let m = msg.trim_end().strip_suffix(']')?;
    let inner = &m[m.rfind('[')? + 1..];
    (!inner.is_empty() && inner.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'_' || c == b'-')).then_some(inner)
}

/// The kernel's generic MCE line ("CPU 0: Machine Check: 0 Bank 5: be00000000800400"): status
/// valid (bit 63) and not uncorrected (UC, bit 61).
fn mce_status_corrected(msg: &str) -> bool {
    let Some(i) = msg.find(" Bank ") else { return false };
    let Some(j) = msg[i..].find(": ") else { return false };
    let hex = msg[i + j + 2..].split_whitespace().next().unwrap_or("");
    u64::from_str_radix(hex.trim_start_matches("0x"), 16).is_ok_and(|s| s >> 63 & 1 == 1 && s >> 61 & 1 == 0)
}

/// Reads every /dev/kmsg record with seq > since. Err("EPERM") when the caller
/// may not read the kernel log (kernel.dmesg_restrict).
fn kmsg_events(since: u64) -> Result<(Vec<Value>, u64), String> {
    let mut f = std::fs::OpenOptions::new().read(true).custom_flags(libc::O_NONBLOCK | libc::O_CLOEXEC)
        .open("/dev/kmsg").map_err(|e| if e.raw_os_error() == Some(libc::EPERM) || e.raw_os_error() == Some(libc::EACCES) { "EPERM".to_string() } else { format!("/dev/kmsg: {e}") })?;
    let (mut out, mut last) = (Vec::new(), since);
    let mut buf = vec![0u8; 8192];
    loop {
        match f.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => {
                let rec = String::from_utf8_lossy(&buf[..n]);
                let Some((head, body)) = rec.split_once(';') else { continue };
                let mut h = head.split(',');
                let _pri = h.next();
                let Some(seq) = h.next().and_then(|s| s.parse::<u64>().ok()) else { continue };
                let ts: u64 = h.next().and_then(|s| s.parse().ok()).unwrap_or(0);
                last = last.max(seq);
                if seq <= since { continue; }
                let msg = body.lines().next().unwrap_or("");
                if let Some(mut e) = classify(msg) {
                    e["seq"] = json!(seq);
                    e["ts_us"] = json!(ts);
                    out.push(e);
                }
            }
            Err(e) if e.raw_os_error() == Some(libc::EPIPE) => continue,  // record overwritten meanwhile
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(format!("/dev/kmsg: {e}")),
        }
    }
    if out.len() > MAX_EVENTS { out.drain(..out.len() - MAX_EVENTS); }
    Ok((out, last))
}

/// Every kernel log record of level warning or worse (pri & 7 <= 4) with seq > since,
/// continuation lines (leading space) folded into their record. Returns the newest
/// MAX_KLOG records. Err("EPERM") when the kernel log is restricted.
fn klog_records(since: u64) -> Result<(Vec<Value>, u64), String> {
    let mut f = std::fs::OpenOptions::new().read(true).custom_flags(libc::O_NONBLOCK | libc::O_CLOEXEC)
        .open("/dev/kmsg").map_err(|e| if e.raw_os_error() == Some(libc::EPERM) || e.raw_os_error() == Some(libc::EACCES) { "EPERM".to_string() } else { format!("/dev/kmsg: {e}") })?;
    let (mut out, mut last) = (Vec::new(), since);
    let mut buf = vec![0u8; 8192];
    loop {
        match f.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => {
                let rec = String::from_utf8_lossy(&buf[..n]);
                let Some((head, body)) = rec.split_once(';') else { continue };
                let mut h = head.split(',');
                let Some(pri) = h.next().and_then(|s| s.parse::<u32>().ok()) else { continue };
                let Some(seq) = h.next().and_then(|s| s.parse::<u64>().ok()) else { continue };
                let ts: u64 = h.next().and_then(|s| s.parse().ok()).unwrap_or(0);
                last = last.max(seq);
                if seq <= since || pri & 7 > 4 { continue; }
                // Body: the message (may span several lines), then " KEY=value" dictionary
                // lines (leading space) that dmesg does not print either.
                let msg: String = body.lines().take_while(|l| !l.starts_with(' ')).collect::<Vec<_>>().join(" ⏎ ")
                    .chars().take(1000).collect();
                let msg = scrub(&msg);
                out.push(json!({"seq": seq, "ts_us": ts, "level": pri & 7, "msg": msg}));
            }
            Err(e) if e.raw_os_error() == Some(libc::EPIPE) => continue,
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(format!("/dev/kmsg: {e}")),
        }
    }
    if out.len() > MAX_KLOG { out.drain(..out.len() - MAX_KLOG); }
    Ok((out, last))
}

/// {"ok", "records", "last_seq", "boot_id"} or {"ok": false, "needs_root": true}.
pub fn klog(since: u64) -> Value {
    let boot_id = std::fs::read_to_string("/proc/sys/kernel/random/boot_id").unwrap_or_default().trim().to_owned();
    match klog_records(since) {
        Ok((records, last)) => json!({"ok": true, "records": records, "last_seq": last, "boot_id": boot_id}),
        Err(e) if e == "EPERM" => json!({"ok": false, "needs_root": true, "error": "the kernel log is restricted (kernel.dmesg_restrict=1)"}),
        Err(e) => json!({"ok": false, "error": e}),
    }
}

/// PCIe devices with a non-zero AER total (sysfs, readable by anyone).
fn aer_counters() -> Vec<Value> {
    let total = |p: std::path::PathBuf, key: &str| -> u64 {
        std::fs::read_to_string(p).ok().and_then(|s| s.lines().find_map(|l| l.strip_prefix(key)?.trim().parse().ok())).unwrap_or(0)
    };
    let mut v = Vec::new();
    for e in std::fs::read_dir("/sys/bus/pci/devices").into_iter().flatten().flatten() {
        let d = e.path();
        if !d.join("aer_dev_correctable").is_file() { continue; }
        let cor = total(d.join("aer_dev_correctable"), "TOTAL_ERR_COR");
        let nonfatal = total(d.join("aer_dev_nonfatal"), "TOTAL_ERR_NONFATAL");
        let fatal = total(d.join("aer_dev_fatal"), "TOTAL_ERR_FATAL");
        if cor + nonfatal + fatal == 0 { continue; }
        let class = std::fs::read_to_string(d.join("class")).unwrap_or_default();
        let vendor = std::fs::read_to_string(d.join("vendor")).unwrap_or_default();
        let what = match (vendor.trim(), class.trim().get(..6)) {
            ("0x10de", Some("0x0300")) | ("0x10de", Some("0x0302")) => "NVIDIA GPU",
            (_, Some("0x0604")) => "PCIe port",
            (_, Some("0x0108")) => "NVMe",
            (_, Some("0x0280")) => "Wi-Fi",
            _ => "device",
        };
        v.push(json!({"dev": e.file_name().to_string_lossy(), "what": what, "cor": cor, "nonfatal": nonfatal, "fatal": fatal}));
    }
    v
}

/// {"ok", "events", "last_seq", "boot_id", "aer"} or {"ok": false, "needs_root": true}.
pub fn scan(since: u64) -> Value {
    let boot_id = std::fs::read_to_string("/proc/sys/kernel/random/boot_id").unwrap_or_default().trim().to_owned();
    match kmsg_events(since) {
        Ok((events, last)) => json!({"ok": true, "events": events, "last_seq": last, "boot_id": boot_id, "aer": aer_counters()}),
        Err(e) if e == "EPERM" => json!({"ok": false, "needs_root": true, "error": "the kernel log is restricted (kernel.dmesg_restrict=1)"}),
        Err(e) => json!({"ok": false, "error": e}),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn scrub_kernel_addresses() {
        assert_eq!(scrub("RIP: 0010:foo+0x1/0x2 ffff8881234567ab and 0xffffffffc0123456 ok"), "RIP: 0010:foo+0x1/0x2 <addr> and <addr> ok");
        assert_eq!(scrub("Bank 5: be00000000800400 IPID 0x000000000000abcd"), "Bank 5: be00000000800400 IPID 0x000000000000abcd");
        assert_eq!(scrub("ünïcode ffff"), "ünïcode ffff");
    }
    #[test]
    fn xid_lines() {
        let e = classify("NVRM: Xid (PCI:0000:01:00): 79, pid='<unknown>', name=<unknown>, GPU has fallen off the bus.").unwrap();
        assert_eq!(e["code"], 79);
        assert_eq!(e["level"], "critical");
        let e = classify("NVRM: Xid (PCI:0000:01:00): 154, GPU recovery action changed from 0x0 (None) to 0x2 (Node Reboot Required)").unwrap();
        assert_eq!(e["code"], 154);
        assert!(e["title"].as_str().unwrap().contains("Node Reboot Required"));
        assert!(classify("NVRM: loading NVIDIA UNIX Open Kernel Module").is_none());
        assert_eq!(classify("pcieport 0000:00:01.1: AER: Corrected error message received from 0000:01:00.0").unwrap()["kind"], "aer");
        assert_eq!(classify("mce: [Hardware Error]: Machine check events logged").unwrap()["level"], "warn");
        // One corrected error, decoded over several records: one warning, no critical detail lines.
        assert_eq!(classify("[Hardware Error]: Corrected error, no action required.").unwrap()["level"], "warn");
        assert_eq!(classify("[Hardware Error]: CPU:2 (1a:44:0) MC1_STATUS[-|CE|MiscV|AddrV|-|-|SyndV|-|-|-]: 0x9c2040000000011b").unwrap()["level"], "warn");
        assert!(classify("[Hardware Error]: Error Addr: 0x00000000a1b2c3c0").is_none());
        assert!(classify("[Hardware Error]: IPID: 0x0001000100000000, Syndrome: 0x000000005a020001").is_none());
        assert!(classify("[Hardware Error]: Instruction Fetch Unit Ext. Error Code: 0, Microtag probe port parity error.").is_none());
        assert!(classify("[Hardware Error]: cache level: L1, tx: INSN").is_none());
        assert_eq!(classify("[Hardware Error]: Uncorrected, software restartable error.").unwrap()["level"], "critical");
        assert_eq!(classify("[Hardware Error]: CPU:0 (1a:44:0) MC5_STATUS[Over|UE|MiscV|AddrV|PCC|-|SyndV|-|-|-]: 0xfe20000000000135").unwrap()["level"], "critical");
        assert_eq!(classify("mce: [Hardware Error]: CPU 0: Machine Check: 0 Bank 7: 8c00004000010090").unwrap()["level"], "warn");
        assert_eq!(classify("mce: [Hardware Error]: CPU 0: Machine Check: 0 Bank 7: be00000000800400").unwrap()["level"], "critical");
        assert_eq!(classify("mce: [Hardware Error]: CPU 3: Machine Check Exception: 5 Bank 0: 8c00004000010090").unwrap()["level"], "critical");
        assert!(classify("mce: [Hardware Error]: TSC 0 ADDR 1ffffffe0 MISC d012000101000000 SYND 4d000000 IPID 1000b000000000").is_none());
        assert_eq!(classify("watchdog: BUG: soft lockup - CPU#3 stuck for 22s!").unwrap()["kind"], "lockup");
        assert_eq!(classify("nvidia 0000:01:00.0: Unable to change power state from D3cold to D0, device inaccessible").unwrap()["kind"], "d3cold");
    }
    fn kl(msg: &str) -> (String, String) {
        let e = classify(msg).unwrap_or_else(|| panic!("not classified: {msg}"));
        (e["kind"].as_str().unwrap().into(), e["level"].as_str().unwrap().into())
    }
    fn is(msg: &str, kind: &str, level: &str) { assert_eq!(kl(msg), (kind.to_string(), level.to_string()), "{msg}"); }
    #[test]
    fn nvme_lines() {
        is("nvme nvme0: I/O 123 (I/O Cmd) QID 5 timeout, aborting", "nvme", "error");
        is("nvme nvme0: I/O tag 12 (c00c) opcode 0x2 (I/O Cmd) QID 1 timeout, reset controller", "nvme", "error");
        is("nvme nvme0: resetting controller", "nvme", "error");
        is("nvme nvme0: controller is down; will reset: CSTS=0xffffffff, PCI_STATUS=0xffff", "nvme", "critical");
        assert!(classify("nvme nvme0: pci function 0000:01:00.0").is_none());
        assert!(classify("nvme nvme0: 8/0/0 default/read/poll queues").is_none());
        assert!(classify("nvme 0000:01:00.0: platform quirk: setting simple suspend").is_none());
        assert!(classify("nvme_core: module loaded, I/O timeout=30").is_none());
    }
    #[test]
    fn fs_lines() {
        is("EXT4-fs error (device nvme0n1p2): ext4_lookup:1855: inode #131073: comm ls: iget: bad extra_isize", "fs", "critical");
        is("BTRFS error (device nvme0n1p2): bdev /dev/nvme0n1p2 errs: wr 0, rd 0, flush 0, corrupt 3, gen 0", "fs", "critical");
        is("BTRFS warning (device dm-0): csum failed root 5 ino 257 off 0 csum 0x8941f998 expected csum 0x1d4c2f0b mirror 1", "fs", "critical");
        is("XFS (sda1): Corruption detected. Unmount and run xfs_repair", "fs", "critical");
        is("XFS (dm-0): Metadata corruption detected at xfs_buf_ioend+0x8f/0x180 [xfs], xfs_inode block 0x40", "fs", "critical");
        assert!(classify("EXT4-fs (sda1): mounted filesystem with ordered data mode. Quota mode: none.").is_none());
        assert!(classify("EXT4-fs warning (device sda1): ext4_dx_add_entry:2402: Directory index full!").is_none());
        assert!(classify("BTRFS info (device nvme0n1p2): using crc32c checksum algorithm").is_none());
        assert!(classify("BTRFS warning (device nvme0n1p2): space cache v1 is being deprecated").is_none());
        assert!(classify("XFS (sda1): Mounting V5 Filesystem").is_none());
    }
    #[test]
    fn igpu_wifi_lines() {
        is("i915 0000:00:02.0: [drm] GPU HANG: ecode 9:1:85dffffb, in firefox [2345]", "igpu", "error");
        is("xe 0000:00:02.0: [drm] GT0: Timedout job: seqno=4294967169, lrc_seqno=4294967169, guc_id=12, flags=0x0 in glxgears [1234]", "igpu", "error");
        is("xe 0000:00:02.0: [drm] GT0: trying reset from guc_exec_queue_timedout_job", "igpu", "error");
        assert!(classify("i915 0000:00:02.0: [drm] Finished loading DMC firmware i915/adlp_dmc.bin (v2.20)").is_none());
        assert!(classify("xe 0000:00:02.0: [drm] GT0: reset done").is_none());
        is("iwlwifi 0000:02:00.0: Microcode SW error detected. Restarting 0x0.", "wifi", "warn");
        is("mt7921e 0000:03:00.0: Message 00020007 (seq 3) timeout", "wifi", "warn");
        is("ath11k_pci 0000:03:00.0: firmware crashed: MHI_CB_EE_RDDM", "wifi", "warn");
        assert!(classify("iwlwifi 0000:02:00.0: loaded firmware version 77.2df0e0e8.0 ty-a0-gf-a0-77.ucode").is_none());
        assert!(classify("mt7921e 0000:03:00.0: ASIC revision: 79220010").is_none());
    }
    #[test]
    fn thermal_oom_oops_lines() {
        is("thermal thermal_zone0: critical temperature reached, shutting down", "thermal", "critical");
        is("CPU0: Package temperature above threshold, cpu clock throttled (total events = 1)", "thermal", "warn");
        assert!(classify("CPU0: Package temperature/speed normal").is_none());
        is("Out of memory: Killed process 4242 (firefox) total-vm:8123456kB, anon-rss:4012345kB, file-rss:0kB, shmem-rss:0kB, UID:1000 pgtables:9000kB oom_score_adj:0", "oom", "warn");
        assert!(classify("oom_reaper: reaped process 4242 (firefox), now anon-rss:0kB, file-rss:0kB, shmem-rss:0kB").is_none());
        is("Oops: 0000 [#1] PREEMPT SMP NOPTI", "oops", "error");
        is("BUG: kernel NULL pointer dereference, address: 0000000000000008", "oops", "error");
        let e = classify("WARNING: CPU: 3 PID: 1234 at drivers/gpu/drm/amd/amdgpu/amdgpu_dm.c:123 dm_update+0x1/0x2 [amdgpu]").unwrap();
        assert_eq!(e["kind"], "oops");
        assert_eq!(e["title"], "Kernel warning in amdgpu");
        assert_eq!(classify("Oops: 0000 [#1] PREEMPT SMP NOPTI").unwrap()["title"], "Kernel oops / bug");
        // Existing kinds win: a soft lockup is still a lockup, not a bug report.
        is("watchdog: BUG: soft lockup - CPU#3 stuck for 22s!", "lockup", "critical");
        assert!(classify("mydriver: BUG: not a kernel report").is_none());
        assert!(classify("usb 1-1: new high-speed USB device number 2 using xhci_hcd").is_none());
    }
    #[test]
    fn module_tags() {
        assert_eq!(module_tag("foo+0x1/0x2 [nvidia_drm]"), Some("nvidia_drm"));
        assert_eq!(module_tag("Oops: 0000 [#1] PREEMPT SMP"), None);
        assert_eq!(module_tag("trailing [#1]"), None);
        assert_eq!(module_tag("[]"), None);
        assert_eq!(module_tag("no brackets"), None);
    }
}
