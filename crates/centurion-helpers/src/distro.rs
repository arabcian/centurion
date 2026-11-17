//! Which distribution this is, and what that means for a backup: the system image's default exclude
//! list (package caches, re-downloadable repository data, snapshot stores) and the files the
//! configuration backup's "system profile" keeps (package manager configuration, initramfs, the list
//! of installed packages).
//!
//! The GUI keeps a copy of the exclude lists (gui/src/distroprofile.cpp); a test below checks that
//! every pattern here is spelled the same there.

use std::fs;
use std::path::Path;

/// "gentoo", "arch", "debian", "fedora", "opensuse", "void", "alpine", or "" for anything else.
/// Derivatives are found through ID_LIKE (Manjaro, EndeavourOS, CachyOS → arch; Ubuntu, Mint,
/// Pop!_OS → debian; RHEL, Rocky, Alma, CentOS → fedora; Leap, Tumbleweed, SLES → opensuse).
pub fn family(os_release: &str) -> &'static str {
    let ids: Vec<String> = os_release.lines()
        .filter_map(|l| l.strip_prefix("ID=").or_else(|| l.strip_prefix("ID_LIKE=")))
        .flat_map(|v| v.trim().trim_matches(|c| c == '"' || c == '\'').split_whitespace().map(str::to_owned).collect::<Vec<_>>())
        .collect();
    ids.iter().find_map(|id| match id.as_str() {
        "gentoo" => Some("gentoo"),
        "arch" | "archarm" => Some("arch"),
        "debian" | "ubuntu" => Some("debian"),
        "fedora" | "rhel" | "centos" => Some("fedora"),
        "suse" | "opensuse" | "sles" => Some("opensuse"),
        i if i.starts_with("opensuse") => Some("opensuse"),
        "void" => Some("void"),
        "alpine" => Some("alpine"),
        _ => None,
    }).unwrap_or("")
}

pub fn this_family() -> &'static str { family(&fs::read_to_string("/etc/os-release").unwrap_or_default()) }

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
        (kind == "file" && path.starts_with('/')).then(|| path.replace("\\040", " "))
    }).collect()
}

/// System profile, every family: how the machine boots, mounts and loads modules.
pub const PROFILE_COMMON: &[&str] = &[
    "etc/os-release", "etc/fstab", "etc/crypttab", "etc/default/grub", "etc/kernel", "etc/dracut.conf", "etc/dracut.conf.d",
    "etc/modprobe.d", "etc/modules-load.d", "etc/sysctl.conf", "etc/sysctl.d", "etc/udev/rules.d", "etc/X11/xorg.conf.d",
    "etc/environment", "etc/locale.gen", "etc/locale.conf", "etc/vconsole.conf", "etc/hostname", "etc/hosts",
    "etc/security/limits.conf", "etc/security/limits.d", "etc/systemd/system", "etc/systemd/logind.conf",
    "etc/systemd/sleep.conf", "etc/tmpfiles.d",
];

/// System profile, per family: the package manager's configuration and the initramfs generator.
pub fn profile_paths(family: &str) -> &'static [&'static str] {
    match family {
        "gentoo" => &["etc/portage", "var/lib/portage/world", "var/lib/portage/world_sets", "usr/src/linux/.config",
                      "etc/conf.d", "etc/rc.conf", "etc/local.d", "etc/runlevels", "etc/env.d", "etc/genkernel.conf"],
        "arch" => &["etc/pacman.conf", "etc/pacman.d", "etc/makepkg.conf", "etc/mkinitcpio.conf", "etc/mkinitcpio.d"],
        "debian" => &["etc/apt/sources.list", "etc/apt/sources.list.d", "etc/apt/preferences", "etc/apt/preferences.d",
                      "etc/apt/apt.conf.d", "etc/initramfs-tools", "etc/default", "etc/netplan", "etc/network/interfaces"],
        "fedora" => &["etc/dnf", "etc/yum.repos.d", "etc/sysconfig", "etc/selinux/config"],
        "opensuse" => &["etc/zypp/repos.d", "etc/zypp/zypp.conf", "etc/sysconfig", "etc/snapper/configs"],
        "void" => &["etc/xbps.d", "etc/rc.conf", "etc/rc.local", "etc/runit/runsvdir/default"],
        "alpine" => &["etc/apk/world", "etc/apk/repositories", "etc/mkinitfs", "etc/runlevels", "etc/conf.d"],
        _ => &[],
    }
}

/// Never copied into a profile: key rings, agent sockets and repository credentials.
pub const PROFILE_SKIP: &[&str] = &["/etc/portage/gnupg", "/etc/pacman.d/gnupg", "/etc/zypp/credentials.d",
                                    "/etc/apt/auth.conf", "/etc/apt/auth.conf.d"];

/// Installed packages, one per line: "category/name-version" (Gentoo's own atoms) or "name version"
/// (everything else; a space because Debian and RPM names may contain "-<digit>"). Empty when the
/// package database is unknown or unreadable.
pub fn installed_packages(family: &str) -> Vec<String> {
    let mut out = match family {
        "gentoo" => gentoo_vdb(Path::new("/var/db/pkg")),
        "arch" => pacman_local(Path::new("/var/lib/pacman/local")),
        "debian" => dpkg_status(&fs::read_to_string("/var/lib/dpkg/status").unwrap_or_default()),
        "alpine" => apk_installed(&fs::read_to_string("/lib/apk/db/installed").unwrap_or_default()),
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
            let auto = apt_auto(&fs::read_to_string("/var/lib/apt/extended_states").unwrap_or_default());
            dpkg_status(&fs::read_to_string("/var/lib/dpkg/status").unwrap_or_default()).into_iter()
                .filter_map(|l| l.split(' ').next().map(str::to_owned)).filter(|n| !auto.contains(n)).collect()
        }
        "alpine" => fs::read_to_string("/etc/apk/world").unwrap_or_default().lines().map(str::to_owned).collect(),
        _ => Vec::new(),
    };
    out.retain(|l| !l.trim().is_empty());
    out.sort();
    out.dedup();
    out
}

fn command_lines(name: &str, args: &[&str]) -> Vec<String> {
    let Some(bin) = ["/usr/bin", "/bin", "/usr/sbin", "/sbin"].iter().map(|d| Path::new(d).join(name)).find(|p| p.is_file()) else {
        return Vec::new();
    };
    std::process::Command::new(bin).args(args).env_clear().env("PATH", "/usr/sbin:/usr/bin:/sbin:/bin").env("LC_ALL", "C")
        .stdin(std::process::Stdio::null()).stderr(std::process::Stdio::null()).output()
        .map(|o| String::from_utf8_lossy(&o.stdout).lines().map(str::trim).filter(|l| !l.is_empty()).map(str::to_owned).collect())
        .unwrap_or_default()
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

/// dpkg's status file: the stanzas whose Status ends in "installed".
fn dpkg_status(text: &str) -> Vec<String> {
    text.split("\n\n").filter_map(|st| {
        let field = |k: &str| st.lines().find_map(|l| l.strip_prefix(k)).map(str::trim);
        let status = field("Status:")?;
        if !status.ends_with(" installed") { return None; }
        let name = field("Package:")?;
        let arch = field("Architecture:").filter(|a| *a != "all" && *a != "amd64");
        Some(match arch { Some(a) => format!("{name}:{a} {}", field("Version:").unwrap_or("")), None => format!("{name} {}", field("Version:").unwrap_or("")) })
    }).collect()
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
        assert_eq!(family("ID=nixos\n"), "");
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

    /// The GUI's copy (Defaults button, the per-distribution menu) spells every pattern the same.
    #[test]
    fn gui_copy_matches() {
        const GUI: &str = include_str!("../../../gui/src/distroprofile.cpp");
        for f in ["gentoo", "arch", "debian", "fedora", "opensuse"] {
            for e in image_excludes(f) { assert!(GUI.contains(&format!("\"{e}\"")), "distroprofile.cpp lacks {e} ({f})"); }
        }
    }

    #[test]
    fn swaps() {
        let s = "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n/swapfile                               file\t\t8388604\t\t0\t\t-2\n\
                 /dev/zram0                              partition\t4194300\t\t0\t\t100\n/swap\\040dir/sf file 1 0 -3\n";
        assert_eq!(swap_files(s), vec!["/swapfile".to_string(), "/swap dir/sf".to_string()]);
    }

    #[test]
    fn package_databases() {
        let desc = "%NAME%\nlinux\n\n%VERSION%\n6.11.1.arch1-1\n\n%REASON%\n1\n";
        assert_eq!(pacman_desc(desc), (Some("linux".into()), Some("6.11.1.arch1-1".into()), true));
        let st = "Package: gcc-14-base\nStatus: install ok installed\nArchitecture: amd64\nVersion: 14.2.0-4\n\n\
                  Package: old\nStatus: deinstall ok config-files\nVersion: 1\n\n\
                  Package: libc6\nStatus: install ok installed\nArchitecture: i386\nVersion: 2.40-1\n";
        assert_eq!(dpkg_status(st), vec!["gcc-14-base 14.2.0-4".to_string(), "libc6:i386 2.40-1".to_string()]);
        let auto = apt_auto("Package: a\nArchitecture: amd64\nAuto-Installed: 1\n\nPackage: b\nAuto-Installed: 0\n");
        assert!(auto.contains("a") && !auto.contains("b"));
        assert_eq!(apk_installed("C:x\nP:musl\nV:1.2.5-r0\n\nP:busybox\nV:1.36\n"), vec!["musl 1.2.5-r0".to_string(), "busybox 1.36".to_string()]);
    }
}
