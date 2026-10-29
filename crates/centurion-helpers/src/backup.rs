//! Backup engine behind Health → Backup (backup-helper).
//!
//! Two kinds of archive:
//!
//! * System image — the whole root file system through
//!   `tar --acls --xattrs --xattrs-include='*' -cpf - / | <compressor>`,
//!   written as `backup-YYYY-MM-DD.tar.<ext>` (0600 root: it holds /etc/shadow),
//!   and the matching restore `<decompressor> | tar … -xpf - -C <target>`.
//!   Root only; progress lines are streamed while it runs.
//!
//! * Centurion configuration — everything Centurion has stored (user
//!   config, root-owned presets/boot profiles/NVIDIA curves/calibration) plus an
//!   optional "system profile" (Portage config, world set, kernel config, fstab,
//!   boot/module/sysctl configuration, package list), as one small dated
//!   `centurion-config-YYYY-MM-DD_HHMMSS.tar.gz`. Made unprivileged from readable files.
//!   Restore: the user part unprivileged; the root part through a whitelist,
//!   every file re-created root:root 0644 (owner/mode in the archive are never
//!   trusted). The system profile is reference material and is never restored.
//!
//! Cancel: the GUI closes our stdin; the children (own process groups) are
//! terminated and the partial archive is removed.
//!
//! Encryption (optional): the image goes through `age -r <public key>`. Reading it back needs the age
//! secret key, which comes with the request and is written to a 0600 scratch file for that run only.
//!
//! Increments (optional): every image has a snapshot next to it (`<image>.snar`, GNU tar's
//! listed-incremental). An increment is one level above the image it continues; retention keeps
//! whole chains, and a restore unpacks the full image and then each increment in order.
//!
//! config_diff: what changed between two configuration archives (read only).

use serde_json::{json, Value};
use std::collections::{BTreeMap, BTreeSet, VecDeque};
use std::fs::{self, File};
use std::io::{BufRead, BufReader, Read, Write};
use std::os::unix::ffi::{OsStrExt, OsStringExt};
use std::os::unix::fs::{MetadataExt, OpenOptionsExt, PermissionsExt};
use std::os::unix::io::AsRawFd;
use std::os::unix::process::{CommandExt, ExitStatusExt};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, ExitStatus, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime};

/// The exclude list of the system image. `/x/*` rather than `/x`: the (empty)
/// mount points stay in the archive, so a restore onto a fresh file system
/// still has /proc, /sys, /dev, /run and /tmp to mount on.
pub const DEFAULT_EXCLUDES: &[&str] = &[
    "/proc/*", "/sys/*", "/dev/*", "/run/*", "/tmp/*", "/var/tmp/*", "/mnt/*", "/media/*",
    "/lost+found", "/var/cache/*", "/usr/portage/distfiles/*",
];
pub const HOME_EXCLUDE: &str = "/home/*";
/// Mount points created in the restore target when the archive lacks them
/// (archives made with plain `--exclude=/proc`): (path, mode).
const MOUNT_POINTS: &[(&str, u32)] = &[("proc", 0o755), ("sys", 0o755), ("dev", 0o755), ("run", 0o755),
    ("tmp", 0o1777), ("var/tmp", 0o1777), ("mnt", 0o755), ("media", 0o755), ("home", 0o755)];

pub const CONFIG_FORMAT: &str = "centurion-config-backup";
const MANIFEST: &str = "Centurion-BACKUP.json";
/// Root-owned Centurion state saved in a configuration backup (relative to /).
const SYS_CENTURION: &[&str] = &["etc/centurion", "etc/nvcurve", "var/lib/centurion",
    "etc/modprobe.d/zz-centurion-nvidia.conf"];
const SYS_CENTURION_LOGS: &[&str] = &["var/log/centurion"];
/// /var/lib/centurion files worth restoring; the rest (boot guard
/// state, boot-default snapshot, pending GPU mode) belongs to the boot it was made in.
/// /etc/centurion files a restore may write (everything the helpers keep there); anything else in an archive is skipped.
const ETC_RESTORE: &[&str] = &["tune-boot.json", "intel-uv-boot.json", "netguard.json", "wakeup.json", "dgpu-awake", "wireless-pm-lock"];
const VARLIB_RESTORE: &[&str] = &["signature.json", "calibration.json", "outcomes.json", "io-probe.json", "wmae-verified.json"];
/// "System profile": how this installation is put together. Reference only.
const PROFILE: &[&str] = &[
    "etc/portage", "var/lib/portage/world", "var/lib/portage/world_sets", "usr/src/linux/.config",
    "etc/fstab", "etc/default/grub", "etc/kernel", "etc/dracut.conf", "etc/dracut.conf.d",
    "etc/modprobe.d", "etc/modules-load.d", "etc/sysctl.conf", "etc/sysctl.d", "etc/conf.d", "etc/rc.conf",
    "etc/local.d", "etc/runlevels", "etc/udev/rules.d", "etc/X11/xorg.conf.d", "etc/environment", "etc/env.d",
    "etc/locale.gen", "etc/hosts", "etc/security/limits.conf", "etc/security/limits.d",
];
/// Left out of the system profile: Portage's own GnuPG home (root 0700). It is
/// the keyring `getuto` regenerates for verifying the tree, not configuration,
/// and its private-key / agent-socket directories must not land in a user file.
const PROFILE_SKIP: &[&str] = &["/etc/portage/gnupg"];
const MAX_FILE: u64 = 32 << 20;       // one staged file
const MAX_STAGE: u64 = 256 << 20;     // a whole configuration backup
const MAX_ROOT_FILE: u64 = 16 << 20;  // one root file on restore

// ── cancel / children ───────────────────────────────────────────────────────

static CANCEL: AtomicBool = AtomicBool::new(false);
static PGIDS: Mutex<Vec<i32>> = Mutex::new(Vec::new());

fn kill_all(sig: libc::c_int) {
    for &p in PGIDS.lock().unwrap_or_else(|e| e.into_inner()).iter() { unsafe { libc::kill(-p, sig); } }
}
fn cancelled() -> bool { CANCEL.load(Ordering::Relaxed) }

/// After the request line: anything else on stdin is ignored, EOF cancels.
pub fn cancel_on_stdin_eof() {
    std::thread::spawn(|| {
        let mut b = [0u8; 256];
        while matches!(std::io::stdin().read(&mut b), Ok(n) if n > 0) {}
        CANCEL.store(true, Ordering::Relaxed);
        kill_all(libc::SIGTERM);
    });
}

fn is_root() -> bool { unsafe { libc::geteuid() == 0 } }

/// tar / compressors from the system directories only. As root the binary and
/// every directory above it must be root-owned and not group/other-writable.
fn tool(name: &str) -> Option<PathBuf> {
    ["/usr/bin", "/bin", "/usr/sbin", "/sbin"].iter().map(|d| Path::new(d).join(name))
        .find(|p| if is_root() { crate::trusted_path(p) } else { p.is_file() })
}

fn command(path: &Path, low_priority: bool) -> Command {
    let mut c = Command::new(path);
    // argv[0] = bare name: tar prefixes its messages with it ("tar: …"), which is
    // how the readers tell messages from member names.
    if let Some(n) = path.file_name() { c.arg0(n); }
    c.env_clear().env("PATH", "/usr/sbin:/usr/bin:/sbin:/bin").env("LC_ALL", "C").process_group(0);
    if low_priority {
        unsafe {
            c.pre_exec(|| {
                libc::setpriority(libc::PRIO_PROCESS as _, 0, 19);
                libc::syscall(libc::SYS_ioprio_set, 1, 0, (2 << 13) | 7);  // best-effort, lowest level
                Ok(())
            });
        }
    }
    c
}

fn spawn(mut c: Command, what: &str) -> Result<Child, String> {
    let child = c.spawn().map_err(|e| format!("{what}: {e}"))?;
    PGIDS.lock().unwrap_or_else(|e| e.into_inner()).push(child.id() as i32);
    Ok(child)  // `c` (and the pipe ends it holds) is dropped here
}

// ── progress shared with the reader threads ─────────────────────────────────

#[derive(Default)]
struct Progress {
    files: AtomicU64,
    current: Mutex<String>,
    msg_count: AtomicU64,
    msgs: Mutex<VecDeque<String>>,   // last lines tar / the compressor printed
}

impl Progress {
    fn message(&self, line: String) {
        if line.contains("Removing leading") { return; }  // tar's note about "/" member names
        self.msg_count.fetch_add(1, Ordering::Relaxed);
        let mut m = self.msgs.lock().unwrap_or_else(|e| e.into_inner());
        if m.len() >= 60 { m.pop_front(); }
        m.push_back(line);
    }
    fn tail(&self) -> Vec<String> { self.msgs.lock().unwrap_or_else(|e| e.into_inner()).iter().cloned().collect() }
    fn has(&self, needle: &str) -> bool { self.msgs.lock().unwrap_or_else(|e| e.into_inner()).iter().any(|l| l.contains(needle)) }
}

/// Lines of a child's stream: "tar: …" (or everything, with `names` false) are
/// messages; the rest are member names from -v / -t.
fn reader<R: Read + Send + 'static>(stream: R, p: Arc<Progress>, names: bool) -> std::thread::JoinHandle<()> {
    std::thread::spawn(move || {
        for raw in BufReader::with_capacity(64 * 1024, stream).split(b'\n').flatten() {
            if names && !raw.starts_with(b"tar: ") {
                p.files.fetch_add(1, Ordering::Relaxed);
                if let Ok(mut c) = p.current.try_lock() { *c = String::from_utf8_lossy(&raw[..raw.len().min(300)]).into_owned(); }
            } else if !raw.is_empty() {
                p.message(String::from_utf8_lossy(&raw[..raw.len().min(500)]).into_owned());
            }
        }
    })
}

/// Waits for every child, calling `tick` about once a second. A cancel sends
/// SIGTERM (done by the stdin watcher), then SIGKILL after 5 s.
fn wait_all(children: &mut [Child], mut tick: impl FnMut()) -> Vec<ExitStatus> {
    let mut st: Vec<Option<ExitStatus>> = children.iter().map(|_| None).collect();
    let mut since_tick = Instant::now();
    let mut cancel_at: Option<Instant> = None;
    loop {
        for (i, c) in children.iter_mut().enumerate() {
            if st[i].is_none() { if let Ok(Some(s)) = c.try_wait() { st[i] = Some(s); } }
        }
        if st.iter().all(Option::is_some) { break; }
        if cancelled() {
            let t = *cancel_at.get_or_insert_with(|| { kill_all(libc::SIGTERM); Instant::now() });
            if t.elapsed() > Duration::from_secs(5) { kill_all(libc::SIGKILL); }
        }
        if since_tick.elapsed() >= Duration::from_secs(1) { since_tick = Instant::now(); tick(); }
        std::thread::sleep(Duration::from_millis(100));
    }
    PGIDS.lock().unwrap_or_else(|e| e.into_inner()).clear();
    st.into_iter().map(|s| s.unwrap()).collect()
}

fn fail(msg: impl Into<String>) -> Value {
    json!({"ok": false, "error": msg.into(), "cancelled": cancelled()})
}

// ── small helpers ───────────────────────────────────────────────────────────

struct Stamp { date: String, time: String, iso: String }

fn stamp() -> Stamp {
    let mut tm: libc::tm = unsafe { std::mem::zeroed() };
    let t = unsafe { libc::time(std::ptr::null_mut()) };
    unsafe { libc::localtime_r(&t, &mut tm) };
    let off = tm.tm_gmtoff;
    let date = format!("{:04}-{:02}-{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    Stamp {
        time: format!("{:02}{:02}{:02}", tm.tm_hour, tm.tm_min, tm.tm_sec),
        iso: format!("{date}T{:02}:{:02}:{:02}{}{:02}:{:02}", tm.tm_hour, tm.tm_min, tm.tm_sec,
                     if off < 0 { '-' } else { '+' }, off.abs() / 3600, off.abs() / 60 % 60),
        date,
    }
}

fn uname() -> (String, String) {
    let mut u: libc::utsname = unsafe { std::mem::zeroed() };
    if unsafe { libc::uname(&mut u) } != 0 { return (String::new(), String::new()); }
    let s = |p: &[libc::c_char]| unsafe { std::ffi::CStr::from_ptr(p.as_ptr()) }.to_string_lossy().into_owned();
    (s(&u.nodename), s(&u.release))
}

fn free_bytes(dir: &Path) -> Option<u64> {
    let c = std::ffi::CString::new(dir.as_os_str().as_bytes()).ok()?;
    let mut s: libc::statvfs = unsafe { std::mem::zeroed() };
    (unsafe { libc::statvfs(c.as_ptr(), &mut s) } == 0).then(|| s.f_bavail as u64 * s.f_frsize as u64)
}

fn abs_dir(v: &Value, what: &str) -> Result<PathBuf, String> {
    let s = v.as_str().filter(|s| s.starts_with('/') && s.len() < 1024 && !s.contains('\0'))
        .ok_or_else(|| format!("{what}: an absolute path is required"))?;
    let p = fs::canonicalize(s).map_err(|e| format!("{s}: {e}"))?;
    if p.is_dir() { Ok(p) } else { Err(format!("{}: not a directory", p.display())) }
}

fn abs_file(v: &Value) -> Result<PathBuf, String> {
    let s = v.as_str().filter(|s| s.starts_with('/') && s.len() < 1024 && !s.contains('\0'))
        .ok_or("archive: an absolute path is required")?;
    let p = fs::canonicalize(s).map_err(|e| format!("{s}: {e}"))?;
    if p.is_file() { Ok(p) } else { Err(format!("{}: not a regular file", p.display())) }
}

/// Escapes tar's exclude wildcards in a literal path.
fn literal(p: &Path) -> String {
    let mut o = String::new();
    for c in p.to_string_lossy().chars() { if matches!(c, '*' | '?' | '[' | '\\') { o.push('\\'); } o.push(c); }
    o
}

fn fsync_dir(dir: &Path) {
    if let Ok(d) = fs::OpenOptions::new().read(true).custom_flags(libc::O_DIRECTORY | libc::O_CLOEXEC).open(dir) { let _ = d.sync_all(); }
}

/// 0700 scratch directory `<base>/<prefix>XXXXXX`.
fn mkdtemp(base: &Path, prefix: &str) -> Result<PathBuf, String> {
    let t = base.join(format!("{prefix}XXXXXX"));
    let mut c = std::ffi::CString::new(t.as_os_str().as_bytes()).map_err(|e| e.to_string())?.into_bytes_with_nul();
    if unsafe { libc::mkdtemp(c.as_mut_ptr() as *mut libc::c_char) }.is_null() {
        return Err(format!("{}: {}", t.display(), std::io::Error::last_os_error()));
    }
    c.pop();
    Ok(PathBuf::from(std::ffi::OsString::from_vec(c)))
}

struct Scratch(PathBuf);
impl Drop for Scratch { fn drop(&mut self) { let _ = fs::remove_dir_all(&self.0); } }

// ── compression ─────────────────────────────────────────────────────────────

#[derive(Clone, Copy, PartialEq, Debug)]
enum Comp { Gzip, Zstd, Xz, Bzip2, Age, None }

/// The first bytes of an age file (format v1, the only one).
const AGE_HEADER: &[u8] = b"age-encryption.org/v1";

impl Comp {
    fn from_magic(f: &File) -> Comp {
        use std::os::unix::fs::FileExt;
        let mut m = [0u8; 22];
        let n = f.read_at(&mut m, 0).unwrap_or(0);
        match &m[..n] {
            [0x1f, 0x8b, ..] => Comp::Gzip,
            [0x28, 0xb5, 0x2f, 0xfd, ..] => Comp::Zstd,
            [0xfd, b'7', b'z', b'X', b'Z', 0, ..] => Comp::Xz,
            [b'B', b'Z', b'h', ..] => Comp::Bzip2,
            _ if m[..n].starts_with(AGE_HEADER) => Comp::Age,
            _ => Comp::None,
        }
    }
    /// Decompressor writing to stdout.
    fn decompressor(self) -> Result<(PathBuf, Vec<&'static str>), String> {
        let (names, args): (&[&str], Vec<&str>) = match self {
            Comp::Gzip => (&["pigz", "gzip"], vec!["-dc"]),
            Comp::Zstd => (&["zstd"], vec!["-dc", "-T0"]),
            Comp::Xz => (&["xz"], vec!["-dc", "-T0"]),
            Comp::Bzip2 => (&["pbzip2", "bzip2"], vec!["-dc"]),
            Comp::None | Comp::Age => return Err("not a compressed stream".into()),
        };
        names.iter().find_map(|n| tool(n)).map(|p| (p, args))
            .ok_or_else(|| format!("{} is not installed", names[0]))
    }
}

fn sniff(p: &Path) -> Comp { File::open(p).map(|f| Comp::from_magic(&f)).unwrap_or(Comp::None) }

/// Compression of the tar stream inside an encrypted image. age only encrypts, so the name is the record of
/// what was compressed before it: `backup-….tar.gz.age`, `….tar.zst.age`, `….tar.xz.age`.
fn inner_comp(archive: &Path) -> Result<Comp, String> {
    let n = archive.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    [(".tar.gz.age", Comp::Gzip), (".tar.zst.age", Comp::Zstd), (".tar.xz.age", Comp::Xz)].iter()
        .find(|(e, _)| n.ends_with(e)).map(|(_, c)| *c)
        .ok_or_else(|| "an encrypted image must be named …tar.gz.age, …tar.zst.age or …tar.xz.age".to_string())
}

/// The age recipient of a request: a public key (`age1…`), passed to `age -r` as an argument.
fn recipient_of(req: &Value) -> Result<Option<String>, String> {
    let Some(r) = req["recipient"].as_str().map(str::trim).filter(|s| !s.is_empty()) else { return Ok(None) };
    if r.len() != 62 || !r.starts_with("age1") || !r.bytes().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit()) {
        return Err("the recipient is not an age public key (it starts with age1)".into());
    }
    Ok(Some(r.to_owned()))
}

/// The age secret key of a request ("AGE-SECRET-KEY-1…"), written to a 0600 file in a scratch directory that
/// is removed when the returned guard is dropped. It is never logged and never kept.
fn identity_file(req: &Value) -> Result<Option<(Scratch, PathBuf)>, String> {
    let Some(k) = req["identity"].as_str().map(str::trim).filter(|s| !s.is_empty()) else { return Ok(None) };
    if k.len() > 128 || !k.starts_with("AGE-SECRET-KEY-1") || !k.bytes().all(|c| c.is_ascii_uppercase() || c.is_ascii_digit() || c == b'-') {
        return Err("the key is not an age secret key (AGE-SECRET-KEY-1…)".into());
    }
    let dir = Scratch(mkdtemp(&std::env::temp_dir(), "centurion-key-")?);
    let path = dir.0.join("key");
    let mut f = fs::OpenOptions::new().write(true).create_new(true).mode(0o600)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC).open(&path).map_err(|e| e.to_string())?;
    f.write_all(format!("{k}\n").as_bytes()).map_err(|e| e.to_string())?;
    Ok(Some((dir, path)))
}

// ── system image: create ────────────────────────────────────────────────────

/// An increment is one level above the image it continues. After MAX_LEVEL increments the next image is a full
/// one again, so a chain is at most a full image and a week of dailies.
const MAX_LEVEL: u64 = 6;

/// The newest image in a folder that has a snapshot: the next increment continues from it.
struct Tip { snar: PathBuf, level: u64, chain: String }

fn chain_tip(dir: &Path) -> Option<Tip> {
    let mut best: Option<(SystemTime, Tip)> = None;
    for e in fs::read_dir(dir).ok()?.flatten() {
        let n = e.file_name().to_string_lossy().into_owned();
        let p = e.path();
        if !is_image_name(&n) || !fs::symlink_metadata(snar_of(&p)).is_ok_and(|m| m.is_file()) { continue; }
        let Some(info) = read_info(&p) else { continue };
        let (Some(level), Some(chain)) = (info["level"].as_u64(), info["chain"].as_str()) else { continue };
        let mtime = fs::symlink_metadata(&p).and_then(|m| m.modified()).unwrap_or(SystemTime::UNIX_EPOCH);
        if best.as_ref().map_or(true, |(t, _)| mtime > *t) {
            best = Some((mtime, Tip { snar: snar_of(&p), level, chain: chain.to_owned() }));
        }
    }
    best.map(|(_, t)| t)
}

/// {"op":"system_backup","dest_dir":"/…","compressor":"pigz|zstd|xz","level":6,"threads":0,
///  "include_home":false,"excludes":[…],"low_priority":true,"verify":true,"keep":0,
///  "recipient":"age1…","incremental":false}
pub fn system_backup(req: &Value) -> Value {
    match system_backup_inner(req) { Ok(v) => v, Err(e) => fail(e) }
}

fn system_backup_inner(req: &Value) -> Result<Value, String> {
    let dest = abs_dir(&req["dest_dir"], "dest_dir")?;
    let tar = tool("tar").ok_or("tar is not installed")?;
    let level = req["level"].as_i64();
    let threads = req["threads"].as_u64().filter(|&t| (1..=256).contains(&t))
        .unwrap_or_else(|| std::thread::available_parallelism().map_or(1, |n| n.get() as u64));
    let lvl = |lo: i64, hi: i64, def: i64| level.filter(|l| (lo..=hi).contains(l)).unwrap_or(def);
    let (comp_path, comp_args, ext): (PathBuf, Vec<String>, &str) = match req["compressor"].as_str().unwrap_or("pigz") {
        "pigz" | "gzip" => match tool("pigz") {
            Some(p) => (p, vec![format!("-p{threads}"), format!("-{}", lvl(1, 9, 6))], "gz"),
            None => (tool("gzip").ok_or("neither pigz nor gzip is installed")?, vec![format!("-{}", lvl(1, 9, 6))], "gz"),
        },
        "zstd" => (tool("zstd").ok_or("zstd is not installed")?,
                   vec![format!("-T{threads}"), format!("-{}", lvl(1, 19, 3)), "-q".into()], "zst"),
        "xz" => (tool("xz").ok_or("xz is not installed")?, vec![format!("-T{threads}"), format!("-{}", lvl(0, 9, 6))], "xz"),
        o => return Err(format!("unknown compressor: {o}")),
    };
    let recipient = recipient_of(req)?;
    let age = if recipient.is_some() { Some(tool("age").ok_or("age is not installed (needed to encrypt the image)")?) } else { None };
    let mut excludes: Vec<String> = match req["excludes"].as_array() {
        Some(a) => {
            if a.len() > 128 { return Err("too many exclude patterns".into()); }
            a.iter().map(|v| v.as_str().map(str::trim).filter(|s| !s.is_empty() && s.len() <= 512 && !s.contains(['\0', '\n']))
                .map(str::to_owned).ok_or_else(|| "invalid exclude pattern".to_string())).collect::<Result<_, _>>()?
        }
        None => DEFAULT_EXCLUDES.iter().map(|s| s.to_string()).collect(),
    };
    if !req["include_home"].as_bool().unwrap_or(false) { excludes.push(HOME_EXCLUDE.into()); }
    let low = req["low_priority"].as_bool().unwrap_or(true);

    if free_bytes(&dest).is_some_and(|f| f < 1 << 30) {
        return Err(format!("less than 1 GiB free in {}", dest.display()));
    }
    // An increment continues from the snapshot of the newest image; without one, or past MAX_LEVEL, it is a full image.
    let tip = if req["incremental"].as_bool().unwrap_or(false) { chain_tip(&dest).filter(|t| t.level < MAX_LEVEL) } else { None };
    let level_out = tip.as_ref().map_or(0, |t| t.level + 1);
    let st = stamp();
    let ext_full = format!("tar.{ext}{}", if age.is_some() { ".age" } else { "" });
    let mut stem = if level_out == 0 { format!("backup-{}", st.date) } else { format!("backup-{}_{}-i{level_out}", st.date, st.time) };
    if level_out == 0 && dest.join(format!("{stem}.{ext_full}")).exists() { stem = format!("backup-{}_{}", st.date, st.time); }
    let name = format!("{stem}.{ext_full}");
    let chain = tip.as_ref().map_or_else(|| name.clone(), |t| t.chain.clone());
    let fin = dest.join(&name);
    let partial = dest.join(format!("{name}.partial"));
    let (snar_fin, snar_part) = (snar_of(&fin), dest.join(format!(".{name}.snar.partial")));
    // The archive must not swallow itself, its snapshot or the older images next to it.
    excludes.push(literal(&fin));
    excludes.push(literal(&partial));
    excludes.push(literal(&snar_fin));
    excludes.push(literal(&snar_part));
    excludes.push(format!("{}/backup-*.tar.*", literal(&dest).trim_end_matches('/')));

    let out = fs::OpenOptions::new().write(true).create_new(true).mode(0o600)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC).open(&partial)
        .map_err(|e| format!("{}: {e}", partial.display()))?;
    struct Partial<'a>(&'a Path, bool);
    impl Drop for Partial<'_> { fn drop(&mut self) { if !self.1 { let _ = fs::remove_file(self.0); } } }
    let mut guard = Partial(&partial, false);
    let mut snap = Partial(&snar_part, false);
    if let Some(t) = &tip { fs::copy(&t.snar, &snar_part).map_err(|e| format!("{}: {e}", t.snar.display()))?; }

    let mut tc = command(&tar, low);
    tc.current_dir("/").args(["--acls", "--xattrs", "--xattrs-include=*", "--ignore-failed-read"]);
    // GNU tar's snapshot: absent = a full dump (and it is written); present = only what changed since it.
    tc.arg(format!("--listed-incremental={}", snar_part.display()));
    for e in &excludes { tc.arg(format!("--exclude={e}")); }
    tc.args(["-cvpf", "-", "/"]).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
    let pipe = format!("{} {}{}", comp_path.display(), comp_args.join(" "),
                       recipient.as_deref().map_or(String::new(), |r| format!(" | age -r {r}")));
    crate::emit(&json!({"event": "start", "phase": "backup", "archive": fin, "excludes": excludes,
                        "command": format!("tar --acls --xattrs --xattrs-include='*' --listed-incremental=… --exclude=… -cpf - / | {pipe}")}));
    let mut t = spawn(tc, "tar")?;
    let prog = Arc::new(Progress::default());
    let mut readers = vec![reader(t.stderr.take().unwrap(), prog.clone(), true)];
    let mut cc = command(&comp_path, low);
    cc.args(&comp_args).stdin(Stdio::from(t.stdout.take().unwrap())).stderr(Stdio::piped())
        .stdout(if age.is_some() { Stdio::piped() } else { Stdio::from(out.try_clone().map_err(|e| e.to_string())?) });
    let mut c = match spawn(cc, "compressor") {
        Ok(c) => c,
        Err(e) => { kill_all(libc::SIGKILL); let _ = t.wait(); return Err(e); }
    };
    readers.push(reader(c.stderr.take().unwrap(), prog.clone(), false));
    let mut kids = vec![t, c];
    if let Some(age) = &age {
        let mut ac = command(age, low);
        ac.arg("-r").arg(recipient.as_deref().unwrap_or_default())
            .stdin(Stdio::from(kids[1].stdout.take().unwrap()))
            .stdout(Stdio::from(out.try_clone().map_err(|e| e.to_string())?)).stderr(Stdio::piped());
        let mut a = match spawn(ac, "age") {
            Ok(a) => a,
            Err(e) => { kill_all(libc::SIGKILL); for k in &mut kids { let _ = k.wait(); } return Err(e); }
        };
        readers.push(reader(a.stderr.take().unwrap(), prog.clone(), false));
        kids.push(a);
    }

    let started = Instant::now();
    let status = wait_all(&mut kids, || {
        crate::emit(&json!({"event": "progress", "phase": "backup", "bytes": out.metadata().map_or(0, |m| m.len()),
                            "files": prog.files.load(Ordering::Relaxed), "current": prog.current.lock().unwrap_or_else(|e| e.into_inner()).clone(),
                            "elapsed": started.elapsed().as_secs()}));
    });
    for r in readers { let _ = r.join(); }
    if cancelled() { return Err("cancelled — the partial archive was removed".into()); }
    let last = || prog.tail().last().map(|l| format!(": {l}")).unwrap_or_default();
    let (ts, cs) = (status[0], status[1]);
    if !cs.success() { return Err(format!("the compressor failed ({cs}){}", last())); }
    if let Some(a) = status.get(2).filter(|a| !a.success()) { return Err(format!("age failed ({a}){}", last())); }
    // tar: 0 = clean, 1 = files changed while being read (normal on a live
    // system), 2 = errors on some files — the archive is still usable, flagged.
    let tcode = ts.code().ok_or_else(|| format!("tar was killed ({ts})"))?;
    if tcode > 2 { return Err(format!("tar failed with status {tcode}")); }
    out.sync_all().map_err(|e| format!("{}: {e}", partial.display()))?;
    let bytes = out.metadata().map_or(0, |m| m.len());
    drop(out);
    fs::rename(&partial, &fin).map_err(|e| format!("{}: {e}", fin.display()))?;
    guard.1 = true;
    fsync_dir(&dest);
    // The snapshot lists the system's paths and times: as private as the image (0600 root).
    let _ = fs::set_permissions(&snar_part, fs::Permissions::from_mode(0o600));
    fs::rename(&snar_part, &snar_fin)
        .map_err(|e| format!("{name} was kept, but its snapshot could not be: {e} — the next increment will be a full image"))?;
    snap.1 = true;

    let files = prog.files.load(Ordering::Relaxed);
    let (host, kernel) = uname();
    let mut info = json!({"format": "centurion-system-backup", "version": 1, "created": st.iso,
        "host": host, "kernel": kernel, "archive": name, "bytes": bytes, "files": files,
        "compressor": comp_path.file_name().map(|n| n.to_string_lossy().into_owned()), "excludes": excludes,
        "include_home": req["include_home"].as_bool().unwrap_or(false), "tar_status": tcode,
        "messages": prog.msg_count.load(Ordering::Relaxed),
        "level": level_out, "chain": chain, "snapshot": true, "encrypted": age.is_some(), "recipient": recipient});
    let side = sidecar(&fin);
    let _ = crate::write_root_file(&side, info.to_string().as_bytes());
    crate::wlog::log("backup", &format!("system_backup {} level {level_out}{}", fin.display(), if age.is_some() { " encrypted" } else { "" }));

    // Only the key reads an encrypted image back, so it is verified from the list, with the key.
    let mut verified = Value::Null;
    if req["verify"].as_bool().unwrap_or(false) && age.is_none() {
        match read_archive(&fin, None, low, None, false) {
            Ok(r) if r.files == files => {
                verified = json!(true);
                info["verified"] = json!(stamp().iso);
                let _ = crate::write_root_file(&side, info.to_string().as_bytes());
            }
            Ok(r) => return Err(format!("verification failed: {} entries written, {} read back — {} was kept, do not rely on it",
                                        files, r.files, fin.display())),
            Err(e) => return Err(format!("verification failed: {e} — {} was kept, do not rely on it", fin.display())),
        }
    }
    let removed = prune(&dest, req["keep"].as_u64().unwrap_or(0) as usize, &chain);
    if !removed.is_empty() { crate::wlog::log("backup", &format!("retention removed {}", removed.join(" "))); }
    Ok(json!({"ok": true, "archive": fin, "bytes": bytes, "files": files, "tar_status": tcode,
              "degraded": tcode == 2, "verified": verified, "encrypted": age.is_some(), "level": level_out, "chain": chain,
              "removed": removed, "message_count": prog.msg_count.load(Ordering::Relaxed), "messages": prog.tail(),
              "elapsed": started.elapsed().as_secs()}))
}

/// The root-owned snapshot of an image: GNU tar's listed-incremental file, which the next increment starts from.
fn snar_of(archive: &Path) -> PathBuf { PathBuf::from(format!("{}.snar", archive.display())) }

fn sidecar(archive: &Path) -> String { format!("{}.info.json", archive.display()) }

/// The info sidecar of an image. As root a root-owned, non-symlink sidecar only (see `verify`).
fn read_info(archive: &Path) -> Option<Value> {
    let side = sidecar(archive);
    let text = if is_root() { crate::read_root_file(&side, 1 << 20) } else { fs::read_to_string(&side).ok() };
    text.and_then(|s| serde_json::from_str::<Value>(&s).ok()).filter(Value::is_object)
}

const IMAGE_EXT: &[&str] = &[".tar.gz.age", ".tar.zst.age", ".tar.xz.age", ".tar.gz", ".tar.zst", ".tar.xz"];

/// `backup-YYYY-MM-DD[_HHMMSS][-iN].tar.<ext>[.age]` — the only names retention ever deletes.
fn is_image_name(n: &str) -> bool {
    let Some(rest) = n.strip_prefix("backup-") else { return false };
    let Some(rest) = IMAGE_EXT.iter().find_map(|e| rest.strip_suffix(e)) else { return false };
    match rest.rfind("-i") {
        Some(p) => is_stamp(&rest[..p]) && !rest[p + 2..].is_empty() && rest[p + 2..].bytes().all(|c| c.is_ascii_digit()),
        None => is_stamp(rest),
    }
}

/// `YYYY-MM-DD` or `YYYY-MM-DD_HHMMSS`.
fn is_stamp(s: &str) -> bool {
    let b = s.as_bytes();
    let date = b.len() >= 10 && b[..10].iter().enumerate().all(|(i, c)| if i == 4 || i == 7 { *c == b'-' } else { c.is_ascii_digit() });
    date && (b.len() == 10 || (b.len() == 17 && b[10] == b'_' && b[11..].iter().all(u8::is_ascii_digit)))
}

/// The chain an image belongs to: the name of its full image (a full image is its own chain).
fn chain_of(archive: &Path, name: &str) -> String {
    read_info(archive).and_then(|i| i["chain"].as_str().filter(|c| is_image_name(c)).map(str::to_owned))
        .unwrap_or_else(|| name.to_owned())
}

/// Keeps the newest `keep` chains in `dir` (0 = keep everything). A chain is a full image with its increments
/// and is only ever removed whole, so no kept increment is left without the image it continues from. The chain
/// `kept` (the one just made) is always kept and counts as one of them.
fn prune(dir: &Path, keep: usize, kept: &str) -> Vec<String> {
    if keep == 0 { return Vec::new(); }
    // chain → (newest modification time, its images)
    let mut chains: BTreeMap<String, (SystemTime, Vec<String>)> = BTreeMap::new();
    for e in fs::read_dir(dir).into_iter().flatten().flatten() {
        let n = e.file_name().to_string_lossy().into_owned();
        let p = e.path();
        let Ok(m) = fs::symlink_metadata(&p) else { continue };
        if !(m.is_file() && is_image_name(&n)) { continue; }
        let c = chains.entry(chain_of(&p, &n)).or_insert((SystemTime::UNIX_EPOCH, Vec::new()));
        c.0 = c.0.max(m.modified().unwrap_or(SystemTime::UNIX_EPOCH));
        c.1.push(n);
    }
    let mut others: Vec<(SystemTime, String)> = chains.iter().filter(|(c, _)| c.as_str() != kept)
        .map(|(c, v)| (v.0, c.clone())).collect();
    others.sort_by_key(|(t, _)| std::cmp::Reverse(*t));
    let mut removed = Vec::new();
    for (_, c) in others.into_iter().skip(keep.saturating_sub(1)) {
        for n in &chains[&c].1 {
            let p = dir.join(n);
            if fs::remove_file(&p).is_ok() {
                let _ = fs::remove_file(sidecar(&p));
                let _ = fs::remove_file(snar_of(&p));
                removed.push(n.clone());
            }
        }
    }
    removed
}

// ── system image: verify / restore ──────────────────────────────────────────

struct ReadResult { files: u64, tar_status: i32, prog: Arc<Progress>, elapsed: u64 }

/// `[age -d] | <decompressor> | tar -t` (target None) or `… | tar -xp -C target`. Compression comes from the
/// file's magic bytes; an encrypted image is decrypted with the age identity file `key` first, and its inner
/// compression is read from its name. `incremental` unpacks an increment: GNU tar's listed-incremental with an
/// empty snapshot, so the deletions an increment records are applied.
fn read_archive(archive: &Path, target: Option<&Path>, low: bool, key: Option<&Path>, incremental: bool) -> Result<ReadResult, String> {
    let tar = tool("tar").ok_or("tar is not installed")?;
    let file = File::open(archive).map_err(|e| format!("{}: {e}", archive.display()))?;
    let total = file.metadata().map_or(0, |m| m.len());
    let comp = Comp::from_magic(&file);
    let phase = if target.is_some() { "restore" } else { "verify" };
    let prog = Arc::new(Progress::default());
    // The stages between the file and tar, in order: [age -d], then the decompressor.
    let mut stages: Vec<(PathBuf, Vec<String>, Comp)> = Vec::new();
    let inner = if comp == Comp::Age {
        let key = key.ok_or("the archive is encrypted: its age key is needed to read it")?;
        let age = tool("age").ok_or("age is not installed (needed to read an encrypted image)")?;
        stages.push((age, vec!["-d".into(), "-i".into(), key.display().to_string()], Comp::Age));
        inner_comp(archive)?
    } else {
        comp
    };
    if inner != Comp::None {
        let (path, args) = inner.decompressor()?;
        stages.push((path, args.into_iter().map(str::to_owned).collect(), inner));
    }
    let mut kids: Vec<Child> = Vec::new();
    let mut readers = Vec::new();
    // The children read through a duplicate of `file`: one shared offset, so
    // our own descriptor tells how far into the archive they are.
    let mut input = Stdio::from(file.try_clone().map_err(|e| e.to_string())?);
    for (path, args, _) in &stages {
        let mut c = command(path, low);
        c.args(args).stdin(input).stdout(Stdio::piped()).stderr(Stdio::piped());
        let mut ch = spawn(c, "decompressor")?;
        readers.push(reader(ch.stderr.take().unwrap(), prog.clone(), false));
        input = Stdio::from(ch.stdout.take().unwrap());
        kids.push(ch);
    }
    let mut tc = command(&tar, low);
    match target {
        Some(t) => {
            tc.args(["--acls", "--xattrs", "--xattrs-include=*", "--numeric-owner", "-xvpf", "-"]);
            if incremental { tc.arg("--listed-incremental=/dev/null"); }
            tc.arg("-C").arg(t);
        }
        None => { tc.args(["-tf", "-"]); }
    }
    tc.stdin(input).stdout(Stdio::piped()).stderr(Stdio::piped());
    let mut t = match spawn(tc, "tar") {
        Ok(t) => t,
        Err(e) => { kill_all(libc::SIGKILL); for k in &mut kids { let _ = k.wait(); } return Err(e); }
    };
    readers.push(reader(t.stdout.take().unwrap(), prog.clone(), true));
    readers.push(reader(t.stderr.take().unwrap(), prog.clone(), false));
    kids.push(t);

    let started = Instant::now();
    let fd = file.as_raw_fd();
    let status = wait_all(&mut kids, || {
        let pos = unsafe { libc::lseek(fd, 0, libc::SEEK_CUR) }.max(0) as u64;
        crate::emit(&json!({"event": "progress", "phase": phase, "bytes": pos, "total": total,
                            "files": prog.files.load(Ordering::Relaxed), "current": prog.current.lock().unwrap_or_else(|e| e.into_inner()).clone(),
                            "elapsed": started.elapsed().as_secs()}));
    });
    for r in readers { let _ = r.join(); }
    if cancelled() { return Err("cancelled".into()); }
    let last = || prog.tail().last().map(|l| format!(": {l}")).unwrap_or_default();
    let ts = *status.last().unwrap();
    let tcode = ts.code().ok_or_else(|| format!("tar was killed ({ts})"))?;
    for (i, (_, _, kind)) in stages.iter().enumerate() {
        let ds = status[i];
        if *kind == Comp::Age && !ds.success() {
            return Err(if prog.has("no identity matched") { "this key does not open the archive".to_string() }
                       else { format!("age could not decrypt the archive ({ds}){}", last()) });
        }
        // gzip exits 2 for warnings; a decompressor cut off by SIGPIPE after tar
        // finished cleanly had only padding left to write.
        let fine = ds.success() || (*kind == Comp::Gzip && ds.code() == Some(2)) || (ds.signal() == Some(libc::SIGPIPE) && tcode == 0);
        if !fine { return Err(format!("the archive is damaged or truncated (decompressor: {ds}){}", last())); }
    }
    if prog.has("Unexpected EOF") || prog.has("does not look like a tar archive") {
        return Err(format!("the archive is damaged or truncated{}", last()));
    }
    if tcode != 0 && (target.is_none() || tcode != 2) { return Err(format!("tar failed with status {tcode}{}", last())); }
    Ok(ReadResult { files: prog.files.load(Ordering::Relaxed), tar_status: tcode, prog, elapsed: started.elapsed().as_secs() })
}

/// {"op":"verify","archive":"/…"} or {"op":"verify","archives":["/…", …]} — reads whole archives back (compressed
/// stream + tar structure) and compares the entry count with the one recorded when each was made. An encrypted
/// image needs its key: {"identity":"AGE-SECRET-KEY-1…"}. Several archives: one result per archive.
pub fn verify(req: &Value) -> Value {
    let key = match identity_file(req) { Ok(k) => k, Err(e) => return fail(e) };
    let key_path = key.as_ref().map(|(_, p)| p.as_path());
    let low = req["low_priority"].as_bool().unwrap_or(true);
    if let Some(list) = req["archives"].as_array() {
        if list.is_empty() || list.len() > 64 { return fail("choose between 1 and 64 archives"); }
        let results: Vec<Value> = list.iter().map(|a| verify_one(a, key_path, low)).collect();
        return json!({"ok": results.iter().all(|r| r["ok"] == json!(true)), "results": results});
    }
    verify_one(&req["archive"], key_path, low)
}

fn verify_one(archive_v: &Value, key: Option<&Path>, low: bool) -> Value {
    let archive = match abs_file(archive_v) { Ok(a) => a, Err(e) => return fail(e) };
    if key.is_none() && sniff(&archive) == Comp::Age {
        return json!({"ok": false, "needs_key": true, "archive": archive, "error": "the archive is encrypted: its age key is needed"});
    }
    crate::emit(&json!({"event": "start", "phase": "verify", "archive": archive}));
    let r = match read_archive(&archive, None, low, key, false) { Ok(r) => r, Err(e) => return fail(e) };
    let side = sidecar(&archive);
    // As root only a root-owned, non-symlink sidecar counts (the archive's directory may be the
    // user's: a link there must not make root read, and then copy, another root file).
    let text = if is_root() { crate::read_root_file(&side, 1 << 20) } else { fs::read_to_string(&side).ok() };
    let mut info = text.and_then(|s| serde_json::from_str::<Value>(&s).ok()).filter(Value::is_object);
    if let Some(expect) = info.as_ref().and_then(|i| i["files"].as_u64()) {
        if expect != r.files { return fail(format!("{} entries recorded when the backup was made, {} read back", expect, r.files)); }
    }
    if let Some(i) = info.as_mut().filter(|_| is_root()) {
        i["verified"] = json!(stamp().iso);
        let _ = crate::write_root_file(&side, i.to_string().as_bytes());
    }
    json!({"ok": true, "archive": archive, "files": r.files, "compared": info.is_some(), "elapsed": r.elapsed})
}

/// The archives to unpack, in order, to restore `archive`: its full image, then every increment up to it.
/// The flag marks an increment. A full image is restored alone.
fn restore_sequence(archive: &Path) -> Result<Vec<(PathBuf, bool)>, String> {
    let info = read_info(archive).unwrap_or(Value::Null);
    let level = info["level"].as_u64().unwrap_or(0);
    if level == 0 { return Ok(vec![(archive.to_path_buf(), false)]); }
    if level > MAX_LEVEL { return Err(format!("{}: increment {level} is beyond the chain limit", archive.display())); }
    let chain = info["chain"].as_str().filter(|c| is_image_name(c))
        .ok_or("this increment does not name its full image: restore the full image instead")?.to_owned();
    let dir = archive.parent().ok_or("the archive has no folder")?;
    let mut found: Vec<Option<PathBuf>> = vec![None; level as usize + 1];
    for e in fs::read_dir(dir).map_err(|e| format!("{}: {e}", dir.display()))?.flatten() {
        let n = e.file_name().to_string_lossy().into_owned();
        let p = e.path();
        if !is_image_name(&n) { continue; }
        let Some(inf) = read_info(&p) else { continue };
        if inf["chain"].as_str() != Some(chain.as_str()) { continue; }
        if let Some(l) = inf["level"].as_u64().filter(|&l| l <= level) {
            if found[l as usize].is_none() { found[l as usize] = Some(p); }
        }
    }
    found.into_iter().enumerate()
        .map(|(l, p)| p.map(|p| (p, l > 0)).ok_or_else(|| format!("increment {l} of this chain is missing from {}", dir.display())))
        .collect()
}

/// {"op":"system_restore","archive":"/…","target":"/mnt/gentoo","confirm":"/mnt/gentoo","identity":"AGE-SECRET-KEY-1…"}
/// `confirm` must repeat the target: a request cannot restore somewhere by accident.
pub fn system_restore(req: &Value) -> Value {
    let (archive, target) = match (abs_file(&req["archive"]), abs_dir(&req["target"], "target")) {
        (Ok(a), Ok(t)) => (a, t),
        (Err(e), _) | (_, Err(e)) => return fail(e),
    };
    if req["confirm"].as_str().map(Path::new) != Some(target.as_path()) {
        return fail(format!("confirm must repeat the resolved target ({})", target.display()));
    }
    let key = match identity_file(req) { Ok(k) => k, Err(e) => return fail(e) };
    let key_path = key.as_ref().map(|(_, p)| p.as_path());
    if key_path.is_none() && sniff(&archive) == Comp::Age {
        return json!({"ok": false, "needs_key": true, "archive": archive, "error": "the archive is encrypted: its age key is needed"});
    }
    let seq = match restore_sequence(&archive) { Ok(s) => s, Err(e) => return fail(e) };
    crate::wlog::log("backup", &format!("system_restore {} -> {}", archive.display(), target.display()));
    crate::emit(&json!({"event": "start", "phase": "restore", "archive": archive, "target": target,
                        "command": format!("tar --acls --xattrs --xattrs-include='*' --numeric-owner -xpf {} -C {}", archive.display(), target.display())}));
    let low = req["low_priority"].as_bool().unwrap_or(false);
    let (mut files, mut elapsed, mut degraded) = (0u64, 0u64, false);
    let mut messages: Vec<String> = Vec::new();
    let mut message_count = 0u64;
    for (file, incremental) in &seq {
        let r = match read_archive(file, Some(&target), low, key_path, *incremental) {
            Ok(r) => r,
            Err(e) => return fail(if seq.len() > 1 { format!("{}: {e}", file.display()) } else { e }),
        };
        files += r.files;
        elapsed += r.elapsed;
        degraded |= r.tar_status != 0;
        message_count += r.prog.msg_count.load(Ordering::Relaxed);
        messages = r.prog.tail();
    }
    let mut created = Vec::new();
    for (d, mode) in MOUNT_POINTS {
        let p = target.join(d);
        if fs::symlink_metadata(&p).is_err() && fs::create_dir(&p).is_ok() {
            let _ = fs::set_permissions(&p, fs::Permissions::from_mode(*mode));
            created.push(format!("/{d}"));
        }
    }
    unsafe { libc::sync(); }
    json!({"ok": true, "archive": archive, "target": target, "files": files, "steps": seq.len(),
           "degraded": degraded, "created_dirs": created,
           "message_count": message_count, "messages": messages, "elapsed": elapsed})
}

// ── Centurion configuration backup ────────────────────────────────────────────────

fn xdg(var: &str, fallback: &str) -> Option<PathBuf> {
    std::env::var_os(var).map(PathBuf::from).filter(|p| p.is_absolute())
        .or_else(|| std::env::var_os("HOME").map(PathBuf::from).filter(|p| p.is_absolute()).map(|h| h.join(fallback)))
}
/// Directories under $XDG_CONFIG_HOME that hold Centurion data (the Ryzen tab shares
/// its profile folder with the standalone Curve Optimizer applet).
const USER_CONFIG_DIRS: &[&str] = &["centurion", "ryzen-curve-optimizer"];
fn config_home() -> Option<PathBuf> { xdg("XDG_CONFIG_HOME", ".config") }
fn user_state() -> Option<PathBuf> { xdg("XDG_STATE_HOME", ".local/state").map(|p| p.join("centurion")) }

#[derive(Default)]
struct Copier { files: u64, bytes: u64, skipped: Vec<String>, links: bool }

impl Copier {
    /// Copies a file or tree. The path named by the caller is followed if it
    /// is a symlink (/usr/src/linux); links inside a tree are copied as links
    /// (`links`) or left out. Unreadable or oversized entries are listed.
    fn copy(&mut self, src: &Path, dst: &Path, depth: u32) {
        if PROFILE_SKIP.iter().any(|p| src == Path::new(p)) { return; }
        let md = match if depth == 0 { fs::metadata(src) } else { fs::symlink_metadata(src) } {
            Ok(m) => m,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => return,
            Err(e) => return self.skip(src, &e.to_string()),
        };
        let ft = md.file_type();
        if ft.is_symlink() {
            if self.links { if let Ok(t) = fs::read_link(src) { let _ = std::os::unix::fs::symlink(t, dst); } }
        } else if ft.is_dir() {
            if depth > 16 { return self.skip(src, "too deep"); }
            let rd = match fs::read_dir(src) { Ok(r) => r, Err(e) => return self.skip(src, &e.to_string()) };
            if fs::create_dir_all(dst).is_err() { return self.skip(src, "cannot stage"); }
            let mut names: Vec<_> = rd.flatten().map(|e| e.file_name()).collect();
            names.sort();
            for n in names { self.copy(&src.join(&n), &dst.join(&n), depth + 1); }
        } else if ft.is_file() {
            if md.len() > MAX_FILE || self.bytes + md.len() > MAX_STAGE { return self.skip(src, "too large"); }
            if let Some(p) = dst.parent() { let _ = fs::create_dir_all(p); }
            match fs::copy(src, dst) {
                Ok(n) => { self.files += 1; self.bytes += n; }
                Err(e) => self.skip(src, &e.to_string()),
            }
        }
    }
    fn skip(&mut self, p: &Path, why: &str) {
        if self.skipped.len() < 200 { self.skipped.push(format!("{} ({why})", p.display())); }
    }
}

fn write_text(p: &Path, body: &str) { if let Some(d) = p.parent() { let _ = fs::create_dir_all(d); } let _ = fs::write(p, body); }

/// Generated part of the system profile: facts no config file holds.
fn profile_facts(dir: &Path) {
    let (host, kernel) = uname();
    write_text(&dir.join("uname.txt"), &format!("host: {host}\nkernel: {kernel}\n"));
    for (name, src) in [("cmdline.txt", "/proc/cmdline"), ("mounts.txt", "/proc/mounts"), ("partitions.txt", "/proc/partitions"),
                        ("swaps.txt", "/proc/swaps")] {
        if let Ok(s) = fs::read_to_string(src) { write_text(&dir.join(name), &s); }
    }
    write_text(&dir.join("cpu.txt"), crate::cpuinfo_head());
    let dmi: String = ["sys_vendor", "product_name", "product_version", "product_family", "board_name", "bios_version",
                       "bios_date", "ec_firmware_release"].iter()
        .filter_map(|f| crate::read_trimmed(&Path::new("/sys/class/dmi/id").join(f)).ok().map(|v| format!("{f}: {v}\n"))).collect();
    write_text(&dir.join("dmi.txt"), &dmi);
    if let Ok(m) = fs::read_to_string("/proc/modules") {
        let mut names: Vec<&str> = m.lines().filter_map(|l| l.split_whitespace().next()).collect();
        names.sort_unstable();
        write_text(&dir.join("modules.txt"), &(names.join("\n") + "\n"));
    }
    let _ = fs::copy("/proc/config.gz", dir.join("running-kernel-config.gz"));
    // Installed packages (Portage VDB): category/name-version.
    let mut pkgs = Vec::new();
    for cat in fs::read_dir("/var/db/pkg").into_iter().flatten().flatten() {
        for p in fs::read_dir(cat.path()).into_iter().flatten().flatten() {
            pkgs.push(format!("{}/{}", cat.file_name().to_string_lossy(), p.file_name().to_string_lossy()));
        }
    }
    if !pkgs.is_empty() { pkgs.sort(); write_text(&dir.join("packages.txt"), &(pkgs.join("\n") + "\n")); }
}

fn safe_tag(v: &Value) -> Option<&str> {
    v.as_str().filter(|t| !t.is_empty() && t.len() <= 24 && t.bytes().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == b'-'))
}

/// Copies taken right before a firmware-persistent write (GUI: prewrite). Only these are pruned.
const PRE_WRITE_TAGS: &[&str] = &["pre-memtiming", "pre-biosoc", "pre-gpumux", "pre-igpu"];
const PRE_WRITE_KEEP: usize = 5;

fn is_pre_write_name(n: &str) -> bool {
    n.starts_with("centurion-config-") && n.ends_with(".tar.gz") && PRE_WRITE_TAGS.iter().any(|t| n.ends_with(&format!("-{t}.tar.gz")))
}

/// The pre-write copies in `names` that fall outside the newest `keep` (oldest first). The names start
/// with the date and time, so sorting them sorts by age. Every other file is left alone.
fn pre_write_victims(mut names: Vec<String>, keep: usize) -> Vec<String> {
    names.retain(|n| is_pre_write_name(n));
    names.sort_unstable();
    let outside = names.len().saturating_sub(keep);
    names.truncate(outside);
    names
}

fn prune_pre_write(dir: &Path, keep: usize) -> Vec<String> {
    let names: Vec<String> = fs::read_dir(dir).into_iter().flatten().flatten().filter_map(|e| e.file_name().into_string().ok()).collect();
    pre_write_victims(names, keep).into_iter().filter(|n| fs::remove_file(dir.join(n)).is_ok()).collect()
}

/// {"op":"config_backup","dest_dir":"/…","include_profile":true,"include_logs":false,"tag":"pre-restore","centurion_version":"2.0.0"}
/// Runs as the user: only readable files are saved; the rest is listed in `skipped`.
pub fn config_backup(req: &Value) -> Value {
    match config_backup_inner(req) { Ok(v) => v, Err(e) => fail(e) }
}

fn config_backup_inner(req: &Value) -> Result<Value, String> {
    if is_root() { return Err("config_backup runs as the user, not through pkexec".into()); }
    let d = req["dest_dir"].as_str().filter(|s| s.starts_with('/') && s.len() < 1024).ok_or("dest_dir: an absolute path is required")?;
    fs::create_dir_all(d).map_err(|e| format!("{d}: {e}"))?;
    let dest = abs_dir(&req["dest_dir"], "dest_dir")?;
    let tar = tool("tar").ok_or("tar is not installed")?;
    let stage = Scratch(mkdtemp(&std::env::temp_dir(), "centurion-backup-")?);
    let s = &stage.0;
    let logs = req["include_logs"].as_bool().unwrap_or(false);

    let mut user = Copier::default();
    if let Some(c) = config_home() {
        for d in USER_CONFIG_DIRS { user.copy(&c.join(d), &s.join("user/config").join(d), 0); }
    }
    let mut state = Copier::default();
    if logs { if let Some(c) = user_state() { state.copy(&c, &s.join("user/state/centurion"), 0); } }
    let mut sys = Copier::default();
    for p in SYS_CENTURION.iter().chain(if logs { SYS_CENTURION_LOGS } else { &[] }) {
        sys.copy(&Path::new("/").join(p), &s.join("system").join(p), 0);
    }
    let mut prof = Copier { links: true, ..Default::default() };
    if req["include_profile"].as_bool().unwrap_or(true) {
        for p in PROFILE { prof.copy(&Path::new("/").join(p), &s.join("system-profile/root").join(p), 0); }
        profile_facts(&s.join("system-profile"));
    }
    if user.files + sys.files + prof.files == 0 { return Err("nothing to back up yet".into()); }

    let st = stamp();
    let (host, kernel) = uname();
    let dmi = |f: &str| crate::read_trimmed(&Path::new("/sys/class/dmi/id").join(f)).unwrap_or_default();
    let skipped: Vec<String> = [&user, &state, &sys, &prof].iter().flat_map(|c| c.skipped.iter().cloned()).collect();
    let manifest = json!({"format": CONFIG_FORMAT, "version": 1, "created": st.iso, "host": host,
        "centurion_version": req["centurion_version"].as_str().filter(|v| v.len() <= 32).unwrap_or(""),
        "machine": {"product": dmi("product_version"), "bios": dmi("bios_version"), "kernel": kernel},
        "contents": {"user_config": user.files, "user_state": state.files, "system": sys.files, "system_profile": prof.files},
        "skipped": skipped});
    fs::write(s.join(MANIFEST), serde_json::to_vec_pretty(&manifest).unwrap_or_default()).map_err(|e| e.to_string())?;

    let tag = safe_tag(&req["tag"]).map(|t| format!("-{t}")).unwrap_or_default();
    let fin = dest.join(format!("centurion-config-{}_{}{tag}.tar.gz", st.date, st.time));
    let partial = dest.join(format!(".{}.partial", fin.file_name().unwrap().to_string_lossy()));
    let mut tc = command(&tar, false);
    tc.arg("-czf").arg(&partial).arg("-C").arg(s).arg(MANIFEST);
    for top in ["user", "system", "system-profile"] { if s.join(top).exists() { tc.arg(top); } }
    let o = tc.stdin(Stdio::null()).output().map_err(|e| format!("tar: {e}"))?;
    if !o.status.success() {
        let _ = fs::remove_file(&partial);
        return Err(format!("tar failed: {}", String::from_utf8_lossy(&o.stderr).trim()));
    }
    let _ = fs::set_permissions(&partial, fs::Permissions::from_mode(0o600));
    fs::rename(&partial, &fin).map_err(|e| { let _ = fs::remove_file(&partial); format!("{}: {e}", fin.display()) })?;
    let removed = match safe_tag(&req["tag"]) {
        Some(t) if PRE_WRITE_TAGS.contains(&t) => prune_pre_write(&dest, PRE_WRITE_KEEP),
        _ => Vec::new(),
    };
    Ok(json!({"ok": true, "archive": fin, "bytes": fs::metadata(&fin).map_or(0, |m| m.len()),
              "contents": manifest["contents"], "skipped": manifest["skipped"], "removed": removed}))
}

fn read_manifest(tar: &Path, archive: &Path) -> Result<Value, String> {
    let o = command(tar, false).arg("-xOf").arg(archive).arg(MANIFEST).stdin(Stdio::null()).output().map_err(|e| format!("tar: {e}"))?;
    let m: Value = serde_json::from_slice(&o.stdout[..o.stdout.len().min(1 << 20)]).map_err(|_| "not a Centurion configuration backup")?;
    if m["format"] != CONFIG_FORMAT || m["version"] != 1 { return Err("not a Centurion configuration backup (or a newer format)".into()); }
    Ok(m)
}

/// {"op":"config_inspect","archive":"/…"} → {"ok":true,"manifest":{…}}
pub fn config_inspect(req: &Value) -> Value {
    let r = (|| Ok::<_, String>(read_manifest(&tool("tar").ok_or("tar is not installed")?, &abs_file(&req["archive"])?)?))();
    match r { Ok(m) => json!({"ok": true, "manifest": m}), Err(e) => fail(e) }
}

/// Extracts one top-level directory of a configuration backup into a 0700
/// scratch directory; ownership and modes from the archive are not applied.
fn unpack(archive: &Path, top: &str, base: &Path) -> Result<Option<Scratch>, String> {
    let tar = tool("tar").ok_or("tar is not installed")?;
    let m = read_manifest(&tar, archive)?;
    let key = if top == "user" { "user_config" } else { "system" };
    if m["contents"][key].as_u64().unwrap_or(0) == 0 { return Ok(None); }
    let stage = Scratch(mkdtemp(base, ".centurion-restore-")?);
    let o = command(&tar, false).arg("-xf").arg(archive).arg("-C").arg(&stage.0)
        .args(["--no-same-owner", "--no-same-permissions", top]).stdin(Stdio::null()).output().map_err(|e| format!("tar: {e}"))?;
    if !o.status.success() { return Err(format!("tar failed: {}", String::from_utf8_lossy(&o.stderr).trim())); }
    plain_tree(&stage.0, 0)?;
    Ok(Some(stage))
}

/// The archive is not trusted: a backup holds directories and regular files only. A symbolic link in it
/// (e.g. system/etc/centurion -> /etc) would make the restore read through it — as root that
/// copied any file of the link's target, world-readable, into Centurion's directories. Hard links (a second name
/// for a file elsewhere on the same filesystem) and special files are refused the same way. The stage
/// directory is 0700 and owned by the caller, so nothing can be swapped in after this walk.
fn plain_tree(dir: &Path, depth: usize) -> Result<(), String> {
    if depth > 16 { return Err("the backup nests directories too deeply; refusing".into()); }
    for e in fs::read_dir(dir).map_err(|e| format!("{}: {e}", dir.display()))? {
        let e = e.map_err(|e| e.to_string())?;
        let md = fs::symlink_metadata(e.path()).map_err(|e| e.to_string())?;
        let ft = md.file_type();
        if ft.is_dir() { plain_tree(&e.path(), depth + 1)?; }
        else if !ft.is_file() || md.nlink() != 1 {
            return Err(format!("the backup contains a link or special file ({}); refusing to restore from it",
                               e.file_name().to_string_lossy()));
        }
    }
    Ok(())
}

/// {"op":"config_restore_user","archive":"/…"} — as the user: the saved
/// ~/.config/{centurion,ryzen-curve-optimizer} files overwrite the
/// current ones; files that are not in the backup stay.
pub fn config_restore_user(req: &Value) -> Value {
    let r: Result<Value, String> = (|| {
        if is_root() { return Err("config_restore_user runs as the user".to_string()); }
        let archive = abs_file(&req["archive"])?;
        let Some(stage) = unpack(&archive, "user", &std::env::temp_dir())? else { return Ok(json!({"ok": true, "files": 0})) };
        let dst = config_home().ok_or("no home directory")?;
        // A scene's "command" step runs a program at the next scene switch: one that arrives from
        // an archive is dropped (the user re-enters it in the scene editor if it is theirs).
        let dropped = strip_scene_commands(&stage.0.join("user/config/centurion/scenes"));
        let mut c = Copier::default();
        for d in USER_CONFIG_DIRS { c.copy(&stage.0.join("user/config").join(d), &dst.join(d), 0); }
        Ok(json!({"ok": true, "files": c.files, "dir": dst, "skipped": c.skipped, "commands_dropped": dropped}))
    })();
    r.unwrap_or_else(fail)
}

/// Removes the "command" field from the staged scene files; returns the scenes that had one.
/// A scene with a command that cannot be rewritten is removed from the stage, not restored as is.
fn strip_scene_commands(dir: &Path) -> Vec<String> {
    let mut out = Vec::new();
    if !fs::symlink_metadata(dir).map_or(false, |m| m.is_dir()) { return out; }
    for e in fs::read_dir(dir).into_iter().flatten().flatten() {
        let p = e.path();
        if !fs::symlink_metadata(&p).map_or(false, |m| m.is_file()) { continue; }
        let Ok(text) = fs::read_to_string(&p) else { let _ = fs::remove_file(&p); continue };
        let Ok(mut v) = serde_json::from_str::<Value>(&text) else { continue };
        let Some(o) = v.as_object_mut() else { continue };
        if o.remove("command").is_some() {
            let ok = serde_json::to_vec_pretty(&v).ok().map_or(false, |b| fs::write(&p, b).is_ok());
            if !ok { let _ = fs::remove_file(&p); }
            out.push(e.file_name().to_string_lossy().trim_end_matches(".json").to_owned());
        }
    }
    out
}

fn safe_name(n: &std::ffi::OsStr) -> Option<&str> {
    n.to_str().filter(|s| !s.is_empty() && s.len() <= 128 && !s.starts_with('.')
        && s.chars().all(|c| c.is_ascii_alphanumeric() || " _.()+-".contains(c)))
}

/// Installs the regular files of `src` (no symlinks, safe names) into the
/// root-owned directory `dst`, each root:root 0644 through an atomic write.
fn install_dir(src: &Path, dst: &str, only: Option<&[&str]>, done: &mut Vec<String>) -> Result<(), String> {
    // `src` itself must be a real directory (never a link to one); files are opened without following links.
    if !fs::symlink_metadata(src).map_or(false, |m| m.is_dir()) { return Ok(()); }
    let Ok(rd) = fs::read_dir(src) else { return Ok(()) };
    let mut made = false;
    for e in rd.flatten() {
        let Some(name) = e.file_name().to_str().map(str::to_owned) else { continue };
        if safe_name(&e.file_name()).is_none() || only.is_some_and(|o| !o.contains(&name.as_str())) { continue; }
        let Ok(md) = fs::symlink_metadata(e.path()) else { continue };
        if !md.is_file() || md.len() > MAX_ROOT_FILE { continue; }
        if !made { crate::secure_dir(dst)?; made = true; }
        let body = read_nofollow(&e.path())?;
        // Every restorable config file is JSON (the presets directory only holds presets): the consumers parse
        // them again at use, but a file that is not even JSON is dropped here instead of being installed.
        if (dst.ends_with("/presets") && !name.ends_with(".json"))
            || (name.ends_with(".json") && serde_json::from_slice::<Value>(&body).is_err()) { continue; }
        let to = format!("{dst}/{name}");
        crate::write_root_file(&to, &body)?;
        done.push(to);
    }
    Ok(())
}

fn read_nofollow(p: &Path) -> Result<Vec<u8>, String> {
    let f = fs::OpenOptions::new().read(true).custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC | libc::O_NONBLOCK).open(p)
        .map_err(|e| format!("{}: {e}", p.display()))?;
    let md = f.metadata().map_err(|e| e.to_string())?;
    if !md.is_file() || md.len() > MAX_ROOT_FILE { return Err(format!("{}: not a regular file", p.display())); }
    let mut body = Vec::new();
    (&f).take(MAX_ROOT_FILE + 1).read_to_end(&mut body).map_err(|e| e.to_string())?;
    Ok(body)
}

/// {"op":"config_restore_root","archive":"/…"} — root: presets, boot profiles,
/// network guard rules, NVIDIA curve profiles, calibration data and Centurion's
/// NVIDIA module options. Whitelisted paths only.
pub fn config_restore_root(req: &Value) -> Value {
    let r: Result<Value, String> = (|| {
        let archive = abs_file(&req["archive"])?;
        crate::secure_dir("/var/lib/centurion")?;
        let Some(stage) = unpack(&archive, "system", Path::new("/var/lib/centurion"))? else {
            return Ok(json!({"ok": true, "files": 0, "restored": []}));
        };
        let s = stage.0.join("system");
        let mut done = Vec::new();
        crate::secure_dir("/etc/centurion")?;
        install_dir(&s.join("etc/centurion"), "/etc/centurion", Some(ETC_RESTORE), &mut done)?;
        install_dir(&s.join("etc/centurion/presets"), "/etc/centurion/presets", None, &mut done)?;
        if s.join("etc/nvcurve").is_dir() {
            crate::secure_dir("/etc/nvcurve")?;
            install_dir(&s.join("etc/nvcurve"), "/etc/nvcurve", Some(&["config.json"]), &mut done)?;
            install_dir(&s.join("etc/nvcurve/profiles"), "/etc/nvcurve/profiles", None, &mut done)?;
        }
        install_dir(&s.join("var/lib/centurion"), "/var/lib/centurion", Some(VARLIB_RESTORE), &mut done)?;
        // modprobe.d is executable configuration (an `install` line runs a command as root at module load):
        // the saved file is never copied. Its managed options are validated and the file is written anew.
        let conf = s.join("etc/modprobe.d/zz-centurion-nvidia.conf");
        if fs::symlink_metadata(&conf).map_or(false, |m| m.is_file()) {
            let text = String::from_utf8_lossy(&read_nofollow(&conf)?).into_owned();
            if crate::nvreg::restore_from(&text)? > 0 { done.push(crate::nvreg::CENTURION_FILE.to_owned()); }
        }
        crate::wlog::log("backup", &format!("config_restore_root {} ({} files)", archive.display(), done.len()));
        if done.iter().any(|p| p.ends_with("/netguard.json")) {
            if let Some(p) = crate::netguard::daemon_pid() { unsafe { libc::kill(p, libc::SIGHUP); } }
        }
        Ok(json!({"ok": true, "files": done.len(), "restored": done}))
    })();
    r.unwrap_or_else(fail)
}

// ── configuration backups: what changed between two ─────────────────────────────

/// Longest list of files / keys a diff reports; the rest is counted as truncated.
const DIFF_MAX: usize = 200;

/// {"op":"config_diff","a":"/…","b":"/…"} — what changed between two configuration archives: files added,
/// removed and changed, and for JSON and .ini files the keys that changed. Reads only, as the user; restores
/// nothing. The system profile is not compared (it is reference material, see config_backup).
pub fn config_diff(req: &Value) -> Value {
    let r: Result<Value, String> = (|| {
        if is_root() { return Err("config_diff runs as the user, not through pkexec".to_string()); }
        let tar = tool("tar").ok_or("tar is not installed")?;
        let (a, b) = (abs_file(&req["a"])?, abs_file(&req["b"])?);
        let (ma, mb) = (read_manifest(&tar, &a)?, read_manifest(&tar, &b)?);
        let stage = Scratch(mkdtemp(&std::env::temp_dir(), "centurion-diff-")?);
        let fa = read_config_files(&a, &stage.0)?;
        let fb = read_config_files(&b, &stage.0)?;
        let mut d = diff_files(&fa, &fb);
        d["a"] = json!({"created": ma["created"], "centurion_version": ma["centurion_version"]});
        d["b"] = json!({"created": mb["created"], "centurion_version": mb["centurion_version"]});
        d["ok"] = json!(true);
        Ok(d)
    })();
    r.unwrap_or_else(fail)
}

/// Every regular file of a configuration archive's user and system parts, read into memory and keyed by
/// its path inside the archive. `unpack` has already refused links and special files.
fn read_config_files(archive: &Path, base: &Path) -> Result<BTreeMap<String, Vec<u8>>, String> {
    let mut out = BTreeMap::new();
    for top in ["user", "system"] {
        let Some(stage) = unpack(archive, top, base)? else { continue };
        collect_files(&stage.0.join(top), top, 0, &mut out)?;
    }
    Ok(out)
}

fn collect_files(dir: &Path, key: &str, depth: usize, out: &mut BTreeMap<String, Vec<u8>>) -> Result<(), String> {
    if depth > 16 { return Ok(()); }
    for e in fs::read_dir(dir).map_err(|e| format!("{}: {e}", dir.display()))?.flatten() {
        let k = format!("{key}/{}", e.file_name().to_string_lossy());
        let p = e.path();
        let md = fs::symlink_metadata(&p).map_err(|e| format!("{}: {e}", p.display()))?;
        if md.is_dir() { collect_files(&p, &k, depth + 1, out)?; }
        else if md.is_file() && md.len() <= MAX_FILE { out.insert(k, fs::read(&p).map_err(|e| format!("{}: {e}", p.display()))?); }
    }
    Ok(())
}

/// Changes between two sets of archive files (path → content). A file that is only reformatted is not a change.
fn diff_files(a: &BTreeMap<String, Vec<u8>>, b: &BTreeMap<String, Vec<u8>>) -> Value {
    let mut added: Vec<String> = b.keys().filter(|k| !a.contains_key(*k)).cloned().collect();
    let mut removed: Vec<String> = a.keys().filter(|k| !b.contains_key(*k)).cloned().collect();
    let mut changed: Vec<Value> = Vec::new();
    let mut same = 0u64;
    for (k, old) in a {
        let Some(new) = b.get(k) else { continue };
        if old == new { same += 1; continue; }
        match (keys_of(k, old), keys_of(k, new)) {
            (Some(x), Some(y)) => {
                let keys = key_changes(&x, &y);
                if keys.is_empty() { same += 1; } else { changed.push(json!({"path": k, "keys": keys})); }
            }
            // Not JSON or .ini: the whole file is compared, no keys to show.
            _ => changed.push(json!({"path": k, "keys": []})),
        }
    }
    let truncated = added.len() > DIFF_MAX || removed.len() > DIFF_MAX || changed.len() > DIFF_MAX;
    added.truncate(DIFF_MAX);
    removed.truncate(DIFF_MAX);
    changed.truncate(DIFF_MAX);
    json!({"added": added, "removed": removed, "changed": changed, "same": same, "truncated": truncated})
}

/// The key → value view of a JSON or .ini file; None for anything else.
fn keys_of(path: &str, bytes: &[u8]) -> Option<BTreeMap<String, String>> {
    let text = std::str::from_utf8(bytes).ok()?;
    if path.ends_with(".json") {
        let v: Value = serde_json::from_str(text).ok()?;
        let mut out = BTreeMap::new();
        flatten_json("", &v, &mut out);
        Some(out)
    } else if path.ends_with(".ini") {
        Some(flatten_ini(text))
    } else {
        None
    }
}

fn flatten_json(prefix: &str, v: &Value, out: &mut BTreeMap<String, String>) {
    match v {
        Value::Object(m) if !m.is_empty() => {
            for (k, x) in m { flatten_json(&join_key(prefix, k), x, out); }
        }
        Value::Array(a) if !a.is_empty() => {
            for (i, x) in a.iter().enumerate() { flatten_json(&format!("{prefix}[{i}]"), x, out); }
        }
        leaf => { out.insert(prefix.to_owned(), leaf.to_string()); }
    }
}

fn flatten_ini(text: &str) -> BTreeMap<String, String> {
    let mut out = BTreeMap::new();
    let mut section = String::new();
    for line in text.lines().map(str::trim) {
        if line.is_empty() || line.starts_with(['#', ';']) { continue; }
        if let Some(s) = line.strip_prefix('[').and_then(|l| l.strip_suffix(']')) {
            section = s.trim().to_owned();
        } else if let Some((k, v)) = line.split_once('=') {
            out.insert(join_key(&section, k.trim()), v.trim().to_owned());
        }
    }
    out
}

fn join_key(prefix: &str, k: &str) -> String {
    if prefix.is_empty() { k.to_owned() } else { format!("{prefix}.{k}") }
}

/// The keys whose value differs (a missing key counts as null), values cut to keep the report readable.
fn key_changes(a: &BTreeMap<String, String>, b: &BTreeMap<String, String>) -> Vec<Value> {
    let keys: BTreeSet<&String> = a.keys().chain(b.keys()).collect();
    keys.into_iter().filter(|k| a.get(*k) != b.get(*k)).take(DIFF_MAX)
        .map(|k| json!({"key": k, "old": a.get(k).map(|v| v.chars().take(160).collect::<String>()),
                        "new": b.get(k).map(|v| v.chars().take(160).collect::<String>())}))
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn files(list: &[(&str, &str)]) -> BTreeMap<String, Vec<u8>> {
        list.iter().map(|(k, v)| (k.to_string(), v.as_bytes().to_vec())).collect()
    }

    #[test]
    fn diff_reports_changed_keys() {
        let a = files(&[("user/config/centurion/x.json", r#"{"gpu":{"offset":-30,"name":"A"},"keep":1}"#)]);
        let b = files(&[("user/config/centurion/x.json", r#"{"gpu":{"offset":-25,"name":"A"},"keep":1,"new":true}"#)]);
        let d = diff_files(&a, &b);
        assert_eq!(d["changed"], json!([{"path": "user/config/centurion/x.json", "keys": [
            {"key": "gpu.offset", "old": "-30", "new": "-25"},
            {"key": "new", "old": null, "new": "true"}]}]));
        assert_eq!(d["same"], 0);
    }

    #[test]
    fn diff_ignores_reformatting() {
        let a = files(&[("system/etc/centurion/a.json", "{\"x\":1}")]);
        let b = files(&[("system/etc/centurion/a.json", "{\n  \"x\": 1\n}\n")]);
        let d = diff_files(&a, &b);
        assert_eq!(d["changed"], json!([]));
        assert_eq!(d["same"], 1);
    }

    #[test]
    fn diff_added_removed_and_opaque_files() {
        let a = files(&[("user/config/centurion/old.json", "{}"), ("system/etc/centurion/blob", "1")]);
        let b = files(&[("user/config/centurion/new.json", "{}"), ("system/etc/centurion/blob", "2")]);
        let d = diff_files(&a, &b);
        assert_eq!(d["added"], json!(["user/config/centurion/new.json"]));
        assert_eq!(d["removed"], json!(["user/config/centurion/old.json"]));
        assert_eq!(d["changed"], json!([{"path": "system/etc/centurion/blob", "keys": []}]));
        assert_eq!(d["truncated"], false);
    }

    #[test]
    fn ini_keys_by_section() {
        let a = keys_of("x.ini", b"[gpu]\n# note\nclock = 1\n").unwrap();
        let b = keys_of("x.ini", b"[gpu]\nclock=2\n[cpu]\nvolt=3\n").unwrap();
        let keys: Vec<Value> = key_changes(&a, &b);
        assert_eq!(keys.len(), 2);
        assert_eq!(keys[0]["key"], "cpu.volt");
        assert_eq!(keys[1]["key"], "gpu.clock");
    }

    #[test]
    fn increment_names() {
        assert!(is_image_name("backup-2026-10-29.tar.gz"));
        assert!(is_image_name("backup-2026-10-29_142501.tar.zst"));
        assert!(is_image_name("backup-2026-10-29_142501-i3.tar.zst"));
        assert!(is_image_name("backup-2026-10-29.tar.xz.age"));
        assert!(is_image_name("backup-2026-10-29_142501-i1.tar.gz.age"));
        assert!(!is_image_name("backup-2026-10-29-i.tar.gz"));
        assert!(!is_image_name("backup-2026-10-29_142501-x1.tar.gz"));
        assert!(!is_image_name("backup-2026-10-29.tar.gz.snar"));
        assert!(!is_image_name("backup-2026-10-29.tar.gz.age.partial"));
    }

    #[test]
    fn prune_removes_whole_chains_only() {
        let dir = std::env::temp_dir().join(format!("centurion-prune-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        // chain A (full + one increment, older) and chain B (a full image, newest)
        let mk = |name: &str, level: u64, chain: &str| {
            let p = dir.join(name);
            fs::write(&p, b"x").unwrap();
            fs::write(snar_of(&p), b"s").unwrap();
            fs::write(sidecar(&p), json!({"level": level, "chain": chain}).to_string()).unwrap();
            std::thread::sleep(Duration::from_millis(20));
        };
        mk("backup-2026-10-01.tar.gz", 0, "backup-2026-10-01.tar.gz");
        mk("backup-2026-10-02_100000-i1.tar.gz", 1, "backup-2026-10-01.tar.gz");
        mk("backup-2026-10-05.tar.gz", 0, "backup-2026-10-05.tar.gz");
        assert!(prune(&dir, 2, "backup-2026-10-05.tar.gz").is_empty());
        let mut removed = prune(&dir, 1, "backup-2026-10-05.tar.gz");
        removed.sort();
        assert_eq!(removed, vec!["backup-2026-10-01.tar.gz", "backup-2026-10-02_100000-i1.tar.gz"]);
        assert!(dir.join("backup-2026-10-05.tar.gz").exists());
        assert!(!snar_of(&dir.join("backup-2026-10-01.tar.gz")).exists());
        assert!(!sidecar(&dir.join("backup-2026-10-02_100000-i1.tar.gz")).is_empty());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn restore_sequence_full_then_increments() {
        let dir = std::env::temp_dir().join(format!("centurion-seq-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let mk = |name: &str, level: u64| {
            let p = dir.join(name);
            fs::write(&p, b"x").unwrap();
            fs::write(sidecar(&p), json!({"level": level, "chain": "backup-2026-10-01.tar.gz"}).to_string()).unwrap();
        };
        mk("backup-2026-10-01.tar.gz", 0);
        mk("backup-2026-10-02_100000-i1.tar.gz", 1);
        mk("backup-2026-10-03_100000-i2.tar.gz", 2);
        let seq = restore_sequence(&dir.join("backup-2026-10-03_100000-i2.tar.gz")).unwrap();
        let names: Vec<String> = seq.iter().map(|(p, _)| p.file_name().unwrap().to_string_lossy().into_owned()).collect();
        assert_eq!(names, ["backup-2026-10-01.tar.gz", "backup-2026-10-02_100000-i1.tar.gz", "backup-2026-10-03_100000-i2.tar.gz"]);
        assert_eq!(seq.iter().map(|(_, i)| *i).collect::<Vec<_>>(), [false, true, true]);
        // a missing increment is named, never skipped
        fs::remove_file(dir.join("backup-2026-10-02_100000-i1.tar.gz")).unwrap();
        assert!(restore_sequence(&dir.join("backup-2026-10-03_100000-i2.tar.gz")).unwrap_err().contains("increment 1"));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn recipient_and_identity_shapes() {
        assert!(recipient_of(&json!({"recipient": "age1"})).is_err());
        assert!(recipient_of(&json!({})).unwrap().is_none());
        assert!(identity_file(&json!({"identity": "not-a-key"})).is_err());
        assert!(identity_file(&json!({})).unwrap().is_none());
    }

    #[test]
    fn encrypted_image_reads_back_with_its_key_only() {
        // age-keygen, sh, tar and gzip are needed; without them there is nothing to prove.
        let have = |t: &str| tool(t).is_some();
        if !(have("age") && have("tar") && have("gzip")) { return; }
        let dir = std::env::temp_dir().join(format!("centurion-age-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(dir.join("src/etc")).unwrap();
        fs::write(dir.join("src/etc/a.txt"), "hello").unwrap();
        let keygen = |p: &Path| std::process::Command::new("age-keygen").arg("-o").arg(p).output().map_or(false, |o| o.status.success());
        if !keygen(&dir.join("key.txt")) || !keygen(&dir.join("other.txt")) { let _ = fs::remove_dir_all(&dir); return; }
        let text = fs::read_to_string(dir.join("key.txt")).unwrap();
        let recipient = text.lines().find_map(|l| l.strip_prefix("# public key: ")).unwrap().trim().to_owned();
        let secret = text.lines().find(|l| l.starts_with("AGE-SECRET-KEY-")).unwrap().to_owned();
        let other = fs::read_to_string(dir.join("other.txt")).unwrap();
        let other_secret = other.lines().find(|l| l.starts_with("AGE-SECRET-KEY-")).unwrap().to_owned();
        let img = dir.join("backup-2026-10-29.tar.gz.age");
        let sh = format!("tar -C {} -cf - etc | gzip -c | age -r {} > {}", dir.join("src").display(), recipient, img.display());
        assert!(std::process::Command::new("sh").arg("-c").arg(&sh).status().unwrap().success());

        assert_eq!(sniff(&img), Comp::Age);
        assert!(read_archive(&img, None, true, None, false).is_err());
        let (_k, key) = identity_file(&json!({"identity": secret})).unwrap().unwrap();
        assert_eq!(read_archive(&img, None, true, Some(&key), false).unwrap().files, 2);
        let (_w, wrong) = identity_file(&json!({"identity": other_secret})).unwrap().unwrap();
        let e = read_archive(&img, None, true, Some(&wrong), false).err().unwrap();
        assert!(e.contains("does not open"), "{e}");
        let _ = fs::remove_dir_all(&dir);
    }
    #[test]
    fn config_backup_then_diff() {
        // User operations: as root both refuse to run, so the test only proves anything as a user.
        if is_root() { return; }
        let base = std::env::temp_dir().join(format!("centurion-cfg-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&base);
        let cfg = base.join("cfg/centurion");
        let dest = base.join("arch");
        fs::create_dir_all(&cfg).unwrap();
        fs::create_dir_all(&dest).unwrap();
        std::env::set_var("XDG_CONFIG_HOME", base.join("cfg"));
        let dest_s = dest.to_str().unwrap();
        fs::write(cfg.join("gpu.json"), r#"{"gpu":{"offset":-30},"keep":1}"#).unwrap();
        let a = config_backup(&json!({"dest_dir": dest_s, "include_profile": false, "tag": "a"}));
        assert_eq!(a["ok"], true, "{a}");
        fs::write(cfg.join("gpu.json"), r#"{"gpu":{"offset":-25},"keep":1}"#).unwrap();
        fs::write(cfg.join("added.json"), "{}").unwrap();
        let b = config_backup(&json!({"dest_dir": dest_s, "include_profile": false, "tag": "b"}));
        assert_eq!(b["ok"], true, "{b}");
        let d = config_diff(&json!({"a": a["archive"], "b": b["archive"]}));
        assert_eq!(d["ok"], true, "{d}");
        assert_eq!(d["added"], json!(["user/config/centurion/added.json"]));
        assert_eq!(d["changed"][0]["keys"], json!([{"key": "gpu.offset", "old": "-30", "new": "-25"}]));
        let _ = fs::remove_dir_all(&base);
    }

    #[test]
    fn image_names() {
        assert!(is_image_name("backup-2026-09-26.tar.gz"));
        assert!(is_image_name("backup-2026-09-26_142501.tar.zst"));
        assert!(is_image_name("backup-2026-09-26.tar.xz"));
        assert!(!is_image_name("backup-2026-09-26.tar.gz.partial"));
        assert!(!is_image_name("backup-2026-09-26.tar.gz.info.json"));
        assert!(!is_image_name("backup-notes.tar.gz"));
        assert!(!is_image_name("my-backup-2026-09-26.tar.gz"));
        assert!(!is_image_name("backup-2026-09-26_x.tar.gz"));
    }
    #[test]
    fn exclude_literal() {
        assert_eq!(literal(Path::new("/mnt/usb [1]/a*b")), "/mnt/usb \\[1]/a\\*b");
    }
    #[test]
    fn names() {
        use std::ffi::OsStr;
        assert!(safe_name(OsStr::new("Gaming (max).json")).is_some());
        assert!(safe_name(OsStr::new(".hidden")).is_none());
        assert!(safe_name(OsStr::new("a/b")).is_none());
        assert_eq!(safe_tag(&json!("pre-restore")), Some("pre-restore"));
        assert_eq!(safe_tag(&json!("../x")), None);
    }
    #[test]
    fn pre_write_prune_keeps_newest_copies_only() {
        let s = |n: &str| n.to_string();
        let names = vec![
            s("centurion-config-2026-10-04_100000-pre-gpumux.tar.gz"),
            s("centurion-config-2026-10-01_100000-pre-memtiming.tar.gz"),
            s("centurion-config-2026-10-03_100000-pre-restore.tar.gz"), // the restore's own copy: never pruned
            s("centurion-config-2026-10-02_100000.tar.gz"),             // a manual backup
            s("centurion-config-2026-10-05_100000-pre-igpu.tar.gz"),
            s("backup-2026-10-06.tar.gz"),
            s("centurion-config-2026-10-07_100000-pre-biosoc.tar.gz"),
        ];
        assert!(is_pre_write_name("centurion-config-2026-10-01_100000-pre-memtiming.tar.gz"));
        assert!(!is_pre_write_name("centurion-config-2026-10-03_100000-pre-restore.tar.gz"));
        assert_eq!(pre_write_victims(names.clone(), 5), Vec::<String>::new());
        assert_eq!(
            pre_write_victims(names, 2),
            vec![s("centurion-config-2026-10-01_100000-pre-memtiming.tar.gz"), s("centurion-config-2026-10-04_100000-pre-gpumux.tar.gz")]
        );
    }
}
