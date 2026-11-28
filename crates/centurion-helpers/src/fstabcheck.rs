//! Typo check for /etc/fstab edits (sysadmin-config-helper, config_write of "fstab").
//!
//! findmnt --verify catches syntax; this compares the lines a request adds or changes with the disks that are
//! on this machine right now, so a mistyped UUID or file system type is caught before the next boot drops to an
//! emergency shell. Every finding is a plain sentence; the request is refused until it is repeated with
//! "confirm_fstab": true (an entry for a disk that is not plugged in is legitimate, e.g. with nofail).
//!
//! Sources (read-only, no program is run): /dev/disk/by-{uuid,partuuid,label,partlabel}, /sys/class/block,
//! /dev/mapper, the udev database (/run/udev/data/b<major>:<minor>, ID_FS_TYPE) and /proc/filesystems.

use std::collections::{HashMap, HashSet};
use std::os::unix::fs::MetadataExt;
use std::path::Path;

/// What is on this machine. Built from the live system by `Disks::scan`; tests build it by hand.
#[derive(Default)]
pub struct Disks {
    /// "UUID" / "PARTUUID" / "LABEL" / "PARTLABEL" -> identifier -> file system type (if udev knows it).
    pub ids: HashMap<&'static str, HashMap<String, Option<String>>>,
    /// Device nodes ("/dev/nvme0n1p2", "/dev/mapper/root") -> file system type.
    pub nodes: HashMap<String, Option<String>>,
    /// File system types the kernel knows (/proc/filesystems).
    pub filesystems: HashSet<String>,
    /// Directories that exist (only the mount points the checked lines name are looked up).
    pub dirs: HashSet<String>,
}

const ID_DIRS: &[(&str, &str)] = &[("UUID", "/dev/disk/by-uuid"), ("PARTUUID", "/dev/disk/by-partuuid"),
                                   ("LABEL", "/dev/disk/by-label"), ("PARTLABEL", "/dev/disk/by-partlabel")];

/// Types that are not a kernel file system name but are valid in fstab.
const EXTRA_TYPES: &[&str] = &["swap", "auto", "none", "ntfs-3g", "nfs", "nfs4", "cifs", "smb3", "sshfs", "bind", "rbind", "ignore"];

/// udev escapes in /dev/disk/by-label names ("My\x20Disk").
fn udev_unescape(s: &str) -> String {
    let mut out = Vec::new();
    let b = s.as_bytes();
    let hex = |c: u8| (c as char).to_digit(16).map(|d| d as u8);
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'\\' && i + 3 < b.len() && b[i + 1] == b'x' {
            if let (Some(h), Some(l)) = (hex(b[i + 2]), hex(b[i + 3])) { out.push(h << 4 | l); i += 4; continue; }
        }
        out.push(b[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// fstab escapes spaces as \040.
fn fstab_unescape(s: &str) -> String { s.replace("\\040", " ").replace("\\011", "\t") }

/// ID_FS_TYPE of a block device node, from the udev database.
fn fs_type_of(node: &Path) -> Option<String> {
    let rdev = std::fs::metadata(node).ok()?.rdev();
    let major = ((rdev >> 8) & 0xfff) | ((rdev >> 32) & !0xfff);
    let minor = (rdev & 0xff) | ((rdev >> 12) & !0xff);
    let db = std::fs::read_to_string(format!("/run/udev/data/b{major}:{minor}")).ok()?;
    db.lines().find_map(|l| l.strip_prefix("E:ID_FS_TYPE=")).filter(|t| !t.is_empty()).map(str::to_owned)
}

impl Disks {
    pub fn scan(mount_points: &[String]) -> Disks {
        let mut d = Disks::default();
        for (key, dir) in ID_DIRS {
            let m = d.ids.entry(key).or_default();
            for e in std::fs::read_dir(dir).into_iter().flatten().flatten() {
                let Some(name) = e.file_name().to_str().map(udev_unescape) else { continue };
                m.insert(name, std::fs::canonicalize(e.path()).ok().and_then(|p| fs_type_of(&p)));
            }
        }
        for e in std::fs::read_dir("/sys/class/block").into_iter().flatten().flatten() {
            let Some(n) = e.file_name().to_str().map(str::to_owned) else { continue };
            let node = format!("/dev/{n}");
            let t = fs_type_of(Path::new(&node));
            d.nodes.insert(node, t);
        }
        for e in std::fs::read_dir("/dev/mapper").into_iter().flatten().flatten() {
            let p = e.path();
            let t = std::fs::canonicalize(&p).ok().and_then(|r| fs_type_of(&r));
            d.nodes.insert(p.to_string_lossy().into_owned(), t);
        }
        d.filesystems = std::fs::read_to_string("/proc/filesystems").unwrap_or_default().lines()
            .filter_map(|l| l.split_whitespace().last().map(str::to_owned)).collect();
        d.dirs = mount_points.iter().filter(|m| Path::new(m.as_str()).is_dir()).cloned().collect();
        d
    }
}

/// Edit distance (for "did you mean").
fn distance(a: &str, b: &str) -> usize {
    let (a, b): (Vec<char>, Vec<char>) = (a.chars().collect(), b.chars().collect());
    let mut prev: Vec<usize> = (0..=b.len()).collect();
    for i in 1..=a.len() {
        let mut cur = vec![i; b.len() + 1];
        for j in 1..=b.len() {
            cur[j] = (prev[j] + 1).min(cur[j - 1] + 1).min(prev[j - 1] + usize::from(a[i - 1] != b[j - 1]));
        }
        prev = cur;
    }
    prev[b.len()]
}

fn closest<'a>(want: &str, have: impl Iterator<Item = &'a String>) -> Option<&'a String> {
    let w = want.to_lowercase();
    have.map(|h| (distance(&w, &h.to_lowercase()), h)).filter(|(d, _)| *d > 0 && *d <= 3).min_by_key(|(d, _)| *d).map(|(_, h)| h)
}

/// The same file system for fstab purposes ("ntfs3" mounts what udev calls "ntfs", ext4 mounts ext2/3).
fn same_fs(fstab: &str, detected: &str) -> bool {
    let norm = |t: &str| match t { "ntfs3" | "ntfs-3g" => "ntfs".to_owned(), "msdos" | "fat" => "vfat".to_owned(), o => o.to_owned() };
    let (f, d) = (norm(fstab), norm(detected));
    f == d || (f == "ext4" && (d == "ext3" || d == "ext2")) || (f == "ext3" && d == "ext2")
}

/// The active (non-comment) lines of an fstab text, trimmed.
fn entries(text: &str) -> Vec<&str> {
    text.lines().map(str::trim).filter(|l| !l.is_empty() && !l.starts_with('#')).collect()
}

/// The mount points the lines added or changed by `new` (compared with `old`) name.
pub fn changed_mount_points(old: &str, new: &str) -> Vec<String> {
    let before: HashSet<&str> = entries(old).into_iter().collect();
    entries(new).into_iter().filter(|l| !before.contains(l))
        .filter_map(|l| l.split_whitespace().nth(1).map(fstab_unescape)).filter(|m| m.starts_with('/')).collect()
}

/// Findings for the lines `new` adds or changes compared with `old`. Empty = nothing to report.
pub fn check(old: &str, new: &str, disks: &Disks) -> Vec<String> {
    let before: HashSet<&str> = entries(old).into_iter().collect();
    let mut out = Vec::new();
    let mut seen_mounts: HashMap<String, usize> = HashMap::new();
    for (n, raw) in new.lines().enumerate() {
        let line = raw.trim();
        if line.is_empty() || line.starts_with('#') { continue; }
        let f: Vec<&str> = line.split_whitespace().collect();
        let no = n + 1;
        if let Some(mp) = f.get(1).filter(|m| m.starts_with('/')) {
            if let Some(first) = seen_mounts.insert(fstab_unescape(mp), no) {
                if !before.contains(line) { out.push(format!("line {no}: {mp} is already mounted by line {first}")); }
            }
        }
        if before.contains(line) { continue; }
        if f.len() < 4 || f.len() > 6 { out.push(format!("line {no}: an fstab entry has 4 to 6 fields, this one has {}", f.len())); continue; }
        let (spec, mp, fstype, opts) = (f[0], fstab_unescape(f[1]), f[2], f[3]);
        let optional = opts.split(',').any(|o| o == "nofail" || o == "noauto");
        // The device.
        let mut detected: Option<Option<String>> = None;  // Some(type) when the device was found
        if let Some((key, val)) = spec.split_once('=').filter(|(k, _)| ID_DIRS.iter().any(|(d, _)| d == k)) {
            let key = ID_DIRS.iter().find(|(d, _)| *d == key).map(|(d, _)| *d).unwrap_or("UUID");
            let val = fstab_unescape(val.trim_matches('"'));
            let empty = HashMap::new();
            let have = disks.ids.get(key).unwrap_or(&empty);
            let hit = have.get(&val).or_else(|| if key.ends_with("UUID") { have.iter().find(|(k, _)| k.eq_ignore_ascii_case(&val)).map(|(_, v)| v) } else { None });
            match hit {
                Some(t) => detected = Some(t.clone()),
                None if !optional => out.push(match closest(&val, have.keys()) {
                    Some(c) => format!("line {no}: no disk has {key}={val} — did you mean {key}={c}?"),
                    None => format!("line {no}: no disk on this machine has {key}={val} (add nofail if it is a removable disk)"),
                }),
                None => {}
            }
        } else if spec.starts_with("/dev/") {
            match disks.nodes.get(spec) {
                Some(t) => detected = Some(t.clone()),
                None if !optional => out.push(match closest(spec, disks.nodes.keys()) {
                    Some(c) => format!("line {no}: {spec} does not exist — did you mean {c}?"),
                    None => format!("line {no}: {spec} does not exist on this machine (add nofail if it is a removable disk)"),
                }),
                None => {}
            }
        }
        // The file system type.
        let known = fstype.starts_with("fuse.") || EXTRA_TYPES.contains(&fstype) || disks.filesystems.contains(fstype);
        if !known {
            let mut all: Vec<String> = disks.filesystems.iter().cloned().chain(EXTRA_TYPES.iter().map(|s| s.to_string())).collect();
            all.sort();
            out.push(match closest(fstype, all.iter()) {
                Some(c) => format!("line {no}: unknown file system type \"{fstype}\" — did you mean \"{c}\"?"),
                None => format!("line {no}: the kernel does not know the file system type \"{fstype}\" (module not loaded?)"),
            });
        } else if let Some(Some(t)) = &detected {
            if fstype != "auto" && fstype != "none" && !same_fs(fstype, t) {
                out.push(format!("line {no}: {spec} holds {t}, but the entry says {fstype}"));
            }
        }
        // The mount point.
        if fstype == "swap" {
            if mp != "none" && mp != "swap" { out.push(format!("line {no}: a swap entry's mount point is \"none\", not {mp}")); }
        } else if !mp.starts_with('/') {
            out.push(format!("line {no}: the mount point must be an absolute path, not {mp}"));
        } else if !disks.dirs.contains(&mp) && !opts.split(',').any(|o| o == "x-systemd.automount" || o.starts_with("x-mount.mkdir")) {
            out.push(format!("line {no}: the mount point {mp} does not exist (create the directory first)"));
        }
        // Options that matter for safety.
        for o in opts.split(',') {
            if o == "suid" || o == "dev" { out.push(format!("line {no}: option \"{o}\" set explicitly: set-user-ID programs / device files on this file system take effect")); }
        }
        // dump and pass.
        for (i, what) in [(4usize, "dump"), (5, "pass")] {
            if let Some(v) = f.get(i) { if v.parse::<u8>().is_err() { out.push(format!("line {no}: the {what} field must be a number, not {v}")); } }
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn disks() -> Disks {
        let mut d = Disks::default();
        d.ids.entry("UUID").or_default().insert("1234-ABCD".into(), Some("vfat".into()));
        d.ids.entry("UUID").or_default().insert("0a1b2c3d-1111-2222-3333-444455556666".into(), Some("ext4".into()));
        d.ids.entry("LABEL").or_default().insert("Data Disk".into(), Some("ntfs".into()));
        d.nodes.insert("/dev/nvme0n1p3".into(), Some("swap".into()));
        d.filesystems = ["ext4", "vfat", "btrfs", "ntfs3", "tmpfs", "proc"].iter().map(|s| s.to_string()).collect();
        d.dirs = ["/", "/boot", "/mnt/data"].iter().map(|s| s.to_string()).collect();
        d
    }

    const GOOD: &str = "UUID=0a1b2c3d-1111-2222-3333-444455556666 / ext4 defaults 0 1\nUUID=1234-ABCD /boot vfat umask=0077 0 2\n\
                        LABEL=Data\\040Disk /mnt/data ntfs3 defaults,nofail 0 0\n/dev/nvme0n1p3 none swap sw 0 0\ntmpfs /tmp tmpfs defaults 0 0\n";

    #[test]
    fn udev_names_keep_their_utf8() {
        assert_eq!(udev_unescape("M\u{fc}zik"), "M\u{fc}zik");
        assert_eq!(udev_unescape("My\\x20Disk"), "My Disk");
        assert_eq!(udev_unescape("a\\x5cb"), "a\\b");
        assert_eq!(udev_unescape("\\x1\u{e9}"), "\\x1\u{e9}", "no hex pair: kept, not a panic");
    }

    #[test]
    fn a_correct_file_has_no_findings() {
        let mut d = disks();
        d.dirs.insert("/tmp".into());
        assert_eq!(check("", GOOD, &d), Vec::<String>::new());
    }

    #[test]
    fn unchanged_lines_are_not_checked() {
        let old = "UUID=dead-beef /old ext4 defaults 0 2\n";
        assert!(check(old, old, &disks()).is_empty());
    }

    #[test]
    fn typos_are_found_with_a_suggestion() {
        let r = check("", "UUID=0a1b2c3d-1111-2222-3333-444455556667 / ext4 defaults 0 1\n", &disks());
        assert!(r.len() == 1 && r[0].contains("did you mean UUID=0a1b2c3d-1111-2222-3333-444455556666"), "{r:?}");
        let r = check("", "UUID=1234-ABCD /boot ext44 defaults 0 2\n", &disks());
        assert!(r.iter().any(|m| m.contains("did you mean \"ext4\"")), "{r:?}");
        let r = check("", "UUID=1234-ABCD /boot ext4 defaults 0 2\n", &disks());
        assert!(r.iter().any(|m| m.contains("holds vfat")), "{r:?}");
        let r = check("", "UUID=1234-ABCD /bot vfat defaults 0 2\n", &disks());
        assert!(r.iter().any(|m| m.contains("/bot does not exist")), "{r:?}");
        let r = check("", "/dev/nvme0n1p4 none swap sw 0 0\n", &disks());
        assert!(r.iter().any(|m| m.contains("did you mean /dev/nvme0n1p3")), "{r:?}");
        let r = check("", "UUID=1234-ABCD /boot vfat defaults x 2\n", &disks());
        assert!(r.iter().any(|m| m.contains("dump field")), "{r:?}");
        let r = check("", "UUID=1234-ABCD /boot vfat defaults,suid 0 2\n", &disks());
        assert!(r.iter().any(|m| m.contains("\"suid\"")), "{r:?}");
    }

    #[test]
    fn missing_disks_with_nofail_and_duplicates() {
        assert!(check("", "UUID=ffff-0000 /mnt/data ext4 defaults,nofail 0 2\n", &disks()).is_empty());
        let r = check("", "UUID=ffff-0000 /mnt/data ext4 defaults 0 2\n", &disks());
        assert!(r.iter().any(|m| m.contains("removable")), "{r:?}");
        let r = check("UUID=1234-ABCD /boot vfat defaults 0 2\n", "UUID=1234-ABCD /boot vfat defaults 0 2\nUUID=1234-ABCD /boot vfat umask=0077 0 2\n", &disks());
        assert!(r.iter().any(|m| m.contains("already mounted by line 1")), "{r:?}");
    }

    #[test]
    fn helpers() {
        assert_eq!(udev_unescape("My\\x20Disk"), "My Disk");
        assert_eq!(distance("ext44", "ext4"), 1);
        assert!(same_fs("ntfs3", "ntfs") && same_fs("ext4", "ext3") && !same_fs("ext4", "vfat"));
        assert_eq!(changed_mount_points("a /x ext4 d 0 0", "a /x ext4 d 0 0\nb /y ext4 d 0 0"), vec!["/y".to_string()]);
    }
}
