//! sysadmin-helper / sysadmin-config-helper: the system changes Health → System tools and the Health pages offer.
//! The two binaries split the operations (see src/bin/sysadmin-helper.rs): CONFIG_OPS go through
//! sysadmin-config-helper (com.centurion.sysadmin.config, the password every time), the rest through
//! sysadmin-helper (com.centurion.sysadmin, the password then polkit's keep window).
//! Every operation has a fixed target:
//!   - one-shot actions (TRIM, drop caches, a SMART self-test, load / unload a module);
//!   - a fixed table of runtime sysfs knobs, each with its own value check (`sysfs_set`);
//!   - per-device power switches (rfkill, USB / PCI runtime PM) whose device name is shape-checked;
//!   - a fixed list of configuration files (`CONFS`), plus the existing *.conf files of /etc/modprobe.d named by
//!     file name only (`modprobe_d.<name>`): validated, backed up, written atomically.
//! Nothing in a request names a path or a program.

use serde_json::{json, Value};
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

pub const MAX_CONFIG: usize = 256 * 1024;

/// Operations served only by sysadmin-config-helper: each can make root run a command or change what boots
/// (modprobe install, udev RUN, fstab, a kernel module), so they never ride on a cached authorization.
pub const CONFIG_OPS: &[&str] = &["config_write", "config_append", "config_restore", "module", "nvreg_set"];
const BACKUP_ROOT: &str = "/var/lib/centurion/config-backups";
const KEEP_BACKUPS: usize = 10;
const BIN_DIRS: &[&str] = &["/usr/sbin", "/usr/bin", "/sbin", "/bin"];

#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Kind { Sysctl, Modprobe, ModulesLoad, Udev, Fstab, ShellVars }

pub struct Conf { pub id: &'static str, pub path: &'static str, pub kind: Kind, pub dropin: bool }

/// The only files this helper writes. Drop-ins are Centurion's own (removed when emptied);
/// the others are system files that are only replaced, never removed.
pub const CONFS: &[Conf] = &[
    Conf { id: "sysctl", path: "/etc/sysctl.d/99-centurion-user.conf", kind: Kind::Sysctl, dropin: true },
    Conf { id: "modprobe", path: "/etc/modprobe.d/centurion-user.conf", kind: Kind::Modprobe, dropin: true },
    Conf { id: "modules_load", path: "/etc/modules-load.d/centurion-user.conf", kind: Kind::ModulesLoad, dropin: true },
    Conf { id: "udev", path: "/etc/udev/rules.d/99-centurion-user.rules", kind: Kind::Udev, dropin: true },
    Conf { id: "sysctl_conf", path: "/etc/sysctl.conf", kind: Kind::Sysctl, dropin: false },
    Conf { id: "fstab", path: "/etc/fstab", kind: Kind::Fstab, dropin: false },
    // Gentoo / OpenRC: modules loaded at boot and their arguments (modules="…", module_<name>_args="…").
    Conf { id: "openrc_modules", path: "/etc/conf.d/modules", kind: Kind::ShellVars, dropin: false },
];

/// Prefix of the ids of other files in /etc/modprobe.d: "modprobe_d." + the file name.
pub const MODPROBE_D_PREFIX: &str = "modprobe_d.";

/// A file name in /etc/modprobe.d that may be edited: "<name>.conf", no path, not Centurion's own drop-in.
pub fn modprobe_d_name_ok(n: &str) -> bool {
    n.len() <= 69 && n.ends_with(".conf") && n.len() > 5 && n != "centurion-user.conf"
        && n.as_bytes()[0].is_ascii_alphanumeric()
        && n.bytes().all(|b| b.is_ascii_alphanumeric() || b"._-".contains(&b)) && !n.contains("..")
}

pub fn conf(id: &str) -> Option<&'static Conf> {
    if let Some(c) = CONFS.iter().find(|c| c.id == id) { return Some(c); }
    // Another existing regular file of /etc/modprobe.d. Only its name travels in the request; it is never
    // created or removed here. Leaked once: the helper answers one request and exits.
    let name = id.strip_prefix(MODPROBE_D_PREFIX).filter(|n| modprobe_d_name_ok(n))?;
    let path = format!("/etc/modprobe.d/{name}");
    if !std::fs::symlink_metadata(&path).is_ok_and(|m| m.is_file()) { return None; }
    Some(Box::leak(Box::new(Conf { id: Box::leak(id.to_owned().into_boxed_str()), path: Box::leak(path.into_boxed_str()),
                                   kind: Kind::Modprobe, dropin: false })))
}

/// One shell assignment that OpenRC's `source` cannot turn into a command: NAME="text" without $, `, \ or ";
/// NAME='text'; or NAME=word of plain characters. A trailing # comment is allowed.
fn shell_assignment_ok(line: &str) -> bool {
    let Some((name, rest)) = line.split_once('=') else { return false };
    let mut nb = name.bytes();
    if !nb.next().is_some_and(|b| b.is_ascii_alphabetic() || b == b'_') || !nb.all(|b| b.is_ascii_alphanumeric() || b == b'_') { return false; }
    let tail = |t: &str| { let t = t.trim(); t.is_empty() || t.starts_with('#') };
    match rest.as_bytes().first() {
        Some(b'"') => rest[1..].split_once('"').is_some_and(|(v, t)| !v.contains(['$', '`', '\\']) && tail(t)),
        Some(b'\'') => rest[1..].split_once('\'').is_some_and(|(_, t)| tail(t)),
        _ => {
            let (v, t) = rest.split_once(char::is_whitespace).unwrap_or((rest, ""));
            v.bytes().all(|b| b.is_ascii_alphanumeric() || b"_.,:/+=-".contains(&b)) && tail(t)
        }
    }
}

fn module_name_ok(s: &str) -> bool {
    !s.is_empty() && s.len() <= 64 && !s.starts_with('-') && s.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
}

/// Line-level check of a configuration file. A wrong line is reported with its number;
/// nothing is written then.
pub fn validate(kind: Kind, text: &str) -> Result<(), String> {
    if text.len() > MAX_CONFIG { return Err(format!("larger than {} KiB", MAX_CONFIG / 1024)); }
    if text.contains('\0') { return Err("contains a NUL byte".into()); }
    for (n, raw) in text.lines().enumerate() {
        let line = raw.trim();
        let bad = |why: &str| Err(format!("line {}: {why}: {}", n + 1, raw.chars().take(80).collect::<String>()));
        if line.len() > 4096 { return bad("longer than 4096 characters"); }
        if line.is_empty() || line.starts_with('#') || (kind == Kind::Sysctl && line.starts_with(';')) { continue; }
        match kind {
            Kind::Sysctl => {
                let Some((k, _)) = line.trim_start_matches('-').split_once('=') else { return bad("expected key = value") };
                let k = k.trim();
                if k.is_empty() || !k.bytes().all(|b| b.is_ascii_alphanumeric() || b"_.-/*".contains(&b)) { return bad("invalid key"); }
            }
            Kind::Modprobe => {
                let mut w = line.split_whitespace();
                let cmd = w.next().unwrap_or("");
                let arg = w.next();
                match cmd {
                    "options" | "blacklist" => if !arg.is_some_and(module_name_ok) { return bad("expected a module name") },
                    "alias" | "softdep" | "install" | "remove" => if arg.is_none() { return bad("missing argument") },
                    _ => return bad("unknown command (options, blacklist, alias, softdep, install, remove)"),
                }
            }
            Kind::ModulesLoad => if !module_name_ok(line) { return bad("expected one module name per line") },
            Kind::Fstab => if line.split_whitespace().count() < 4 { return bad("an fstab entry has at least 4 fields") },
            Kind::Udev => {}
            Kind::ShellVars => if !shell_assignment_ok(line) {
                return bad("expected NAME=\"value\" (no $, `, \\ or ; : OpenRC reads this file as a shell script)")
            },
        }
    }
    Ok(())
}

/// sysctl keys a configuration file written here may not set: they turn a file write into running a command
/// as root, or switch off a kernel hardening. `kernel.core_pattern` is refused only as a pipe ("|program").
const SYSCTL_DENY: &[&str] = &[
    "kernel.modprobe", "kernel.usermodehelper.", "kernel.kptr_restrict", "kernel.yama.ptrace_scope",
    "kernel.unprivileged_bpf_disabled", "kernel.perf_event_paranoid", "fs.protected_", "kernel.randomize_va_space",
    "kernel.modules_disabled", "kernel.dmesg_restrict", "kernel.kexec_load_disabled", "fs.suid_dumpable",
];

/// The modprobe `install` / `remove` commands accepted: only the "never load this module" idiom.
const MODPROBE_NOOP: &[&str] = &["/bin/true", "/bin/false", "/usr/bin/true", "/usr/bin/false"];

fn sysctl_key_denied(key: &str, value: &str) -> bool {
    let k = key.trim().trim_start_matches('-').replace('/', ".");
    if k == "kernel.core_pattern" { return value.trim_start().starts_with('|'); }
    let hit = |d: &str| if d.ends_with('.') || d.ends_with('_') { k.starts_with(d) } else { k == d };
    if let Some(prefix) = k.split('*').next().filter(|_| k.contains('*')) {
        // A glob: refused if it could match a denied key.
        return SYSCTL_DENY.iter().any(|d| d.starts_with(prefix) || prefix.starts_with(d)) || "kernel.core_pattern".starts_with(prefix);
    }
    SYSCTL_DENY.iter().any(|d| hit(d))
}

/// Lines of `new` that `old` does not already have, with their line numbers. Lines the file already had are the
/// system's own (an existing `install pcspkr /bin/false` stays editable around); only what a request adds is checked.
fn added_lines<'a>(old: &str, new: &'a str) -> Vec<(usize, &'a str)> {
    let before: std::collections::HashSet<&str> = old.lines().map(str::trim).collect();
    new.lines().enumerate().map(|(n, l)| (n + 1, l.trim())).filter(|(_, l)| !l.is_empty() && !l.starts_with('#') && !before.contains(l)).collect()
}

/// Refuses added lines that would make root run a command or lower a kernel hardening (see SYSCTL_DENY,
/// MODPROBE_NOOP; udev RUN / PROGRAM / IMPORT{program}). fstab is checked by fstabcheck instead.
pub fn new_lines_allowed(kind: Kind, old: &str, new: &str) -> Result<(), String> {
    for (n, line) in added_lines(old, new) {
        let bad = |why: &str| Err(format!("line {n}: {why}: {}", line.chars().take(80).collect::<String>()));
        match kind {
            Kind::Sysctl => if let Some((k, v)) = line.split_once('=') {
                if sysctl_key_denied(k, v) { return bad("this key cannot be set from Centurion (it runs a command as root or lowers a kernel protection)"); }
            },
            Kind::Modprobe => {
                let w: Vec<&str> = line.split_whitespace().collect();
                if matches!(w.first(), Some(&"install") | Some(&"remove")) && !(w.len() == 3 && MODPROBE_NOOP.contains(&w[2])) {
                    return bad("install / remove run a command as root; only \"install <module> /bin/false\" (never load it) is accepted here");
                }
            }
            Kind::Udev => {
                for part in line.split(',') {
                    let key = part.trim().split(|c: char| c == '=' || c == '+' || c == '-' || c == ':' || c == '!').next().unwrap_or("").trim();
                    if key.starts_with("RUN") || key.starts_with("PROGRAM") || key.starts_with("IMPORT{program}") {
                        return bad("RUN, PROGRAM and IMPORT{program} start a program as root and are not accepted here");
                    }
                }
            }
            Kind::ModulesLoad | Kind::Fstab | Kind::ShellVars => {}
        }
    }
    Ok(())
}

/// The file as it is now ("" when it does not exist yet), only to compare a request with.
fn current_text(c: &Conf) -> String { std::fs::read_to_string(c.path).unwrap_or_default() }

fn find_bin(name: &str) -> Option<PathBuf> {
    BIN_DIRS.iter().map(|d| Path::new(d).join(name)).find(|p| crate::trusted_path(p))
}

/// Runs a trusted system binary with a clean environment; (success, stdout+stderr), killed after `secs`.
fn run(name: &str, args: &[&str], secs: u64) -> (bool, String) {
    let Some(bin) = find_bin(name) else { return (false, format!("{name} is not installed")) };
    let child = Command::new(&bin).args(args).env_clear().env("PATH", "/usr/sbin:/usr/bin:/sbin:/bin").env("LC_ALL", "C")
        .current_dir("/").stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped()).spawn();
    let mut child = match child { Ok(c) => c, Err(e) => return (false, format!("{}: {e}", bin.display())) };
    let (mut so, mut se) = (child.stdout.take().unwrap(), child.stderr.take().unwrap());
    let out = std::thread::spawn(move || { let mut v = Vec::new(); let _ = (&mut so).take(1 << 20).read_to_end(&mut v); v });
    let err = std::thread::spawn(move || { let mut v = Vec::new(); let _ = (&mut se).take(64 << 10).read_to_end(&mut v); v });
    let deadline = Instant::now() + Duration::from_secs(secs);
    let status = loop {
        match child.try_wait() {
            Ok(Some(s)) => break Some(s),
            Ok(None) if Instant::now() < deadline => std::thread::sleep(Duration::from_millis(50)),
            _ => { crate::kill_reap(&mut child); break None; }
        }
    };
    let mut text = String::from_utf8_lossy(&out.join().unwrap_or_default()).into_owned();
    text.push_str(&String::from_utf8_lossy(&err.join().unwrap_or_default()));
    match status {
        Some(s) => (s.success(), text),
        None => (false, format!("{text}\n{name}: timed out after {secs} s")),
    }
}

fn write_sysfs(path: &str, value: &str) -> Result<(), String> {
    let real = crate::canonical_in_sysfs(Path::new(path)).ok_or_else(|| format!("{path} does not exist"))?;
    crate::sysfs_write(&real, value.as_bytes()).map_err(|e| format!("{path}: {e}"))
}

/// Writes procfs (only /proc/sys/vm/drop_caches here); no symlink follow.
fn write_proc(path: &str, value: &str) -> Result<(), String> {
    use std::io::Write;
    use std::os::unix::fs::OpenOptionsExt;
    let mut f = std::fs::OpenOptions::new().write(true).custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
        .open(path).map_err(|e| format!("{path}: {e}"))?;
    f.write_all(value.as_bytes()).map_err(|e| format!("{path}: {e}"))
}

fn ok(msg: impl Into<String>) -> Value { json!({"ok": true, "message": msg.into()}) }
fn fail(msg: impl Into<String>) -> Value { json!({"ok": false, "error": msg.into()}) }
fn from(r: Result<String, String>) -> Value { r.map_or_else(fail, ok) }

fn bracketed_choices(path: &str) -> Vec<String> {
    std::fs::read_to_string(path).unwrap_or_default().split_whitespace()
        .map(|s| s.trim_matches(|c| c == '[' || c == ']').to_owned()).collect()
}

/// The runtime knobs: (key, check value, apply). Lost at the next boot by design.
fn sysfs_set(key: &str, value: &str) -> Result<String, String> {
    let one_of = |allowed: &[&str]| if allowed.contains(&value) { Ok(()) } else { Err(format!("{key}: value must be one of {}", allowed.join(", "))) };
    match key {
        "cpu_boost" => {
            one_of(&["0", "1"])?;
            let global = "/sys/devices/system/cpu/cpufreq/boost";
            if Path::new(global).exists() { write_sysfs(global, value)?; return Ok(format!("CPU boost {}", if value == "1" { "on" } else { "off" })); }
            let mut n = 0;
            for p in policies() { if Path::new(&format!("{p}/boost")).exists() { write_sysfs(&format!("{p}/boost"), value)?; n += 1; } }
            if n == 0 { return Err("this cpufreq driver has no boost switch".into()); }
            Ok(format!("CPU boost {} on {n} policies", if value == "1" { "on" } else { "off" }))
        }
        "cpu_uncap" => {
            let mut n = 0;
            for p in policies() {
                let hw = crate::read_trimmed(Path::new(&format!("{p}/cpuinfo_max_freq"))).unwrap_or_default();
                if hw.parse::<u64>().is_ok() { write_sysfs(&format!("{p}/scaling_max_freq"), &hw)?; n += 1; }
            }
            Ok(format!("scaling_max_freq raised to the hardware maximum on {n} policies"))
        }
        "zswap" => { one_of(&["Y", "N"])?; write_sysfs("/sys/module/zswap/parameters/enabled", value)?; Ok(format!("zswap {}", if value == "Y" { "on" } else { "off" })) }
        "thp_enabled" => { one_of(&["always", "madvise", "never"])?; write_sysfs("/sys/kernel/mm/transparent_hugepage/enabled", value)?; Ok(format!("transparent huge pages: {value}")) }
        "thp_defrag" => { one_of(&["always", "defer", "defer+madvise", "madvise", "never"])?; write_sysfs("/sys/kernel/mm/transparent_hugepage/defrag", value)?; Ok(format!("huge page defrag: {value}")) }
        "hda_power_save" => {
            let s: u32 = value.parse().map_err(|_| "hda_power_save: seconds 0..3600".to_owned())?;
            if s > 3600 { return Err("hda_power_save: seconds 0..3600".into()); }
            write_sysfs("/sys/module/snd_hda_intel/parameters/power_save", value)?;
            Ok(format!("HDA power saving: {}", if s == 0 { "off".to_owned() } else { format!("after {s} s") }))
        }
        "mem_sleep" => {
            one_of(&["s2idle", "shallow", "deep"])?;
            if !bracketed_choices("/sys/power/mem_sleep").iter().any(|c| c == value) { return Err(format!("this machine does not offer {value}")); }
            write_sysfs("/sys/power/mem_sleep", value)?;
            Ok(format!("suspend mode: {value} (until the next boot)"))
        }
        "pcie_aspm" => {
            one_of(&["default", "performance", "powersave", "powersupersave"])?;
            write_sysfs("/sys/module/pcie_aspm/parameters/policy", value)?;
            Ok(format!("PCIe ASPM policy: {value}"))
        }
        _ => Err(format!("unknown setting {key}")),
    }
}

fn policies() -> Vec<String> {
    let mut v: Vec<String> = std::fs::read_dir("/sys/devices/system/cpu/cpufreq").into_iter().flatten().flatten()
        .filter_map(|e| e.file_name().to_str().filter(|n| n.starts_with("policy")).map(|n| format!("/sys/devices/system/cpu/cpufreq/{n}")))
        .collect();
    v.sort();
    v
}

pub fn usb_device_ok(d: &str) -> bool {
    if let Some(n) = d.strip_prefix("usb") { return !n.is_empty() && n.len() <= 3 && n.bytes().all(|b| b.is_ascii_digit()); }
    let Some((bus, port)) = d.split_once('-') else { return false };
    !bus.is_empty() && bus.len() <= 3 && bus.bytes().all(|b| b.is_ascii_digit()) && d.len() <= 32
        && port.split('.').all(|p| !p.is_empty() && p.len() <= 3 && p.bytes().all(|b| b.is_ascii_digit()))
}

pub fn pci_bdf_ok(d: &str) -> bool {
    let b = d.as_bytes();
    b.len() == 12 && b[4] == b':' && b[7] == b':' && b[10] == b'.' && (b'0'..=b'7').contains(&b[11])
        && [0..4, 5..7, 8..10].iter().all(|r| b[r.clone()].iter().all(|c| c.is_ascii_hexdigit() && !c.is_ascii_uppercase()))
}

/// "nvme0n1" / "nvme0" / "sda" → the device node smartctl takes (an NVMe controller for a namespace).
pub fn smart_node(d: &str) -> Option<String> {
    if let Some(rest) = d.strip_prefix("nvme") {
        let ctrl: String = rest.chars().take_while(char::is_ascii_digit).collect();
        let tail = &rest[ctrl.len()..];
        let ns_ok = tail.is_empty() || tail.strip_prefix('n').is_some_and(|n| !n.is_empty() && n.bytes().all(|b| b.is_ascii_digit()));
        return (!ctrl.is_empty() && ctrl.len() <= 3 && ns_ok).then(|| format!("/dev/nvme{ctrl}"));
    }
    let letters = d.strip_prefix("sd")?;
    (!letters.is_empty() && letters.len() <= 3 && letters.bytes().all(|b| b.is_ascii_lowercase())).then(|| format!("/dev/{d}"))
}

fn backup_dir(c: &Conf) -> String { format!("{BACKUP_ROOT}/{}", c.id) }

fn backups(c: &Conf) -> Vec<String> {
    let mut v: Vec<String> = std::fs::read_dir(backup_dir(c)).into_iter().flatten().flatten()
        .filter_map(|e| e.file_name().to_str().filter(|n| backup_name_ok(n)).map(str::to_owned)).collect();
    v.sort();  // the names are zero-padded milliseconds: sorted = oldest first
    v
}

pub fn backup_name_ok(n: &str) -> bool {
    n.len() == 17 && n.ends_with(".bak") && n[..13].bytes().all(|b| b.is_ascii_digit())
}

/// Copies the current file (if any) into the backup directory, keeping the newest KEEP_BACKUPS.
fn backup(c: &Conf) -> Result<Option<String>, String> {
    let meta = match std::fs::symlink_metadata(c.path) {
        Ok(m) => m,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(e) => return Err(format!("{}: {e}", c.path)),
    };
    if !meta.is_file() { return Err(format!("{} is not a regular file (a symlink?); refusing to replace it", c.path)); }
    if meta.len() as usize > MAX_CONFIG { return Err(format!("{} is larger than {} KiB", c.path, MAX_CONFIG / 1024)); }
    let body = std::fs::read(c.path).map_err(|e| format!("{}: {e}", c.path))?;
    for d in ["/var/lib/centurion", BACKUP_ROOT, &backup_dir(c)] { crate::secure_dir(d)?; }
    let ms = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis()).unwrap_or(0);
    let name = format!("{ms:013}.bak");
    crate::write_root_file(&format!("{}/{name}", backup_dir(c)), &body)?;
    let all = backups(c);
    for old in all.iter().take(all.len().saturating_sub(KEEP_BACKUPS)) { let _ = std::fs::remove_file(format!("{}/{old}", backup_dir(c))); }
    Ok(Some(name))
}

fn after_write(c: &Conf) -> String {
    match c.kind {
        Kind::Sysctl => {
            if !Path::new(c.path).exists() { return "removed; values already applied stay until the next boot".into(); }
            let (good, out) = run("sysctl", &["-p", c.path], 20);
            if good { format!("applied now:\n{}", out.trim()) } else { format!("saved, but applying it reported:\n{}", out.trim()) }
        }
        Kind::Udev => {
            let (good, out) = run("udevadm", &["control", "--reload"], 20);
            if good { "udev rules reloaded: they apply to devices added from now on (or after the next boot)".into() } else { format!("saved; udevadm control --reload: {}", out.trim()) }
        }
        Kind::Modprobe => "saved: takes effect the next time the module loads (a reboot for drivers in the initramfs, after regenerating it)".into(),
        Kind::ModulesLoad => "saved: the modules load at the next boot".into(),
        Kind::Fstab => "saved and verified with findmnt --verify; nothing was mounted or unmounted".into(),
        Kind::ShellVars => "saved: OpenRC's modules service loads them at the next boot (rc-service modules restart loads them now)".into(),
    }
}

fn config_write(c: &Conf, text: &str) -> Result<String, String> {
    validate(c.kind, text)?;
    if text.trim().is_empty() && !c.dropin { return Err(format!("refusing to empty {}", c.path)); }
    let parent = Path::new(c.path).parent().and_then(Path::to_str).unwrap_or("/etc");
    if c.dropin { crate::secure_dir(parent)?; }
    if c.kind == Kind::Fstab {
        crate::secure_dir("/var/lib/centurion")?;
        crate::secure_dir(BACKUP_ROOT)?;
        let cand = format!("{BACKUP_ROOT}/fstab.candidate");
        crate::write_root_file(&cand, text.as_bytes())?;
        let (good, out) = run("findmnt", &["--verify", "--tab-file", &cand], 20);
        let _ = std::fs::remove_file(&cand);
        if !good { return Err(format!("findmnt --verify rejects it:\n{}", out.trim())); }
    }
    let saved = backup(c)?;
    if text.trim().is_empty() {
        if saved.is_some() { std::fs::remove_file(c.path).map_err(|e| format!("{}: {e}", c.path))?; }
    } else {
        let mut body = text.to_owned();
        if !body.ends_with('\n') { body.push('\n'); }
        crate::write_root_file(c.path, body.as_bytes())?;
    }
    crate::wlog::log("config", &format!("{} written ({} bytes), backup {}", c.path, text.len(), saved.as_deref().unwrap_or("none")));
    Ok(after_write(c))
}

/// One line added to one of Centurion's drop-ins (a report's fix), unless an identical line is there.
fn config_append(c: &Conf, line: &str) -> Result<String, String> {
    if !c.dropin { return Err(format!("{} is only edited as a whole", c.path)); }
    let line = line.trim();
    if line.is_empty() || line.contains('\n') { return Err("one non-empty line expected".into()); }
    let mut text = match std::fs::read_to_string(c.path) {
        Ok(t) => t,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => String::new(),
        Err(e) => return Err(format!("{}: {e}", c.path)),
    };
    if text.lines().any(|l| l.trim() == line) { return Ok(format!("{} already has: {line}", c.path)); }
    if !text.is_empty() && !text.ends_with('\n') { text.push('\n'); }
    text.push_str(line);
    text.push('\n');
    config_write(c, &text).map(|m| format!("{line}\n→ {}\n{m}", c.path))
}

fn config_restore(c: &Conf, name: Option<&str>) -> Result<String, String> {
    let list = backups(c);
    let pick = match name {
        Some(n) if backup_name_ok(n) && list.iter().any(|b| b == n) => n.to_owned(),
        Some(_) => return Err("no such backup".into()),
        None => list.last().cloned().ok_or_else(|| format!("no backup of {} yet", c.path))?,
    };
    let body = crate::read_root_file(&format!("{}/{pick}", backup_dir(c)), MAX_CONFIG as u64).ok_or("the backup cannot be read")?;
    // Restoring is a write like any other: the current file is backed up first, so it can be undone.
    config_write(c, &body).map(|m| format!("restored the backup of {}\n{m}", pick.trim_end_matches(".bak")))
}

pub fn handle(req: &Value) -> Value {
    let s = |k: &str| req.get(k).and_then(Value::as_str).unwrap_or("");
    match s("op") {
        "fstrim" => { let (good, out) = run("fstrim", &["-av"], 900); if good { ok(out.trim()) } else { fail(out.trim()) } }
        "drop_caches" => {
            unsafe { libc::sync(); }
            from(write_proc("/proc/sys/vm/drop_caches", "3\n").map(|_| "page cache, dentries and inodes dropped".to_owned()))
        }
        "smart_test" => {
            let Some(node) = smart_node(s("device")) else { return fail("device must be like nvme0n1 or sda") };
            let kind = s("kind");
            if kind != "short" && kind != "long" { return fail("kind must be short or long"); }
            let (good, out) = run("smartctl", &["-t", kind, &node], 30);
            if good { ok(out.trim()) } else { fail(out.trim()) }
        }
        "module" => {
            let name = s("name");
            if !module_name_ok(name) { return fail("invalid module name"); }
            let Some(load) = req.get("load").and_then(Value::as_bool) else { return fail("load must be true or false") };
            let (good, out) = if load { run("modprobe", &[name], 60) } else { run("modprobe", &["-r", name], 60) };
            if good { ok(format!("{name} {}", if load { "loaded" } else { "unloaded" })) } else { fail(out.trim()) }
        }
        "rfkill" => {
            let (Some(i), Some(block)) = (req.get("index").and_then(Value::as_u64), req.get("block").and_then(Value::as_bool)) else {
                return fail("rfkill needs index (number) and block (bool)") };
            if i > 4096 { return fail("index out of range"); }
            from(write_sysfs(&format!("/sys/class/rfkill/rfkill{i}/soft"), if block { "1" } else { "0" })
                .map(|_| format!("radio {i} {}", if block { "blocked" } else { "unblocked" })))
        }
        "usb_power" | "pci_runtime_pm" => {
            let (dev, control) = (s("device"), s("control"));
            if control != "on" && control != "auto" { return fail("control must be on or auto"); }
            let path = if s("op") == "usb_power" {
                if !usb_device_ok(dev) { return fail("invalid USB device name"); }
                format!("/sys/bus/usb/devices/{dev}/power/control")
            } else {
                if !pci_bdf_ok(dev) { return fail("invalid PCI address"); }
                format!("/sys/bus/pci/devices/{dev}/power/control")
            };
            from(write_sysfs(&path, control).map(|_| format!("{dev}: power/control = {control}")))
        }
        "sysfs_set" => from(sysfs_set(s("key"), s("value"))),
        // NVIDIA modprobe options persist and change driver behaviour: password, never a silent grant.
        "nvreg_set" => match req.get("values").and_then(Value::as_object) {
            Some(v) => match crate::nvreg::set(v) {
                Ok(n) => json!({"ok": true, "written": n, "file": crate::nvreg::CENTURION_FILE}),
                Err(e) => fail(e),
            },
            None => fail("values must be an object"),
        },
        "config_write" => match (conf(s("id")), req.get("content").and_then(Value::as_str)) {
            (Some(c), Some(text)) => {
                let old = current_text(c);
                if let Err(e) = new_lines_allowed(c.kind, &old, text) { return fail(e); }
                // fstab: the added / changed lines are compared with the disks present now. A finding is
                // reported back; the same request with "confirm_fstab": true writes anyway.
                if c.kind == Kind::Fstab && req.get("confirm_fstab").and_then(Value::as_bool) != Some(true) {
                    let disks = crate::fstabcheck::Disks::scan(&crate::fstabcheck::changed_mount_points(&old, text));
                    let issues = crate::fstabcheck::check(&old, text, &disks);
                    if !issues.is_empty() {
                        return json!({"ok": false, "needs_confirm": true, "fstab_issues": issues,
                                      "error": format!("the fstab change does not match the disks on this machine:\n{}", issues.join("\n"))});
                    }
                }
                from(config_write(c, text))
            }
            _ => fail("config_write needs a known id and content"),
        },
        "config_append" => match conf(s("id")) {
            Some(c) => match new_lines_allowed(c.kind, "", s("line")) {
                Ok(()) => from(config_append(c, s("line"))),
                Err(e) => fail(e),
            },
            None => fail("unknown configuration id"),
        },
        "config_restore" => match conf(s("id")) {
            Some(c) => from(config_restore(c, req.get("backup").and_then(Value::as_str))),
            None => fail("unknown configuration id"),
        },
        "config_backups" => match conf(s("id")) {
            Some(c) => json!({"ok": true, "backups": backups(c)}),
            None => fail("unknown configuration id"),
        },
        other => fail(format!("unknown op {other}")),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn validators() {
        assert!(validate(Kind::Sysctl, "# c\nvm.swappiness = 100\n-kernel.foo=1\nnet/ipv4/ip_forward=0\n").is_ok());
        assert!(validate(Kind::Sysctl, "vm.swappiness 100").is_err());
        assert!(validate(Kind::Sysctl, "vm swappiness = 1").is_err());
        assert!(validate(Kind::Modprobe, "options nvidia_drm modeset=1\nblacklist nouveau\n").is_ok());
        assert!(validate(Kind::Modprobe, "frobnicate x").is_err());
        assert!(validate(Kind::Modprobe, "options").is_err());
        assert!(validate(Kind::ModulesLoad, "zenpower\n# x\n").is_ok());
        assert!(validate(Kind::ModulesLoad, "a b").is_err());
        assert!(validate(Kind::Fstab, "UUID=x / ext4 defaults 0 1").is_ok());
        assert!(validate(Kind::Fstab, "UUID=x /").is_err());
        assert!(validate(Kind::Udev, "a\0b").is_err());
        // /etc/conf.d/modules is sourced by OpenRC as root: plain assignments only.
        assert!(validate(Kind::ShellVars, "# c\nmodules=\"zenpower ryzen_smu\"\nmodule_nvidia_args='NVreg_X=1'\nmodules_6_12=\"a\" # c\nx=a,b\n").is_ok());
        for bad in ["modules=\"$(reboot)\"", "modules=\"`id`\"", "modules=a; reboot", "modules=\"a\"; reboot", "rm -rf /",
                    "1x=\"a\"", "modules=\"a", "m='a'b", "modules=a&&b", "modules=\"a\\\"\""] {
            assert!(validate(Kind::ShellVars, bad).is_err(), "{bad}");
        }
    }

    #[test]
    fn added_lines_that_run_commands_are_refused() {
        let ok = |k, o: &str, n: &str| new_lines_allowed(k, o, n).is_ok();
        assert!(!ok(Kind::Modprobe, "", "install nvidia /sbin/modprobe --ignore-install nvidia; /bin/sh -c id"));
        assert!(!ok(Kind::Modprobe, "", "remove nvidia /tmp/x"));
        assert!(ok(Kind::Modprobe, "", "install pcspkr /bin/false\noptions nvidia NVreg_X=1"));
        // A line the file already had is the system's own.
        assert!(ok(Kind::Modprobe, "install foo /opt/vendor/load-foo\n", "install foo /opt/vendor/load-foo\nblacklist bar\n"));
        assert!(!ok(Kind::Udev, "", "ACTION==\"add\", SUBSYSTEM==\"usb\", RUN+=\"/tmp/x\""));
        assert!(!ok(Kind::Udev, "", "KERNEL==\"sda\", PROGRAM=\"/bin/x\""));
        assert!(!ok(Kind::Udev, "", "KERNEL==\"sda\", IMPORT{program}=\"/bin/x\""));
        assert!(ok(Kind::Udev, "", "ACTION==\"add\", SUBSYSTEM==\"pci\", ATTR{power/control}=\"auto\", TAG+=\"uaccess\""));
        for bad in ["kernel.core_pattern = |/tmp/x %p", "kernel.modprobe=/tmp/x", "kernel/yama/ptrace_scope = 0",
                    "fs.protected_symlinks=0", "-kernel.kptr_restrict=0", "kernel.* = 0", "kernel.usermodehelper.bset = 1"] {
            assert!(!ok(Kind::Sysctl, "", bad), "{bad}");
        }
        for good in ["kernel.core_pattern = core.%p", "vm.swappiness = 10", "net.ipv4.tcp_congestion_control = bbr", "vm.* = 1"] {
            assert!(ok(Kind::Sysctl, "", good), "{good}");
        }
    }

    #[test]
    fn modprobe_d_names() {
        for good in ["nvidia.conf", "blacklist-nouveau.conf", "amd_pstate.conf", "99-local.conf"] { assert!(modprobe_d_name_ok(good), "{good}"); }
        for bad in ["../passwd.conf", "a/b.conf", ".conf", ".hidden.conf", "x.conf.bak", "centurion-user.conf", "x..conf", "", "a b.conf"] {
            assert!(!modprobe_d_name_ok(bad), "{bad}");
        }
        assert!(conf("modprobe_d.../../etc/passwd").is_none() && conf("modprobe_d.does-not-exist-centurion.conf").is_none());
        assert_eq!(conf("openrc_modules").map(|c| c.kind), Some(Kind::ShellVars));
    }

    #[test]
    fn device_shapes() {
        assert!(usb_device_ok("1-2") && usb_device_ok("3-1.4.2") && usb_device_ok("usb1"));
        assert!(!usb_device_ok("1-") && !usb_device_ok("../x") && !usb_device_ok("1-2/../../x") && !usb_device_ok("usb"));
        assert!(pci_bdf_ok("0000:01:00.0") && !pci_bdf_ok("0000:01:00.8") && !pci_bdf_ok("0000:01:00.0/x") && !pci_bdf_ok("0000:0A:00.0"));
        assert_eq!(smart_node("nvme0n1").as_deref(), Some("/dev/nvme0"));
        assert_eq!(smart_node("nvme1").as_deref(), Some("/dev/nvme1"));
        assert_eq!(smart_node("sdb").as_deref(), Some("/dev/sdb"));
        assert!(smart_node("nvme0n1p1").is_none() && smart_node("sda1").is_none() && smart_node("../sda").is_none());
        assert!(backup_name_ok("1732000000000.bak") && !backup_name_ok("../x.bak"));
    }

    #[test]
    fn requests_are_checked() {
        for r in [json!({"op": "sysfs_set", "key": "zswap", "value": "maybe"}), json!({"op": "sysfs_set", "key": "x", "value": "1"}),
                  json!({"op": "config_write", "id": "passwd", "content": "x"}),
                  json!({"op": "config_append", "id": "fstab", "line": "x"}), json!({"op": "config_append", "id": "modprobe", "line": "a\nb"}), json!({"op": "module", "name": "-r", "load": true}),
                  json!({"op": "usb_power", "device": "1-1", "control": "off"}), json!({"op": "nope"})] {
            assert_eq!(handle(&r)["ok"], false, "{r}");
        }
    }
}
