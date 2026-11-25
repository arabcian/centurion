//! Which distribution this is, and what that means for a backup: the system image's default exclude
//! list (package caches, re-downloadable repository data, snapshot stores) and the files the
//! configuration backup's "system profile" keeps (package manager configuration, initramfs, the list
//! of installed packages).
//!
//! The GUI keeps a copy of the exclude lists (gui/src/distroprofile.cpp); a test below checks that
//! every pattern here is spelled the same there.

use std::fs;
use std::path::Path;

/// "gentoo", "arch", "debian", "fedora", "opensuse", "void", "alpine", "nixos", or "" for anything else.
/// Derivatives are found through ID_LIKE (Manjaro, EndeavourOS, CachyOS → arch; Ubuntu, Mint,
/// Pop!_OS → debian; RHEL, Rocky, Alma, CentOS → fedora; Leap, Tumbleweed, SLES → opensuse).
/// ID wins over ID_LIKE whatever their order in the file.
pub fn family(os_release: &str) -> &'static str {
    let values = |key: &str| -> Vec<String> {
        os_release.lines().filter_map(|l| l.trim().strip_prefix(key))
            .flat_map(|v| v.trim().trim_matches(|c| c == '"' || c == '\'').split_whitespace().map(str::to_lowercase).collect::<Vec<_>>())
            .collect()
    };
    let ids: Vec<String> = values("ID=").into_iter().chain(values("ID_LIKE=")).collect();
    ids.iter().find_map(|id| match id.as_str() {
        "gentoo" => Some("gentoo"),
        "arch" | "archarm" => Some("arch"),
        "debian" | "ubuntu" => Some("debian"),
        "fedora" | "rhel" | "centos" => Some("fedora"),
        "suse" | "opensuse" | "sles" => Some("opensuse"),
        i if i.starts_with("opensuse") => Some("opensuse"),
        "void" => Some("void"),
        "alpine" => Some("alpine"),
        "nixos" => Some("nixos"),
        _ => None,
    }).unwrap_or("")
}

/// /etc/os-release, or /usr/lib/os-release where /etc has none (os-release(5)).
pub fn os_release() -> String {
    fs::read_to_string("/etc/os-release").or_else(|_| fs::read_to_string("/usr/lib/os-release")).unwrap_or_default()
}

pub fn this_family() -> &'static str { family(&os_release()) }

/// Excluded on every system. `/x/*` keeps the (empty) mount point in the archive.
pub const COMMON_EXCLUDES: &[&str] = &[
    "/proc/*", "/sys/*", "/dev/*", "/run/*", "/tmp/*", "/var/tmp/*", "/mnt/*", "/media/*", "/lost+found",
    "/var/cache/*", "/var/lib/systemd/coredump/*", "/.snapshots/*", "/timeshift/*",
];

/// What each family adds: data its package manager downloads again on its own.
pub fn family_excludes(family: &str) -> &'static [&'static str] {
    match family {
        // Distfiles / binary packages of the old layout (the new one is under /var/cache), and the
        // ebuild repository itself (emerge --sync / emerge-webrsync fetches it again).
        "gentoo" => &["/usr/portage/distfiles/*", "/usr/portage/packages/*", "/var/db/repos/gentoo/*"],
        // Sync databases (pacman -Sy).
        "arch" => &["/var/lib/pacman/sync/*"],
        // Package lists (apt update), crash reports, snapd's download cache and the mounted snap
        // revisions (squashfs images already kept in /var/lib/snapd/snaps).
        "debian" => &["/var/lib/apt/lists/*", "/var/crash/*", "/var/lib/snapd/cache/*", "/snap/*/[0-9]*/*"],
        // Fedora/RHEL (dnf: /var/cache), openSUSE (zypper: /var/cache, snapper: /.snapshots), Void and
        // Alpine keep everything re-downloadable under the common excludes already.
        _ => &[],
    }
}

/// The default exclude list of a system image on `family`.
pub fn image_excludes(family: &str) -> Vec<&'static str> {
    COMMON_EXCLUDES.iter().chain(family_excludes(family)).copied().collect()
}

/// Swap files in use (/proc/swaps type "file"): never worth archiving, and read back as garbage.
pub fn swap_files(proc_swaps: &str) -> Vec<String> {
    proc_swaps.lines().skip(1).filter_map(|l| {
        let mut f = l.split_whitespace();
        let (path, kind) = (f.next()?, f.next()?);
        (kind == "file" && path.starts_with('/')).then(|| unescape_octal(path))
    }).collect()
}

/// The kernel's escaping in /proc/swaps and /proc/mounts: space, tab, newline and backslash as "\ooo".
fn unescape_octal(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'\\' && i + 3 < b.len() && (b'0'..=b'3').contains(&b[i + 1])
            && b[i + 2..=i + 3].iter().all(|c| (b'0'..=b'7').contains(c)) {
            out.push((b[i + 1] - b'0') * 64 + (b[i + 2] - b'0') * 8 + (b[i + 3] - b'0'));
            i += 4;
        } else {
            out.push(b[i]);
            i += 1;
        }
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// System profile, every family: how the machine boots, mounts and loads modules.
pub const PROFILE_COMMON: &[&str] = &[
    "etc/os-release", "etc/fstab", "etc/default/grub", "etc/kernel", "etc/dracut.conf", "etc/dracut.conf.d",
    "etc/modprobe.d", "etc/modules-load.d", "etc/sysctl.conf", "etc/sysctl.d", "etc/udev/rules.d", "etc/X11/xorg.conf.d",
    "etc/environment", "etc/locale.gen", "etc/locale.conf", "etc/vconsole.conf", "etc/hostname", "etc/hosts",
    "etc/security/limits.conf", "etc/security/limits.d", "etc/systemd/system", "etc/systemd/logind.conf",
    "etc/systemd/sleep.conf", "etc/tmpfiles.d", "etc/systemd/zram-generator.conf", "etc/cmdline.d",
    // Boot loader entries: systemd-boot (on /boot or the ESP) and rEFInd's kernel options. Often root-only: listed as unreadable then.
    "boot/loader/loader.conf", "boot/loader/entries", "efi/loader/loader.conf", "efi/loader/entries",
    "boot/efi/loader/loader.conf", "boot/efi/loader/entries", "boot/refind_linux.conf",
];

/// System profile, per family: the package manager's configuration and the initramfs generator.
pub fn profile_paths(family: &str) -> &'static [&'static str] {
    match family {
        "gentoo" => &["etc/portage", "var/lib/portage/world", "var/lib/portage/world_sets", "usr/src/linux/.config",
                      "etc/conf.d", "etc/rc.conf", "etc/local.d", "etc/runlevels", "etc/env.d", "etc/genkernel.conf"],
        "arch" => &["etc/pacman.conf", "etc/pacman.d", "etc/makepkg.conf", "etc/makepkg.conf.d", "etc/mkinitcpio.conf", "etc/mkinitcpio.d",
                    "etc/paru.conf", "etc/booster.yaml", "etc/xdg/reflector/reflector.conf"],
        // NixOS is rebuilt from its configuration: configuration.nix, hardware-configuration.nix, flake.nix / flake.lock.
        "nixos" => &["etc/nixos", "etc/nix/nix.conf", "etc/nix/registry.json", "etc/nix/machines"],
        "debian" => &["etc/apt/sources.list", "etc/apt/sources.list.d", "etc/apt/preferences", "etc/apt/preferences.d",
                      "etc/apt/apt.conf.d", "etc/initramfs-tools", "etc/default"],
        // Not /etc/sysconfig as a whole: network scripts there hold Wi-Fi keys, and much of it is root-only.
        "fedora" => &["etc/dnf", "etc/yum.repos.d", "etc/selinux/config", "etc/sysconfig/kernel", "etc/sysconfig/cpupower"],
        "opensuse" => &["etc/zypp/repos.d", "etc/zypp/zypp.conf", "etc/sysconfig/bootloader", "etc/sysconfig/kernel",
                        "etc/snapper/configs"],
        "void" => &["etc/xbps.d", "etc/rc.conf", "etc/rc.local", "etc/runit/runsvdir/default"],
        "alpine" => &["etc/apk/world", "etc/apk/repositories", "etc/mkinitfs", "etc/runlevels", "etc/conf.d"],
        _ => &[],
    }
}

/// Never copied into a profile: key rings, agent sockets and repository credentials. Network configuration
/// (netplan, /etc/network/interfaces, NetworkManager, ifcfg) is left out of the lists for the same reason:
/// it may hold Wi-Fi passphrases, and the profile is a file of the user's.
pub const PROFILE_SKIP: &[&str] = &["/etc/portage/gnupg", "/etc/pacman.d/gnupg", "/etc/zypp/credentials.d",
                                    "/etc/apt/auth.conf", "/etc/apt/auth.conf.d", "/etc/nix/netrc", "/etc/nixos/secrets"];

/// Gentoo: the ebuild repositories repos.conf defines without a sync source (local overlays: nothing fetches them
/// again), as (name, location). The Gentoo repository and synced overlays (eselect repository / git) are left out:
/// their definitions are in /etc/portage/repos.conf, which the profile keeps.
pub fn gentoo_local_overlays(repos_conf: &str) -> Vec<(String, String)> {
    let mut out = Vec::new();
    let mut flush = |name: &Option<String>, loc: &Option<String>, synced: bool| {
        if let (Some(n), Some(l)) = (name, loc) {
            if !synced && n != "DEFAULT" && n != "gentoo" && l.starts_with('/') && !l.contains("..") { out.push((n.clone(), l.clone())); }
        }
    };
    let (mut name, mut loc, mut synced) = (None::<String>, None::<String>, false);
    for raw in repos_conf.lines() {
        let line = raw.trim();
        if line.starts_with('#') || line.is_empty() { continue; }
        if let Some(sec) = line.strip_prefix('[').and_then(|l| l.strip_suffix(']')) {
            flush(&name, &loc, synced);
            (name, loc, synced) = (Some(sec.trim().to_owned()), None, false);
            continue;
        }
        let Some((k, v)) = line.split_once('=') else { continue };
        match k.trim() {
            "location" => loc = Some(v.trim().trim_end_matches('/').to_owned()),
            "sync-uri" | "sync-type" if !v.trim().is_empty() => synced = true,
            _ => {}
        }
    }
    flush(&name, &loc, synced);
    out
}

/// /etc/portage/repos.conf as one text, whether it is a file or a directory of *.conf files.
pub fn repos_conf_text() -> String {
    let p = Path::new("/etc/portage/repos.conf");
    if p.is_file() { return fs::read_to_string(p).unwrap_or_default(); }
    let mut files: Vec<_> = fs::read_dir(p).into_iter().flatten().flatten().map(|e| e.path()).filter(|f| f.is_file()).collect();
    files.sort();
    files.iter().filter_map(|f| fs::read_to_string(f).ok()).collect::<Vec<_>>().join("\n")
}

/// NixOS: the packages of the running system and the user's profile, from the store paths their bin/ links
/// point into ("name-version"), without running nix.
pub fn nix_packages(dirs: &[&Path]) -> Vec<String> {
    let mut out = Vec::new();
    for d in dirs {
        for e in fs::read_dir(d).into_iter().flatten().flatten() {
            let Ok(t) = fs::read_link(e.path()) else { continue };
            let Some(rest) = t.to_str().and_then(|t| t.strip_prefix("/nix/store/")) else { continue };
            let Some(pkg) = rest.split('/').next().and_then(|h| h.get(33..)) else { continue };  // "<32-char hash>-name-version"
            if !pkg.is_empty() { out.push(pkg.to_owned()); }
        }
    }
    out.sort();
    out.dedup();
    out
}

/// The profile paths for `family`: the common ones and the family's, without a path that another one in the
/// list already contains (Debian's etc/default holds etc/default/grub), so no file is copied or counted twice.
pub fn profile_list(family: &str) -> Vec<&'static str> {
    let all: Vec<&'static str> = PROFILE_COMMON.iter().chain(profile_paths(family)).copied().collect();
    let mut out: Vec<&'static str> = Vec::new();
    for p in &all {
        if out.contains(p) { continue; }
        if all.iter().any(|q| q != p && p.starts_with(&format!("{q}/"))) { continue; }
        out.push(p);
    }
    out
}

/// Installed packages, one per line: "category/name-version" (Gentoo's own atoms) or "name version"
/// (everything else; a space because Debian and RPM names may contain "-<digit>"). Empty when the
/// package database is unknown or unreadable.
pub fn installed_packages(family: &str) -> Vec<String> {
    let mut out = match family {
        "gentoo" => gentoo_vdb(Path::new("/var/db/pkg")),
        "arch" => pacman_local(Path::new("/var/lib/pacman/local")),
        "debian" => dpkg_status(&fs::read_to_string("/var/lib/dpkg/status").unwrap_or_default()),
        "alpine" => apk_installed(&fs::read_to_string("/lib/apk/db/installed").unwrap_or_default()),
        "nixos" => {
            let user = std::env::var("USER").unwrap_or_default();
            let per_user = format!("/etc/profiles/per-user/{user}/bin");
            let mut dirs = vec![Path::new("/run/current-system/sw/bin")];
            if !user.is_empty() && !user.contains('/') { dirs.push(Path::new(&per_user)); }
            nix_packages(&dirs)
        }
        "fedora" | "opensuse" => command_lines("rpm", &["-qa", "--qf", "%{NAME} %{VERSION}-%{RELEASE}.%{ARCH}\\n"]),
        "void" => command_lines("xbps-query", &["-l"]).iter().filter_map(|l| {
            // "ii name-1.2_3 description"
            let pv = l.split_whitespace().nth(1)?;
            let i = pv.rfind('-')?;
            Some(format!("{} {}", &pv[..i], &pv[i + 1..]))
        }).collect(),
        _ => Vec::new(),
    };
    out.sort();
    out.dedup();
    out
}

/// Packages the user asked for (not pulled in as dependencies), when the database says so.
pub fn explicit_packages(family: &str) -> Vec<String> {
    let mut out = match family {
        "gentoo" => fs::read_to_string("/var/lib/portage/world").unwrap_or_default().lines().map(str::to_owned).collect(),
        "arch" => pacman_explicit(Path::new("/var/lib/pacman/local")),
        "debian" => {
            // extended_states names packages without ":arch"; a foreign-architecture one is compared by its name.
            let auto = apt_auto(&fs::read_to_string("/var/lib/apt/extended_states").unwrap_or_default());
            dpkg_status(&fs::read_to_string("/var/lib/dpkg/status").unwrap_or_default()).into_iter()
                .filter_map(|l| l.split(' ').next().map(str::to_owned))
                .filter(|n| !auto.contains(n.split(':').next().unwrap_or(n))).collect()
        }
        "alpine" => fs::read_to_string("/etc/apk/world").unwrap_or_default().lines().map(str::to_owned).collect(),
        _ => Vec::new(),
    };
    for l in out.iter_mut() { *l = l.trim().to_owned(); }
    out.retain(|l| !l.is_empty() && !l.starts_with('#'));
    out.sort();
    out.dedup();
    out
}

/// Packages that come from outside the distribution's repositories (Arch: the AUR and local builds, `pacman -Qqm`).
pub fn foreign_packages(family: &str) -> Vec<String> {
    match family { "arch" => command_lines("pacman", &["-Qqm"]), _ => Vec::new() }
}

/// A package query's output lines. Bounded: a locked or broken database must not hang a configuration backup
/// (the automatic one runs unattended), so the command is killed after COMMAND_TIMEOUT and its output dropped.
fn command_lines(name: &str, args: &[&str]) -> Vec<String> {
    use std::io::Read as _;
    const COMMAND_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(20);
    const MAX_OUTPUT: u64 = 16 << 20;
    let Some(bin) = ["/usr/bin", "/bin", "/usr/sbin", "/sbin"].iter().map(|d| Path::new(d).join(name)).find(|p| p.is_file()) else {
        return Vec::new();
    };
    let Ok(mut child) = std::process::Command::new(bin).args(args).env_clear().env("PATH", "/usr/sbin:/usr/bin:/sbin:/bin")
        .env("LC_ALL", "C").stdin(std::process::Stdio::null()).stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::null()).spawn() else { return Vec::new() };
    // Read on a thread (a full pipe would block the child); wait here with a deadline.
    let Some(mut stdout) = child.stdout.take() else { let _ = child.kill(); let _ = child.wait(); return Vec::new() };
    let reader = std::thread::spawn(move || {
        let mut buf = Vec::new();
        let _ = (&mut stdout).take(MAX_OUTPUT).read_to_end(&mut buf);
        buf
    });
    let deadline = std::time::Instant::now() + COMMAND_TIMEOUT;
    let ok = loop {
        match child.try_wait() {
            Ok(Some(st)) => break st.success(),
            Ok(None) if std::time::Instant::now() < deadline => std::thread::sleep(std::time::Duration::from_millis(50)),
            _ => { let _ = child.kill(); let _ = child.wait(); break false; }
        }
    };
    let out = reader.join().unwrap_or_default();
    if !ok { return Vec::new(); }
    String::from_utf8_lossy(&out).lines().map(str::trim).filter(|l| !l.is_empty()).map(str::to_owned).collect()
}

fn gentoo_vdb(root: &Path) -> Vec<String> {
    let mut pkgs = Vec::new();
    for cat in fs::read_dir(root).into_iter().flatten().flatten() {
        for p in fs::read_dir(cat.path()).into_iter().flatten().flatten() {
            pkgs.push(format!("{}/{}", cat.file_name().to_string_lossy(), p.file_name().to_string_lossy()));
        }
    }
    pkgs
}

/// A pacman "desc" file: %NAME%, %VERSION%, %REASON% (1 = installed as a dependency).
fn pacman_desc(text: &str) -> (Option<String>, Option<String>, bool) {
    let (mut name, mut version, mut dep) = (None, None, false);
    let mut lines = text.lines();
    while let Some(l) = lines.next() {
        match l.trim() {
            "%NAME%" => name = lines.next().map(|s| s.trim().to_owned()),
            "%VERSION%" => version = lines.next().map(|s| s.trim().to_owned()),
            "%REASON%" => dep = lines.next().is_some_and(|s| s.trim() == "1"),
            _ => {}
        }
    }
    (name, version, dep)
}

fn pacman_local(root: &Path) -> Vec<String> {
    fs::read_dir(root).into_iter().flatten().flatten()
        .filter_map(|e| fs::read_to_string(e.path().join("desc")).ok())
        .filter_map(|d| match pacman_desc(&d) { (Some(n), Some(v), _) => Some(format!("{n} {v}")), _ => None })
        .collect()
}

fn pacman_explicit(root: &Path) -> Vec<String> {
    fs::read_dir(root).into_iter().flatten().flatten()
        .filter_map(|e| fs::read_to_string(e.path().join("desc")).ok())
        .filter_map(|d| match pacman_desc(&d) { (Some(n), _, false) => Some(n), _ => None })
        .collect()
}

/// dpkg's status file: the stanzas whose Status ends in "installed". A package of a foreign architecture
/// (multiarch: i386 next to amd64) is named "name:arch"; the native one is the most common architecture.
fn dpkg_status(text: &str) -> Vec<String> {
    let text = text.replace("\r\n", "\n");
    let rows: Vec<(&str, &str, &str)> = text.split("\n\n").filter_map(|st| {
        let field = |k: &str| st.lines().find_map(|l| l.strip_prefix(k)).map(str::trim);
        if !field("Status:")?.ends_with(" installed") { return None; }
        Some((field("Package:")?, field("Architecture:").unwrap_or("all"), field("Version:").unwrap_or("")))
    }).collect();
    let mut count: std::collections::HashMap<&str, usize> = std::collections::HashMap::new();
    for (_, a, _) in &rows { if *a != "all" { *count.entry(a).or_default() += 1; } }
    let native = count.into_iter().max_by_key(|(a, n)| (*n, std::cmp::Reverse(*a))).map(|(a, _)| a).unwrap_or("");
    rows.iter().map(|(n, a, v)| if *a == "all" || *a == native { format!("{n} {v}") } else { format!("{n}:{a} {v}") }).collect()
}

/// apt's extended_states: names marked "Auto-Installed: 1".
fn apt_auto(text: &str) -> std::collections::HashSet<String> {
    text.split("\n\n").filter(|st| st.lines().any(|l| l.trim() == "Auto-Installed: 1"))
        .filter_map(|st| st.lines().find_map(|l| l.strip_prefix("Package:")).map(|n| n.trim().to_owned())).collect()
}

/// apk's installed database: "P:" name, "V:" version per stanza.
fn apk_installed(text: &str) -> Vec<String> {
    text.split("\n\n").filter_map(|st| {
        let field = |k: &str| st.lines().find_map(|l| l.strip_prefix(k));
        Some(format!("{} {}", field("P:")?, field("V:").unwrap_or("")))
    }).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn families() {
        assert_eq!(family("NAME=Gentoo\nID=gentoo\n"), "gentoo");
        assert_eq!(family("ID=manjaro\nID_LIKE=arch\n"), "arch");
        assert_eq!(family("ID=endeavouros\nID_LIKE=\"arch\"\n"), "arch");
        assert_eq!(family("ID=linuxmint\nID_LIKE=\"ubuntu debian\"\n"), "debian");
        assert_eq!(family("ID=pop\nID_LIKE=\"ubuntu debian\"\n"), "debian");
        assert_eq!(family("ID=\"rocky\"\nID_LIKE=\"rhel centos fedora\"\n"), "fedora");
        assert_eq!(family("ID=\"opensuse-tumbleweed\"\nID_LIKE=\"opensuse suse\"\n"), "opensuse");
        assert_eq!(family("ID=void\n"), "void");
        assert_eq!(family("ID=alpine\n"), "alpine");
        assert_eq!(family("ID=nixos\n"), "nixos");
        assert_eq!(family(""), "");
    }

    #[test]
    fn excludes_keep_mount_points() {
        for f in ["gentoo", "arch", "debian", "fedora", "opensuse", "void", "alpine", ""] {
            let ex = image_excludes(f);
            for m in ["/proc/*", "/sys/*", "/dev/*", "/run/*"] { assert!(ex.contains(&m), "{f}: {m}"); }
            assert!(ex.iter().all(|e| e.starts_with('/')), "{f}");
            assert!(!ex.contains(&"/proc"), "{f}");
        }
        assert!(image_excludes("gentoo").contains(&"/usr/portage/distfiles/*"));
        assert!(!image_excludes("arch").contains(&"/usr/portage/distfiles/*"));
    }

    /// The GUI's copy (Defaults button, the per-distribution menu) holds exactly the same lists.
    #[test]
    fn gui_copy_matches() {
        const GUI: &str = include_str!("../../../gui/src/distroprofile.cpp");
        let array = |name: &str| -> Vec<String> {
            let start = GUI.find(&format!("const char *const {name}[] = {{")).unwrap_or_else(|| panic!("{name} not in distroprofile.cpp"));
            let body = &GUI[start..];
            let body = &body[body.find('{').unwrap() + 1..body.find("};").unwrap()];
            body.split('"').skip(1).step_by(2).map(str::to_owned).collect()
        };
        assert_eq!(array("COMMON"), COMMON_EXCLUDES.iter().map(|s| s.to_string()).collect::<Vec<_>>(), "COMMON");
        for (f, name) in [("gentoo", "GENTOO"), ("arch", "ARCH"), ("debian", "DEBIAN")] {
            assert_eq!(array(name), family_excludes(f).iter().map(|s| s.to_string()).collect::<Vec<_>>(), "{name}");
        }
        for f in ["fedora", "opensuse", "void", "alpine", ""] { assert!(family_excludes(f).is_empty(), "{f}: the GUI has no list for it"); }
    }

    #[test]
    fn id_wins_over_id_like() {
        // ID_LIKE first in the file: ID still decides.
        assert_eq!(family("ID_LIKE=\"ubuntu debian\"\nID=arch\n"), "arch");
        assert_eq!(family("  ID=Gentoo\n"), "gentoo");
        assert_eq!(family("VERSION_ID=12\nID=debian\n"), "debian");
    }

    #[test]
    fn profile_list_has_no_nested_duplicates() {
        for f in ["gentoo", "arch", "debian", "fedora", "opensuse", "void", "alpine", ""] {
            let l = profile_list(f);
            for p in &l {
                assert!(!l.iter().any(|q| q != p && p.starts_with(&format!("{q}/"))), "{f}: {p}");
                for bad in ["netplan", "network/interfaces", "crypttab", "NetworkManager", "shadow", "sudoers"] {
                    assert!(!p.contains(bad), "{f}: {p} may hold secrets");
                }
            }
            assert_eq!(l.len(), l.iter().collect::<std::collections::HashSet<_>>().len(), "{f}: duplicates");
        }
        assert!(profile_list("debian").contains(&"etc/default") && !profile_list("debian").contains(&"etc/default/grub"));
        assert!(profile_list("arch").contains(&"etc/default/grub"));
    }

    #[test]
    fn command_lines_missing_or_failing() {
        assert!(command_lines("centurion-no-such-tool", &[]).is_empty());
        assert!(command_lines("false", &[]).is_empty());
        assert_eq!(command_lines("echo", &["one", "two"]), vec!["one two".to_string()]);
    }

    #[test]
    fn swaps() {
        let s = "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n/swapfile                               file\t\t8388604\t\t0\t\t-2\n\
                 /dev/zram0                              partition\t4194300\t\t0\t\t100\n/swap\\040dir/sf file 1 0 -3\n";
        assert_eq!(swap_files(s), vec!["/swapfile".to_string(), "/swap dir/sf".to_string()]);
        assert_eq!(unescape_octal("/a\\011b\\134c\\0"), "/a\tb\\c\\0");
    }

    #[test]
    fn package_databases() {
        let desc = "%NAME%\nlinux\n\n%VERSION%\n6.11.1.arch1-1\n\n%REASON%\n1\n";
        assert_eq!(pacman_desc(desc), (Some("linux".into()), Some("6.11.1.arch1-1".into()), true));
        let st = "Package: gcc-14-base\nStatus: install ok installed\nArchitecture: amd64\nVersion: 14.2.0-4\n\n\
                  Package: old\nStatus: deinstall ok config-files\nVersion: 1\n\n\
                  Package: libc6\nStatus: install ok installed\nArchitecture: i386\nVersion: 2.40-1\n";
        assert_eq!(dpkg_status(st), vec!["gcc-14-base 14.2.0-4".to_string(), "libc6:i386 2.40-1".to_string()]);
        // An arm64 system: its own packages carry no suffix.
        let arm = "Package: a\nStatus: install ok installed\nArchitecture: arm64\nVersion: 1\n\n\
                   Package: b\nStatus: install ok installed\nArchitecture: arm64\nVersion: 2\n\n\
                   Package: c\nStatus: install ok installed\nArchitecture: armhf\nVersion: 3\n";
        assert_eq!(dpkg_status(arm), vec!["a 1".to_string(), "b 2".to_string(), "c:armhf 3".to_string()]);
        let auto = apt_auto("Package: a\nArchitecture: amd64\nAuto-Installed: 1\n\nPackage: b\nAuto-Installed: 0\n");
        assert!(auto.contains("a") && !auto.contains("b"));
        assert_eq!(apk_installed("C:x\nP:musl\nV:1.2.5-r0\n\nP:busybox\nV:1.36\n"), vec!["musl 1.2.5-r0".to_string(), "busybox 1.36".to_string()]);
    }

    #[test]
    fn gentoo_overlays_and_nix_packages() {
        let conf = "[DEFAULT]\nmain-repo = gentoo\n\n[gentoo]\nlocation = /var/db/repos/gentoo\nsync-type = rsync\n\n\
                    [local]\nlocation = /var/db/repos/local/\n\n[guru]\nlocation = /var/db/repos/guru\nsync-type = git\nsync-uri = https://x\n\n\
                    [bad]\nlocation = ../etc\n";
        assert_eq!(gentoo_local_overlays(conf), vec![("local".to_string(), "/var/db/repos/local".to_string())]);
        assert!(profile_list("nixos").contains(&"etc/nixos"));
        assert!(PROFILE_SKIP.contains(&"/etc/nix/netrc"));
        let dir = std::env::temp_dir().join(format!("centurion-nix-{}", std::process::id()));
        let _ = fs::create_dir_all(&dir);
        let _ = std::os::unix::fs::symlink("/nix/store/0123456789abcdfghijklmnpqrsvwxyz-firefox-130.0/bin/firefox", dir.join("firefox"));
        let _ = std::os::unix::fs::symlink("/usr/bin/x", dir.join("other"));
        assert_eq!(nix_packages(&[&dir]), vec!["firefox-130.0".to_string()]);
        let _ = fs::remove_dir_all(&dir);
    }
}
