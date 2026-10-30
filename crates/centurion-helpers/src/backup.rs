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
//! Encryption (optional): the image goes through age, to a public key (`age -r <age1…>`) or with a
//! passphrase (`age -p`). Secrets arrive in the request on stdin and never touch a disk: a secret key is
//! handed to age through a pipe (`-i /dev/fd/3`), a passphrase is typed into age's own prompt on a
//! private pseudo-terminal (age reads passphrases only from a terminal). Both are wiped when dropped.
//!
//! Increments (optional): every image has a snapshot (`<image>.snar`, GNU tar's listed-incremental;
//! for an encrypted image a private root-only copy under /var/lib/centurion/backup-snar, so the
//! list of every path on the system does not travel next to the ciphertext). An increment is one
//! level above the image it continues and keeps its chain's encryption; retention keeps whole chains,
//! and a restore unpacks the full image and then each increment in order. tar's own incremental
//! extraction is NOT used: it deletes everything in an archived directory that was not there at
//! backup time — new users under /home, the disks mounted under /mnt (the backup disk itself).
//! `ChainRestore` applies the recorded renames and deletes only what the chain itself restored.
//!
//! config_diff: what changed between two configuration archives, or one and the live configuration
//! (read only).

use crate::tarscan;
use serde_json::{json, Value};
use std::collections::{BTreeMap, BTreeSet, HashSet, VecDeque};
use std::fs::{self, File};
use std::hash::{BuildHasher, Hasher};
use std::io::{BufRead, BufReader, Read, Write};
use std::os::fd::{FromRawFd, OwnedFd, RawFd};
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

/// A secret from the request: never logged, never written to a file, wiped when dropped.
struct Secret(Vec<u8>);
impl Drop for Secret {
    fn drop(&mut self) { for b in self.0.iter_mut() { unsafe { std::ptr::write_volatile(b, 0) }; } }
}

/// What reads (or, for a passphrase, also writes) an encrypted image.
enum Key {
    /// Every "AGE-SECRET-KEY-1…" line of what was pasted or loaded (an age-keygen file has comments around it).
    Identity(Secret),
    Passphrase(Secret),
}

/// {"passphrase":"…"} or {"identity":"AGE-SECRET-KEY-1…" (or a whole key file)}; neither = no key.
fn key_of(req: &Value) -> Result<Option<Key>, String> {
    if let Some(p) = req["passphrase"].as_str().filter(|p| !p.is_empty()) {
        if p.len() > 1024 || p.contains(['\0', '\n', '\r']) { return Err("the passphrase must be one line of at most 1024 bytes".into()); }
        return Ok(Some(Key::Passphrase(Secret(p.as_bytes().to_vec()))));
    }
    let Some(text) = req["identity"].as_str().filter(|s| !s.trim().is_empty()) else { return Ok(None) };
    let keys: Vec<&str> = text.lines().map(str::trim).filter(|l| l.starts_with("AGE-SECRET-KEY-")).collect();
    let valid = |k: &&str| k.len() <= 128 && k.starts_with("AGE-SECRET-KEY-1") && k.bytes().all(|c| c.is_ascii_uppercase() || c.is_ascii_digit() || c == b'-');
    if keys.is_empty() || keys.len() > 16 || !keys.iter().all(valid) {
        return Err("the key is not an age secret key (AGE-SECRET-KEY-1…)".into());
    }
    let mut v = keys.join("\n").into_bytes();
    v.push(b'\n');
    Ok(Some(Key::Identity(Secret(v))))
}

fn cloexec_pipe() -> Result<(OwnedFd, OwnedFd), String> {
    let mut fds = [0 as RawFd; 2];
    if unsafe { libc::pipe2(fds.as_mut_ptr(), libc::O_CLOEXEC) } != 0 { return Err(std::io::Error::last_os_error().to_string()); }
    Ok(unsafe { (OwnedFd::from_raw_fd(fds[0]), OwnedFd::from_raw_fd(fds[1])) })
}

/// Resources an age child needs while it runs. Dropped after the child has been waited for: the slave
/// side of the terminal closes first, so the prompt-answering thread sees the end and is joined.
#[derive(Default)]
struct AgeAux {
    key_pipe: Option<OwnedFd>,  // read end of the key pipe: closed right after the spawn
    tty_slave: Option<OwnedFd>,
    stop: Arc<AtomicBool>,
    answerer: Option<std::thread::JoinHandle<()>>,
}
impl Drop for AgeAux {
    fn drop(&mut self) {
        self.tty_slave = None;
        self.stop.store(true, Ordering::Relaxed);
        if let Some(h) = self.answerer.take() { let _ = h.join(); }
    }
}

/// Answers every passphrase prompt age writes to its terminal (age asks twice when encrypting), at most
/// four times. The passphrase goes nowhere else; the terminal's echo is off (age_command turns it off first).
fn answer_prompts(master: OwnedFd, pass: Secret, stop: Arc<AtomicBool>) {
    let fd = master.as_raw_fd();
    let mut seen: Vec<u8> = Vec::new();
    let (mut pos, mut answered) = (0usize, 0);
    let mut buf = [0u8; 512];
    while !stop.load(Ordering::Relaxed) {
        let mut p = libc::pollfd { fd, events: libc::POLLIN, revents: 0 };
        if unsafe { libc::poll(&mut p, 1, 200) } <= 0 { continue; }
        let n = unsafe { libc::read(fd, buf.as_mut_ptr() as *mut libc::c_void, buf.len()) };
        if n <= 0 { break; }  // EIO: every slave descriptor is closed, age is gone
        seen.extend_from_slice(&buf[..n as usize]);
        while answered < 4 {
            let Some(i) = find(&seen[pos..], b"assphrase").map(|i| pos + i) else { break };
            let Some(j) = seen[i..].iter().position(|&b| b == b':').map(|j| i + j) else { break };
            let mut line = Secret([pass.0.as_slice(), b"\n"].concat());
            let _ = unsafe { libc::write(fd, line.0.as_ptr() as *const libc::c_void, line.0.len()) };
            line.0.iter_mut().for_each(|b| *b = 0);
            answered += 1;
            pos = j + 1;
        }
        if seen.len() > 64 << 10 { break; }
    }
}

fn find(hay: &[u8], needle: &[u8]) -> Option<usize> { hay.windows(needle.len()).position(|w| w == needle) }

/// `age` as a stage of the pipe: encrypting (`recipient`, or the `Key::Passphrase`) or decrypting (`key`).
/// The returned AgeAux must outlive the child.
fn age_command(age: &Path, low: bool, decrypt: bool, recipient: Option<&str>, key: Option<&Key>) -> Result<(Command, AgeAux), String> {
    let mut aux = AgeAux::default();
    let cmd = match (decrypt, key) {
        (true, Some(Key::Identity(k))) => {
            let (r, w) = cloexec_pipe()?;
            // The key is small: it fits the pipe buffer, so it is written whole before age starts.
            let n = unsafe { libc::write(w.as_raw_fd(), k.0.as_ptr() as *const libc::c_void, k.0.len()) };
            if n != k.0.len() as isize { return Err("could not hand the key to age".into()); }
            drop(w);
            let rfd = r.as_raw_fd();
            let mut c = command(age, low);
            c.args(["-d", "-i", "/dev/fd/3"]);
            unsafe { c.pre_exec(move || if libc::dup2(rfd, 3) < 0 { Err(std::io::Error::last_os_error()) } else { Ok(()) }); }
            aux.key_pipe = Some(r);
            c
        }
        (_, Some(Key::Passphrase(p))) if decrypt || recipient.is_none() => {
            let (mut m, mut s) = (0 as RawFd, 0 as RawFd);
            if unsafe { libc::openpty(&mut m, &mut s, std::ptr::null_mut(), std::ptr::null(), std::ptr::null()) } != 0 {
                return Err(format!("no pseudo-terminal for the passphrase: {}", std::io::Error::last_os_error()));
            }
            let (master, slave) = unsafe { (OwnedFd::from_raw_fd(m), OwnedFd::from_raw_fd(s)) };
            for fd in [m, s] { unsafe { libc::fcntl(fd, libc::F_SETFD, libc::FD_CLOEXEC); } }
            // Echo off from the start: the answer may arrive before age turns echo off itself, and an echoed
            // passphrase would come back through the master into answer_prompts' buffer.
            let mut t: libc::termios = unsafe { std::mem::zeroed() };
            if unsafe { libc::tcgetattr(s, &mut t) } == 0 {
                t.c_lflag &= !(libc::ECHO | libc::ECHONL);
                unsafe { libc::tcsetattr(s, libc::TCSANOW, &t) };
            }
            let mut c = command_tty(age, low, s);
            c.arg(if decrypt { "-d" } else { "-p" });
            let stop = aux.stop.clone();
            let pass = Secret(p.0.clone());
            aux.answerer = Some(std::thread::spawn(move || answer_prompts(master, pass, stop)));
            aux.tty_slave = Some(slave);
            c
        }
        (false, _) => {
            let r = recipient.ok_or("no recipient or passphrase to encrypt with")?;
            let mut c = command(age, low);
            c.arg("-r").arg(r);
            c
        }
        (true, None) => return Err("the archive is encrypted: its passphrase or age key is needed to read it".into()),
        (true, Some(_)) => unreachable!(),
    };
    Ok((cmd, aux))
}

/// Like `command`, but the child gets a session of its own with `tty` as its controlling terminal — age opens
/// /dev/tty for the passphrase. setsid also makes it a process group leader, so the cancel still reaches it.
fn command_tty(path: &Path, low_priority: bool, tty: RawFd) -> Command {
    let mut c = Command::new(path);
    if let Some(n) = path.file_name() { c.arg0(n); }
    c.env_clear().env("PATH", "/usr/sbin:/usr/bin:/sbin:/bin").env("LC_ALL", "C");
    unsafe {
        c.pre_exec(move || {
            if libc::setsid() < 0 || libc::ioctl(tty, libc::TIOCSCTTY as _, 0) < 0 { return Err(std::io::Error::last_os_error()); }
            if low_priority {
                libc::setpriority(libc::PRIO_PROCESS as _, 0, 19);
                libc::syscall(libc::SYS_ioprio_set, 1, 0, (2 << 13) | 7);
            }
            Ok(())
        });
    }
    c
}

/// Whether `key` opens an encrypted image: age decrypts (and authenticates) the first chunk, nothing more.
fn key_opens(archive: &Path, key: &Key) -> bool {
    let (Some(age), Ok(f)) = (tool("age"), File::open(archive)) else { return false };
    let Ok((mut c, aux)) = age_command(&age, true, true, None, Some(key)) else { return false };
    c.stdin(Stdio::from(f)).stdout(Stdio::piped()).stderr(Stdio::null());
    let Ok(mut ch) = c.spawn() else { return false };
    let mut aux = aux;
    aux.key_pipe = None;
    let mut one = [0u8; 1];
    let got = ch.stdout.take().is_some_and(|mut o| matches!(o.read(&mut one), Ok(1)));
    let _ = ch.kill();
    let _ = ch.wait();
    got
}

// ── system image: create ────────────────────────────────────────────────────

/// An increment is one level above the image it continues. After MAX_LEVEL increments the next image is a full
/// one again, so a chain is at most a full image and a week of dailies.
const MAX_LEVEL: u64 = 6;

/// How an image is encrypted, as its sidecar records it: what an increment must match to continue its chain.
#[derive(Clone, Debug, PartialEq)]
enum Enc { None, Recipient(String), Passphrase }

impl Enc {
    fn of_info(info: &Value) -> Enc {
        match (info["encryption"].as_str(), info["recipient"].as_str()) {
            (Some("passphrase"), _) => Enc::Passphrase,
            (_, Some(r)) if !r.is_empty() => Enc::Recipient(r.to_owned()),
            _ if info["encrypted"].as_bool() == Some(true) => Enc::Passphrase,  // encrypted, no key recorded
            _ => Enc::None,
        }
    }
    fn label(&self) -> &'static str {
        match self { Enc::None => "not encrypted", Enc::Recipient(_) => "encrypted to a public key", Enc::Passphrase => "encrypted with a passphrase" }
    }
}

/// An image a new increment could continue.
struct Tip { image: PathBuf, snar: PathBuf, level: u64, chain: String, enc: Enc }

/// The newest image in a folder that has a snapshot and the given encryption: the next increment continues
/// from it. A folder may hold chains of several kinds (an older passphrase chain next to a newer plain image):
/// an image continues the newest chain made the same way. None: no such image (`any` tells whether there was
/// another kind).
fn chain_tip(dir: &Path, enc: &Enc) -> (Option<Tip>, bool) {
    let mut best: Option<(SystemTime, Tip)> = None;
    let mut any = false;
    for e in fs::read_dir(dir).into_iter().flatten().flatten() {
        let n = e.file_name().to_string_lossy().into_owned();
        let p = e.path();
        if !is_image_name(&n) || !fs::symlink_metadata(snar_of(&p)).is_ok_and(|m| m.is_file()) { continue; }
        let Some(info) = read_info(&p) else { continue };
        let (Some(level), Some(chain)) = (info["level"].as_u64(), info["chain"].as_str()) else { continue };
        any = true;
        let tip_enc = Enc::of_info(&info);
        if tip_enc != *enc { continue; }
        let mtime = fs::symlink_metadata(&p).and_then(|m| m.modified()).unwrap_or(SystemTime::UNIX_EPOCH);
        if best.as_ref().map_or(true, |(t, _)| mtime > *t) {
            best = Some((mtime, Tip { snar: snar_of(&p), image: p, level, chain: chain.to_owned(), enc: tip_enc }));
        }
    }
    (best.map(|(_, t)| t), any)
}

/// Whether a new image may continue the chain of `tip`: same encryption, and for a passphrase the same
/// passphrase (a restore asks for one). Err stops the backup; Ok(Some(note)) starts a new chain instead.
fn continues(tip: &Tip, enc: &Enc, key: Option<&Key>) -> Result<Option<String>, String> {
    if tip.level >= MAX_LEVEL { return Ok(Some(format!("the chain has {MAX_LEVEL} increments: a new full image starts the next one"))); }
    if *enc != tip.enc {
        return Ok(Some(format!("a new full image: the chain is {}, this image is {}", tip.enc.label(), enc.label())));
    }
    if *enc == Enc::Passphrase && !key.is_some_and(|k| key_opens(&tip.image, k)) {
        return Err(format!("this passphrase does not open {}, the image this increment would continue. Use the chain's \
                            passphrase, or untick Incremental to start a new chain.", tip.image.file_name().unwrap_or_default().to_string_lossy()));
    }
    Ok(None)
}

/// {"op":"system_backup","dest_dir":"/…","compressor":"pigz|zstd|xz","level":6,"threads":0,
///  "include_home":false,"excludes":[…],"low_priority":true,"verify":true,"keep":0,"incremental":false,
///  "recipient":"age1…" | "passphrase":"…", "identity":"AGE-SECRET-KEY-1…" (only to verify a recipient image)}
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
    let key = key_of(req)?;
    let enc = match (&recipient, &key) {
        (Some(_), Some(Key::Passphrase(_))) => return Err("choose a public key or a passphrase, not both".into()),
        (Some(r), _) => Enc::Recipient(r.clone()),
        (None, Some(Key::Passphrase(_))) => Enc::Passphrase,
        (None, _) => Enc::None,
    };
    let age = if enc != Enc::None { Some(tool("age").ok_or("age is not installed (needed to encrypt the image)")?) } else { None };
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
    // An increment continues from the snapshot of the newest image, when the chain allows it (see `continues`).
    let mut notes: Vec<String> = Vec::new();
    let tip = if req["incremental"].as_bool().unwrap_or(false) {
        match chain_tip(&dest, &enc) {
            (Some(t), _) => match continues(&t, &enc, key.as_ref())? { None => Some(t), Some(n) => { notes.push(n); None } },
            (None, true) => { notes.push(format!("a new full image: the earlier images here were made another way (this one is {})", enc.label())); None }
            (None, false) => { notes.push("no earlier image with a snapshot here: this one is the full image of a new chain".into()); None }
        }
    } else { None };
    let level_out = tip.as_ref().map_or(0, |t| t.level + 1);
    let st = stamp();
    let ext_full = format!("tar.{ext}{}", if age.is_some() { ".age" } else { "" });
    let mut stem = if level_out == 0 { format!("backup-{}", st.date) } else { format!("backup-{}_{}-i{level_out}", st.date, st.time) };
    if level_out == 0 && dest.join(format!("{stem}.{ext_full}")).exists() { stem = format!("backup-{}_{}", st.date, st.time); }
    let name = format!("{stem}.{ext_full}");
    let chain = tip.as_ref().map_or_else(|| name.clone(), |t| t.chain.clone());
    let fin = dest.join(&name);
    let partial = dest.join(format!("{name}.partial"));
    let snar_fin = snar_of(&fin);
    let snar_part = snar_fin.with_file_name(format!(".{}.partial", snar_fin.file_name().unwrap_or_default().to_string_lossy()));
    if age.is_some() {
        crate::secure_dir("/var/lib/centurion")?;
        crate::secure_dir(PRIVATE_SNAR)?;
        let _ = fs::set_permissions(PRIVATE_SNAR, fs::Permissions::from_mode(0o700));  // even the image names stay private
    }
    // The archive must not swallow itself, its snapshot, the older images next to it or the private snapshots.
    excludes.push(literal(&fin));
    excludes.push(literal(&partial));
    excludes.push(literal(&snar_fin));
    excludes.push(literal(&snar_part));
    excludes.push(format!("{}/backup-*.tar.*", literal(&dest).trim_end_matches('/')));
    excludes.push(format!("{PRIVATE_SNAR}/*"));

    let out = fs::OpenOptions::new().write(true).create_new(true).mode(0o600)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC).open(&partial)
        .map_err(|e| format!("{}: {e}", partial.display()))?;
    struct Partial<'a>(&'a Path, bool);
    impl Drop for Partial<'_> { fn drop(&mut self) { if !self.1 { let _ = fs::remove_file(self.0); } } }
    let mut guard = Partial(&partial, false);
    let mut snap = Partial(&snar_part, false);
    let _ = fs::remove_file(&snar_part);  // a leftover from a crashed run would make this a wrong increment
    if let Some(t) = &tip { fs::copy(&t.snar, &snar_part).map_err(|e| format!("{}: {e}", t.snar.display()))?; }

    let mut tc = command(&tar, low);
    tc.current_dir("/").args(["--acls", "--xattrs", "--xattrs-include=*", "--ignore-failed-read"]);
    // GNU tar's snapshot: absent = a full dump (and it is written); present = only what changed since it.
    tc.arg(format!("--listed-incremental={}", snar_part.display()));
    for e in &excludes { tc.arg(format!("--exclude={e}")); }
    tc.args(["-cvpf", "-", "/"]).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
    let pipe = format!("{} {}{}", comp_path.display(), comp_args.join(" "), match &enc {
        Enc::None => String::new(), Enc::Recipient(r) => format!(" | age -r {r}"), Enc::Passphrase => " | age -p".into() });
    crate::emit(&json!({"event": "start", "phase": "backup", "archive": fin, "excludes": excludes, "level": level_out, "notes": notes,
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
    let mut _age_aux = None;
    if let Some(age) = &age {
        let built = age_command(age, low, false, recipient.as_deref(), key.as_ref());
        let (mut ac, aux) = match built {
            Ok(x) => x,
            Err(e) => { kill_all(libc::SIGKILL); for k in &mut kids { let _ = k.wait(); } return Err(e); }
        };
        ac.stdin(Stdio::from(kids[1].stdout.take().unwrap()))
            .stdout(Stdio::from(out.try_clone().map_err(|e| e.to_string())?)).stderr(Stdio::piped());
        let mut a = match spawn(ac, "age") {
            Ok(a) => a,
            Err(e) => { kill_all(libc::SIGKILL); for k in &mut kids { let _ = k.wait(); } return Err(e); }
        };
        _age_aux = Some(aux);
        readers.push(reader(a.stderr.take().unwrap(), prog.clone(), false));
        kids.push(a);
    }

    let started = Instant::now();
    let status = wait_all(&mut kids, || {
        crate::emit(&json!({"event": "progress", "phase": "backup", "bytes": out.metadata().map_or(0, |m| m.len()),
                            "files": prog.files.load(Ordering::Relaxed), "current": prog.current.lock().unwrap_or_else(|e| e.into_inner()).clone(),
                            "elapsed": started.elapsed().as_secs()}));
    });
    drop(_age_aux);
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
    let (encryption, rec) = match &enc { Enc::None => (Value::Null, Value::Null), Enc::Recipient(r) => (json!("recipient"), json!(r)), Enc::Passphrase => (json!("passphrase"), Value::Null) };
    let mut info = json!({"format": "centurion-system-backup", "version": 1, "created": st.iso,
        "host": host, "kernel": kernel, "archive": name, "bytes": bytes, "files": files,
        "compressor": comp_path.file_name().map(|n| n.to_string_lossy().into_owned()), "excludes": excludes,
        "include_home": req["include_home"].as_bool().unwrap_or(false), "tar_status": tcode,
        "messages": prog.msg_count.load(Ordering::Relaxed),
        "level": level_out, "chain": chain, "snapshot": true, "encrypted": age.is_some(), "encryption": encryption, "recipient": rec,
        "parent": tip.as_ref().map(|t| t.image.file_name().unwrap_or_default().to_string_lossy().into_owned())});
    let side = sidecar(&fin);
    let _ = crate::write_root_file(&side, info.to_string().as_bytes());
    crate::wlog::log("backup", &format!("system_backup {} level {level_out}{}", fin.display(),
                                        if age.is_some() { format!(" ({})", enc.label()) } else { String::new() }));

    // An encrypted image is read back with what opens it: the passphrase it was made with, or a secret key
    // sent along for this purpose. Without one it is verified later, from the list.
    let mut verified = Value::Null;
    let can_read = age.is_none() || matches!(&key, Some(Key::Passphrase(_))) || matches!(&key, Some(Key::Identity(_)));
    if req["verify"].as_bool().unwrap_or(false) && can_read {
        match read_archive(&fin, None, low, key.as_ref(), None) {
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
              "degraded": tcode == 2, "verified": verified, "encrypted": age.is_some(), "encryption": info["encryption"],
              "level": level_out, "chain": chain, "notes": notes,
              "removed": removed, "message_count": prog.msg_count.load(Ordering::Relaxed), "messages": prog.tail(),
              "elapsed": started.elapsed().as_secs()}))
}

/// Snapshots of encrypted images: root-only, here rather than next to the image (a snapshot names every
/// archived path). One per image, named by the image's name and folder.
const PRIVATE_SNAR: &str = "/var/lib/centurion/backup-snar";

/// The root-owned snapshot of an image: GNU tar's listed-incremental file, which the next increment starts from.
/// Next to the image, or for an encrypted image in PRIVATE_SNAR (moving such an image makes the next one full).
fn snar_of(archive: &Path) -> PathBuf {
    let name = archive.file_name().unwrap_or_default().to_string_lossy();
    if !name.ends_with(".age") { return PathBuf::from(format!("{}.snar", archive.display())); }
    let dir = archive.parent().map(|d| d.as_os_str().as_bytes().to_vec()).unwrap_or_default();
    // FNV-1a of the folder: stable across runs (unlike std's hasher), and the name alone keeps it readable.
    let h = dir.iter().fold(0xcbf2_9ce4_8422_2325u64, |h, &b| (h ^ b as u64).wrapping_mul(0x0100_0000_01b3));
    PathBuf::from(format!("{PRIVATE_SNAR}/{h:016x}-{name}.snar"))
}

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
/// file's magic bytes; an encrypted image is decrypted with `key` first, and its inner compression is read
/// from its name. With `chain`, the tar stream also passes through `ChainRestore` (an image of a chain being
/// restored step by step); `true` = this step is an increment, whose renames and deletions are applied.
fn read_archive(archive: &Path, target: Option<&Path>, low: bool, key: Option<&Key>,
                chain: Option<(&mut ChainRestore, bool)>) -> Result<ReadResult, String> {
    let tar = tool("tar").ok_or("tar is not installed")?;
    let file = File::open(archive).map_err(|e| format!("{}: {e}", archive.display()))?;
    let total = file.metadata().map_or(0, |m| m.len());
    let comp = Comp::from_magic(&file);
    let phase = if target.is_some() { "restore" } else { "verify" };
    let prog = Arc::new(Progress::default());
    // The stages between the file and tar, in order: [age -d], then the decompressor.
    let mut stages: Vec<(Command, Comp)> = Vec::new();
    let mut auxes: Vec<AgeAux> = Vec::new();
    let inner = if comp == Comp::Age {
        let age = tool("age").ok_or("age is not installed (needed to read an encrypted image)")?;
        let (c, aux) = age_command(&age, low, true, None, key)?;
        stages.push((c, Comp::Age));
        auxes.push(aux);
        inner_comp(archive)?
    } else {
        comp
    };
    if inner != Comp::None {
        let (path, args) = inner.decompressor()?;
        let mut c = command(&path, low);
        c.args(args);
        stages.push((c, inner));
    }
    let kinds: Vec<Comp> = stages.iter().map(|(_, k)| *k).collect();
    let mut kids: Vec<Child> = Vec::new();
    let mut readers = Vec::new();
    // The children read through a duplicate of `file`: one shared offset, so
    // our own descriptor tells how far into the archive they are.
    let mut src: Box<dyn Read + Send> = Box::new(file.try_clone().map_err(|e| e.to_string())?);
    let mut src_stdio: Option<Stdio> = Some(Stdio::from(file.try_clone().map_err(|e| e.to_string())?));
    for (mut c, _) in stages {
        c.stdin(src_stdio.take().unwrap()).stdout(Stdio::piped()).stderr(Stdio::piped());
        let mut ch = match spawn(c, "decompressor") {
            Ok(ch) => ch,
            Err(e) => { kill_all(libc::SIGKILL); for k in &mut kids { let _ = k.wait(); } return Err(e); }
        };
        for a in &mut auxes { a.key_pipe = None; }  // age holds its own copy now
        readers.push(reader(ch.stderr.take().unwrap(), prog.clone(), false));
        let out = ch.stdout.take().unwrap();
        src_stdio = Some(Stdio::from(out.try_clone_fd()?));
        src = Box::new(out);
        kids.push(ch);
    }
    let mut tc = command(&tar, low);
    match target {
        Some(t) => {
            // No --listed-incremental here: see ChainRestore for why the deletions are not tar's.
            tc.args(["--acls", "--xattrs", "--xattrs-include=*", "--numeric-owner", "-xvpf", "-"]);
            tc.arg("-C").arg(t);
        }
        None => { tc.args(["-tf", "-"]); }
    }
    let tee = chain.is_some();
    tc.stdin(if tee { drop(src_stdio.take()); Stdio::piped() } else { src_stdio.take().unwrap() })
        .stdout(Stdio::piped()).stderr(Stdio::piped());
    if !tee { drop(src); src = Box::new(std::io::empty()); }
    let mut t = match spawn(tc, "tar") {
        Ok(t) => t,
        Err(e) => { kill_all(libc::SIGKILL); for k in &mut kids { let _ = k.wait(); } return Err(e); }
    };
    readers.push(reader(t.stdout.take().unwrap(), prog.clone(), true));
    readers.push(reader(t.stderr.take().unwrap(), prog.clone(), false));
    let sink = t.stdin.take();
    kids.push(t);

    let started = Instant::now();
    let fd = file.as_raw_fd();
    let (status, scanned) = std::thread::scope(|sc| {
        let handle = match (chain, sink) {
            (Some((state, apply)), Some(sink)) => Some(sc.spawn(move || state.pass(src, sink, apply).map(|s| (s, state, apply)))),
            _ => None,
        };
        let status = wait_all(&mut kids, || {
            let pos = unsafe { libc::lseek(fd, 0, libc::SEEK_CUR) }.max(0) as u64;
            crate::emit(&json!({"event": "progress", "phase": phase, "bytes": pos, "total": total,
                                "files": prog.files.load(Ordering::Relaxed), "current": prog.current.lock().unwrap_or_else(|e| e.into_inner()).clone(),
                                "elapsed": started.elapsed().as_secs()}));
        });
        (status, handle.and_then(|h| h.join().ok()))
    });
    drop(auxes);
    for r in readers { let _ = r.join(); }
    if cancelled() { return Err("cancelled".into()); }
    let last = || prog.tail().last().map(|l| format!(": {l}")).unwrap_or_default();
    let ts = *status.last().unwrap();
    let tcode = ts.code().ok_or_else(|| format!("tar was killed ({ts})"))?;
    for (i, kind) in kinds.iter().enumerate() {
        let ds = status[i];
        if *kind == Comp::Age && !ds.success() {
            return Err(if prog.has("no identity matched") { "this key does not open the archive".to_string() }
                       else if prog.has("incorrect passphrase") { "this passphrase does not open the archive".to_string() }
                       else if prog.has("passphrase-encrypted") { "the archive is encrypted with a passphrase, not to a key".to_string() }
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
    // tar has finished this step: now the deletions it recorded, then what it brought in becomes "known".
    if let Some((s, state, apply)) = scanned.transpose().map_err(|e| format!("reading the archive stream: {e}"))? {
        state.finish(s, apply);
    }
    Ok(ReadResult { files: prog.files.load(Ordering::Relaxed), tar_status: tcode, prog, elapsed: started.elapsed().as_secs() })
}

trait TryCloneFd { fn try_clone_fd(&self) -> Result<OwnedFd, String>; }
impl TryCloneFd for std::process::ChildStdout {
    fn try_clone_fd(&self) -> Result<OwnedFd, String> {
        let fd = unsafe { libc::fcntl(self.as_raw_fd(), libc::F_DUPFD_CLOEXEC, 3) };
        if fd < 0 { return Err(std::io::Error::last_os_error().to_string()); }
        Ok(unsafe { OwnedFd::from_raw_fd(fd) })
    }
}

// ── chain restore: renames and deletions, done safely ─────────────────────────

/// One restore of an image chain (full image, then each increment). It remembers every path the chain has
/// put into the target (hashed), so an increment can delete what was deleted on the system between two
/// images — and nothing else: not files the target had of its own, not what the images excluded (/home/*,
/// /mnt/*…), never across a mount point, never through a symbolic link. Directory renames recorded by an
/// increment are applied before tar writes into the new name.
struct ChainRestore {
    root: OwnedFd,
    /// File system of the target: renames and deletions never reach a directory on another one.
    root_dev: libc::dev_t,
    hasher: std::collections::hash_map::RandomState,
    known: HashSet<u64>,
    deleted: u64,
    renamed: u64,
    notes: Vec<String>,
    /// Some stream could not be read: its paths are missing from `known`, so later deletions are fewer.
    partial: bool,
}

/// What one pass over a stream found.
struct Scanned { members: Vec<u64>, dumpdirs: Vec<tarscan::Dumpdir>, broken: bool }

impl ChainRestore {
    fn new(target: &Path) -> Result<ChainRestore, String> {
        let c = std::ffi::CString::new(target.as_os_str().as_bytes()).map_err(|e| e.to_string())?;
        let fd = unsafe { libc::open(c.as_ptr(), libc::O_RDONLY | libc::O_DIRECTORY | libc::O_CLOEXEC) };
        if fd < 0 { return Err(format!("{}: {}", target.display(), std::io::Error::last_os_error())); }
        let root = unsafe { OwnedFd::from_raw_fd(fd) };
        let root_dev = fstat(&root).map(|s| s.st_dev).ok_or_else(|| format!("{}: {}", target.display(), std::io::Error::last_os_error()))?;
        Ok(ChainRestore { root, root_dev, hasher: Default::default(), known: HashSet::new(),
                          deleted: 0, renamed: 0, notes: Vec::new(), partial: false })
    }

    fn hash(&self, p: &[u8]) -> u64 { let mut h = self.hasher.build_hasher(); h.write(p); h.finish() }

    fn note(&mut self, n: String) { if self.notes.len() < 50 { self.notes.push(n); } }

    /// Copies `src` to tar's stdin, reading the headers on the way. Renames are applied the moment their
    /// listing is complete, before the bytes that follow reach tar.
    fn pass(&mut self, mut src: Box<dyn Read + Send>, mut sink: std::process::ChildStdin, apply: bool) -> std::io::Result<Scanned> {
        let mut scan = tarscan::Scanner::new();
        let mut out = Scanned { members: Vec::new(), dumpdirs: Vec::new(), broken: false };
        let mut buf = vec![0u8; 256 << 10];
        loop {
            let n = match src.read(&mut buf) { Ok(0) => break, Ok(n) => n, Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue, Err(e) => return Err(e) };
            let mut events = Vec::new();
            scan.feed(&buf[..n], &mut |e| events.push(e));
            for e in events {
                match e {
                    tarscan::Event::Member(m) => out.members.push(self.hash(&m)),
                    tarscan::Event::Dumpdir(d) => {
                        if apply && d.ok { for (o, n) in &d.renames { self.rename(o, n); } }
                        out.dumpdirs.push(d);
                    }
                }
            }
            if sink.write_all(&buf[..n]).is_err() { break; }  // tar stopped reading: its status tells why
            if cancelled() { break; }
        }
        out.broken = scan.broken;
        Ok(out)
    }

    /// After tar finished a step: an increment's deletions, then everything it brought counts as the chain's.
    fn finish(&mut self, s: Scanned, apply: bool) {
        if s.broken {
            self.partial = true;
            self.note("part of an archive could not be read for its directory listings: nothing was deleted for it".into());
        } else if apply {
            for d in s.dumpdirs.iter().filter(|d| d.ok) { self.purge(d); }
        }
        self.known.extend(s.members);
    }

    /// A directory of the target by its archive path, on the target's own file system. Err(true): the path
    /// goes through a mount point (another file system); Err(false): it cannot be opened.
    fn open_dir(&self, rel: &[u8]) -> Result<OwnedFd, bool> {
        let mut cur = dup(&self.root).ok_or(false)?;
        if rel.is_empty() { return Ok(cur); }
        if !tarscan::plain_path(rel) { return Err(false); }
        for comp in rel.split(|&b| b == b'/') {
            cur = open_at(&cur, comp).ok_or(false)?;
            if fstat(&cur).map(|s| s.st_dev) != Some(self.root_dev) { return Err(true); }
        }
        Ok(cur)
    }

    /// Deletes the entries of directory `d.dir` that the chain restored earlier and that this increment's
    /// listing no longer has.
    fn purge(&mut self, d: &tarscan::Dumpdir) {
        let dir = match self.open_dir(&d.dir) {
            Ok(dir) => dir,
            Err(true) => {
                self.note(format!("/{}: on another file system (a mount point): nothing deleted there", String::from_utf8_lossy(&d.dir)));
                return;
            }
            Err(false) => return,
        };
        let dev = self.root_dev;
        let present: HashSet<&[u8]> = d.names.iter().map(|n| n.as_slice()).collect();
        for name in list_dir(&dir) {
            if present.contains(name.as_slice()) { continue; }
            let path = join(&d.dir, &name);
            if self.known.contains(&self.hash(&path)) { self.remove(&dir, &name, &path, dev, 0); }
        }
    }

    /// Removes `name` (in `parent`) if the chain put it there; a directory only with what it alone holds.
    fn remove(&mut self, parent: &OwnedFd, name: &[u8], path: &[u8], parent_dev: libc::dev_t, depth: u32) -> bool {
        let Ok(c) = std::ffi::CString::new(name) else { return false };
        let Some(st) = fstatat(parent, &c) else { return false };
        if st.st_mode & libc::S_IFMT == libc::S_IFDIR {
            if st.st_dev != parent_dev {
                self.note(format!("/{}: a mount point, kept", String::from_utf8_lossy(path)));
                return false;
            }
            if depth > 64 { return false; }
            let Some(fd) = open_at(parent, name) else { return false };
            let mut all = true;
            for child in list_dir(&fd) {
                let cp = join(path, &child);
                all &= self.known.contains(&self.hash(&cp)) && self.remove(&fd, &child, &cp, st.st_dev, depth + 1);
            }
            if all && unsafe { libc::unlinkat(parent.as_raw_fd(), c.as_ptr(), libc::AT_REMOVEDIR) } == 0 {
                self.deleted += 1;
                return true;
            }
            self.note(format!("/{}: deleted between the images, kept because it holds files the images do not have",
                              String::from_utf8_lossy(path)));
            return false;
        }
        let ok = unsafe { libc::unlinkat(parent.as_raw_fd(), c.as_ptr(), 0) } == 0;
        if ok { self.deleted += 1; }
        ok
    }

    /// A directory renamed between two images: moved inside the target, if the chain made the old one and
    /// nothing is in the way. What was known under the old name is known under the new one.
    fn rename(&mut self, old: &[u8], new: &[u8]) {
        let split = |p: &[u8]| -> Option<(Vec<u8>, Vec<u8>)> {
            if !tarscan::plain_path(p) { return None; }
            let i = p.iter().rposition(|&b| b == b'/');
            Some(match i { Some(i) => (p[..i].to_vec(), p[i + 1..].to_vec()), None => (Vec::new(), p.to_vec()) })
        };
        let show = |p: &[u8]| String::from_utf8_lossy(p).into_owned();
        let (Some((op, on)), Some((np, nn))) = (split(old), split(new)) else { return };
        let done = (|| {
            if !self.known.contains(&self.hash(old)) { return None; }
            let (od, nd) = (self.open_dir(&op).ok()?, self.open_dir(&np).ok()?);
            let (oc, nc) = (std::ffi::CString::new(on).ok()?, std::ffi::CString::new(nn).ok()?);
            let st = fstatat(&od, &oc)?;
            if st.st_mode & libc::S_IFMT != libc::S_IFDIR || fstatat(&nd, &nc).is_some() { return Some(false); }
            Some(unsafe { libc::renameat(od.as_raw_fd(), oc.as_ptr(), nd.as_raw_fd(), nc.as_ptr()) } == 0)
        })();
        if done == Some(true) {
            self.renamed += 1;
            if let Ok(fd) = self.open_dir(new) { self.rekey(&fd, old, new, 0); }
        } else {
            self.note(format!("/{} → /{}: rename recorded by the image not applied (not restored by it, or the new name exists)", show(old), show(new)));
        }
    }

    fn rekey(&mut self, dir: &OwnedFd, old: &[u8], new: &[u8], depth: u32) {
        if depth > 64 { return; }
        self.known.insert(self.hash(new));
        for child in list_dir(dir) {
            let (o, n) = (join(old, &child), join(new, &child));
            if !self.known.contains(&self.hash(&o)) { continue; }
            self.known.insert(self.hash(&n));
            if let Some(fd) = open_at(dir, &child) { self.rekey(&fd, &o, &n, depth + 1); }
        }
    }
}

fn join(dir: &[u8], name: &[u8]) -> Vec<u8> { if dir.is_empty() { name.to_vec() } else { [dir, b"/", name].concat() } }

fn dup(fd: &OwnedFd) -> Option<OwnedFd> {
    let n = unsafe { libc::fcntl(fd.as_raw_fd(), libc::F_DUPFD_CLOEXEC, 3) };
    (n >= 0).then(|| unsafe { OwnedFd::from_raw_fd(n) })
}

/// A directory below `dir`, never through a symbolic link.
fn open_at(dir: &OwnedFd, name: &[u8]) -> Option<OwnedFd> {
    let c = std::ffi::CString::new(name).ok()?;
    let fd = unsafe { libc::openat(dir.as_raw_fd(), c.as_ptr(), libc::O_RDONLY | libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC) };
    (fd >= 0).then(|| unsafe { OwnedFd::from_raw_fd(fd) })
}

fn fstat(fd: &OwnedFd) -> Option<libc::stat> {
    let mut st: libc::stat = unsafe { std::mem::zeroed() };
    (unsafe { libc::fstat(fd.as_raw_fd(), &mut st) } == 0).then_some(st)
}

fn fstatat(dir: &OwnedFd, name: &std::ffi::CStr) -> Option<libc::stat> {
    let mut st: libc::stat = unsafe { std::mem::zeroed() };
    (unsafe { libc::fstatat(dir.as_raw_fd(), name.as_ptr(), &mut st, libc::AT_SYMLINK_NOFOLLOW) } == 0).then_some(st)
}

fn list_dir(dir: &OwnedFd) -> Vec<Vec<u8>> {
    let mut out = Vec::new();
    let Some(d) = dup(dir) else { return out };
    let dp = unsafe { libc::fdopendir(d.as_raw_fd()) };
    if dp.is_null() { return out; }
    std::mem::forget(d);  // closedir closes it
    loop {
        let e = unsafe { libc::readdir(dp) };
        if e.is_null() { break; }
        let name = unsafe { std::ffi::CStr::from_ptr((*e).d_name.as_ptr()) }.to_bytes();
        if name != b"." && name != b".." { out.push(name.to_vec()); }
    }
    unsafe { libc::closedir(dp) };
    out
}

/// {"op":"verify","archive":"/…"} or {"op":"verify","archives":["/…", …]} — reads whole archives back (compressed
/// stream + tar structure) and compares the entry count with the one recorded when each was made. An encrypted
/// image needs its key or passphrase ({"identity":…} / {"passphrase":…}). Several archives: one result per archive.
/// An increment is also checked for the images it continues (`chain_error`): alone it restores nothing.
pub fn verify(req: &Value) -> Value {
    let key = match key_of(req) { Ok(k) => k, Err(e) => return fail(e) };
    let low = req["low_priority"].as_bool().unwrap_or(true);
    if let Some(list) = req["archives"].as_array() {
        if list.is_empty() || list.len() > 64 { return fail("choose between 1 and 64 archives"); }
        let results: Vec<Value> = list.iter().map(|a| verify_one(a, key.as_ref(), low)).collect();
        return json!({"ok": results.iter().all(|r| r["ok"] == json!(true)), "results": results});
    }
    verify_one(&req["archive"], key.as_ref(), low)
}

fn verify_one(archive_v: &Value, key: Option<&Key>, low: bool) -> Value {
    let archive = match abs_file(archive_v) { Ok(a) => a, Err(e) => return fail(e) };
    if key.is_none() && sniff(&archive) == Comp::Age {
        return json!({"ok": false, "needs_key": true, "archive": archive, "error": "the archive is encrypted: its passphrase or age key is needed"});
    }
    crate::emit(&json!({"event": "start", "phase": "verify", "archive": archive}));
    let r = match read_archive(&archive, None, low, key, None) {
        Ok(r) => r,
        Err(e) => { let mut f = fail(e); f["archive"] = json!(archive); return f; }
    };
    let side = sidecar(&archive);
    // As root only a root-owned, non-symlink sidecar counts (the archive's directory may be the
    // user's: a link there must not make root read, and then copy, another root file).
    let text = if is_root() { crate::read_root_file(&side, 1 << 20) } else { fs::read_to_string(&side).ok() };
    let mut info = text.and_then(|s| serde_json::from_str::<Value>(&s).ok()).filter(Value::is_object);
    if let Some(expect) = info.as_ref().and_then(|i| i["files"].as_u64()) {
        if expect != r.files {
            let mut f = fail(format!("{} entries recorded when the backup was made, {} read back", expect, r.files));
            f["archive"] = json!(archive);
            return f;
        }
    }
    let chain_error = match info.as_ref().and_then(|i| i["level"].as_u64()) {
        Some(l) if l > 0 => restore_sequence(&archive).err(),
        _ => None,
    };
    if let Some(i) = info.as_mut().filter(|_| is_root()) {
        i["verified"] = json!(stamp().iso);
        let _ = crate::write_root_file(&side, i.to_string().as_bytes());
    }
    json!({"ok": true, "archive": archive, "files": r.files, "compared": info.is_some(), "elapsed": r.elapsed, "chain_error": chain_error})
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
    // Each increment names the image it continued ("parent"): follow those links, so two increments of the
    // same level (an image made again after its snapshot was lost) cannot be mixed up.
    if info["parent"].is_string() {
        let mut seq = vec![(archive.to_path_buf(), true)];
        let (mut inf, mut l) = (info.clone(), level);
        while l > 0 {
            let p = inf["parent"].as_str().filter(|n| is_image_name(n)).map(|n| dir.join(n))
                .ok_or_else(|| format!("increment {l} of this chain does not name the image it continues"))?;
            inf = read_info(&p).filter(|_| fs::symlink_metadata(&p).is_ok_and(|m| m.is_file()))
                .filter(|i| i["chain"].as_str() == Some(chain.as_str()) && i["level"].as_u64() == Some(l - 1))
                .ok_or_else(|| format!("increment {} of this chain is missing from {}", l - 1, dir.display()))?;
            l -= 1;
            seq.push((p, l > 0));
        }
        seq.reverse();
        return Ok(seq);
    }
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
    let key = match key_of(req) { Ok(k) => k, Err(e) => return fail(e) };
    let seq = match restore_sequence(&archive) { Ok(s) => s, Err(e) => return fail(e) };
    if key.is_none() && seq.iter().any(|(f, _)| sniff(f) == Comp::Age) {
        return json!({"ok": false, "needs_key": true, "archive": archive, "error": "the archive is encrypted: its passphrase or age key is needed"});
    }
    crate::wlog::log("backup", &format!("system_restore {} -> {}{}", archive.display(), target.display(),
                                        if seq.len() > 1 { format!(" ({} steps)", seq.len()) } else { String::new() }));
    crate::emit(&json!({"event": "start", "phase": "restore", "archive": archive, "target": target, "steps": seq.len(),
                        "command": format!("tar --acls --xattrs --xattrs-include='*' --numeric-owner -xpf {} -C {}", archive.display(), target.display())}));
    let low = req["low_priority"].as_bool().unwrap_or(false);
    let (mut files, mut elapsed, mut degraded) = (0u64, 0u64, false);
    let mut messages: Vec<String> = Vec::new();
    let mut message_count = 0u64;
    // A chain is restored through ChainRestore: the increments' renames and deletions, limited to what the chain restored.
    let mut chain = if seq.len() > 1 {
        match ChainRestore::new(&target) { Ok(c) => Some(c), Err(e) => return fail(e) }
    } else { None };
    for (i, (file, incremental)) in seq.iter().enumerate() {
        if seq.len() > 1 {
            crate::emit(&json!({"event": "start", "phase": "restore", "archive": file, "step": i + 1, "steps": seq.len()}));
        }
        let r = match read_archive(file, Some(&target), low, key.as_ref(), chain.as_mut().map(|c| (c, *incremental))) {
            Ok(r) => r,
            Err(e) => return fail(if seq.len() > 1 { format!("{} (step {} of {}): {e}", file.display(), i + 1, seq.len()) } else { e }),
        };
        files += r.files;
        elapsed += r.elapsed;
        degraded |= r.tar_status != 0;
        message_count += r.prog.msg_count.load(Ordering::Relaxed);
        messages = r.prog.tail();
    }
    let (deleted, renamed, notes) = chain.as_ref().map_or((0, 0, Vec::new()), |c| (c.deleted, c.renamed, c.notes.clone()));
    if deleted + renamed > 0 { crate::wlog::log("backup", &format!("system_restore {}: {deleted} deleted, {renamed} renamed as the increments recorded", target.display())); }
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
           "degraded": degraded, "created_dirs": created, "deleted": deleted, "renamed": renamed, "notes": notes,
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
struct Copier { files: u64, bytes: u64, skipped: Vec<String>, unread: Vec<PathBuf>, links: bool }

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
        if self.unread.len() < 1000 { self.unread.push(p.to_path_buf()); }
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

struct Staged { user: Copier, state: Copier, sys: Copier, prof: Copier }

/// Copies what a configuration backup holds into `s`, in the archive's layout (see config_backup).
fn stage_config(s: &Path, profile: bool, logs: bool) -> Staged {
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
    if profile {
        for p in PROFILE { prof.copy(&Path::new("/").join(p), &s.join("system-profile/root").join(p), 0); }
        profile_facts(&s.join("system-profile"));
    }
    Staged { user, state, sys, prof }
}

fn config_backup_inner(req: &Value) -> Result<Value, String> {
    if is_root() { return Err("config_backup runs as the user, not through pkexec".into()); }
    let d = req["dest_dir"].as_str().filter(|s| s.starts_with('/') && s.len() < 1024).ok_or("dest_dir: an absolute path is required")?;
    fs::create_dir_all(d).map_err(|e| format!("{d}: {e}"))?;
    let dest = abs_dir(&req["dest_dir"], "dest_dir")?;
    let tar = tool("tar").ok_or("tar is not installed")?;
    let stage = Scratch(mkdtemp(&std::env::temp_dir(), "centurion-backup-")?);
    let s = &stage.0;
    let Staged { user, state, sys, prof } = stage_config(s, req["include_profile"].as_bool().unwrap_or(true),
                                                          req["include_logs"].as_bool().unwrap_or(false));
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
    let r = (|| read_manifest(&tool("tar").ok_or("tar is not installed")?, &abs_file(&req["archive"])?))();
    match r { Ok(m) => json!({"ok": true, "manifest": m}), Err(e) => fail(e) }
}

/// Extracts one top-level directory of a configuration backup into a 0700
/// scratch directory; ownership and modes from the archive are not applied.
/// `strict` (every restore): links and special files are refused. A comparison only reads regular files and
/// never follows a link, so it accepts them (the system profile keeps symbolic links as links).
fn unpack(archive: &Path, top: &str, base: &Path, strict: bool) -> Result<Option<Scratch>, String> {
    let tar = tool("tar").ok_or("tar is not installed")?;
    let m = read_manifest(&tar, archive)?;
    let key = match top { "user" => "user_config", "system-profile" => "system_profile", _ => "system" };
    let has = m["contents"][key].as_u64().unwrap_or(0) > 0 || (top == "user" && m["contents"]["user_state"].as_u64().unwrap_or(0) > 0);
    if !has { return Ok(None); }
    let stage = Scratch(mkdtemp(base, ".centurion-restore-")?);
    let o = command(&tar, false).arg("-xf").arg(archive).arg("-C").arg(&stage.0)
        .args(["--no-same-owner", "--no-same-permissions", top]).stdin(Stdio::null()).output().map_err(|e| format!("tar: {e}"))?;
    if !o.status.success() { return Err(format!("tar failed: {}", String::from_utf8_lossy(&o.stderr).trim())); }
    if strict { plain_tree(&stage.0, 0)?; }
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
        let Some(stage) = unpack(&archive, "user", &std::env::temp_dir(), true)? else { return Ok(json!({"ok": true, "files": 0})) };
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
    if !fs::symlink_metadata(dir).is_ok_and(|m| m.is_dir()) { return out; }
    for e in fs::read_dir(dir).into_iter().flatten().flatten() {
        let p = e.path();
        if !fs::symlink_metadata(&p).is_ok_and(|m| m.is_file()) { continue; }
        let Ok(text) = fs::read_to_string(&p) else { let _ = fs::remove_file(&p); continue };
        let Ok(mut v) = serde_json::from_str::<Value>(&text) else { continue };
        let Some(o) = v.as_object_mut() else { continue };
        if o.remove("command").is_some() {
            let ok = serde_json::to_vec_pretty(&v).ok().is_some_and(|b| fs::write(&p, b).is_ok());
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
    if !fs::symlink_metadata(src).is_ok_and(|m| m.is_dir()) { return Ok(()); }
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
        let Some(stage) = unpack(&archive, "system", Path::new("/var/lib/centurion"), true)? else {
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
        if fs::symlink_metadata(&conf).is_ok_and(|m| m.is_file()) {
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

// ── configuration backups: what changed ─────────────────────────────────────────

/// Longest list of files / keys / packages a diff reports; the rest is counted.
const DIFF_MAX: usize = 200;
/// Lines shown per changed text file, each way.
const LINES_MAX: usize = 40;
const TEXT_MAX: usize = 256 << 10;
/// Not compared: logs (they change all the time) and generated profile facts that differ on every boot.
const DIFF_SKIP: &[&str] = &["user/state/", "system/var/log/", "system-profile/mounts.txt", "system-profile/swaps.txt",
                             "system-profile/modules.txt", "system-profile/running-kernel-config.gz"];
const PACKAGES: &str = "system-profile/packages.txt";

/// {"op":"config_diff","a":"/…","b":"/…"|"current","profile":true} — what changed between two configuration
/// archives, or between one and the configuration as it is now ("current"): files added, removed and changed;
/// for JSON, .ini and KEY=value files (kernel .config, conf.d, make.conf) the keys; for other text the lines; and
/// with `profile` the system profile too, installed packages as added / removed / upgraded. Two archives are
/// put in time order (older first, `swapped` says so). Reads only, as the user; restores nothing.
pub fn config_diff(req: &Value) -> Value {
    let r: Result<Value, String> = (|| {
        if is_root() { return Err("config_diff runs as the user, not through pkexec".to_string()); }
        let tar = tool("tar").ok_or("tar is not installed")?;
        let mut a = abs_file(&req["a"])?;
        let mut ma = read_manifest(&tar, &a)?;
        let want_profile = req["profile"].as_bool().unwrap_or(true);
        let has_profile = |m: &Value| m["contents"]["system_profile"].as_u64().unwrap_or(0) > 0;
        let stage = Scratch(mkdtemp(&std::env::temp_dir(), "centurion-diff-")?);
        let (fa, fb, b_info, swapped, profile, unreadable);
        if req["b"].as_str() == Some("current") {
            profile = want_profile && has_profile(&ma);
            fa = read_config_files(&a, &stage.0, profile)?;
            let live = stage.0.join("live");
            fs::create_dir_all(&live).map_err(|e| e.to_string())?;
            let st = stage_config(&live, profile, false);
            let mut m = BTreeMap::new();
            for top in ["user", "system", "system-profile"] {
                if live.join(top).is_dir() { collect_files(&live.join(top), top, 0, &mut m)?; }
            }
            fb = m;
            // What could not be read now is not "removed": it is listed apart.
            let mut prefixes: Vec<String> = Vec::new();
            let home = config_home();
            for p in st.user.unread.iter() {
                if let Some(rel) = home.as_ref().and_then(|h| p.strip_prefix(h).ok()) { prefixes.push(format!("user/config/{}", rel.display())); }
            }
            for p in st.sys.unread.iter() { prefixes.push(format!("system{}", p.display())); }
            for p in st.prof.unread.iter() { prefixes.push(format!("system-profile/root{}", p.display())); }
            unreadable = prefixes;
            b_info = json!({"current": true, "created": stamp().iso, "name": "current"});
            swapped = false;
        } else {
            let mut b = abs_file(&req["b"])?;
            let mut mb = read_manifest(&tar, &b)?;
            // Made in the same second: the file times decide.
            let mtime = |p: &Path| fs::metadata(p).and_then(|m| m.modified()).unwrap_or(SystemTime::UNIX_EPOCH);
            swapped = (ma["created"].as_str().unwrap_or(""), mtime(&a)) > (mb["created"].as_str().unwrap_or(""), mtime(&b));
            if swapped { std::mem::swap(&mut a, &mut b); std::mem::swap(&mut ma, &mut mb); }
            profile = want_profile && has_profile(&ma) && has_profile(&mb);
            fa = read_config_files(&a, &stage.0, profile)?;
            fb = read_config_files(&b, &stage.0, profile)?;
            b_info = json!({"created": mb["created"], "centurion_version": mb["centurion_version"],
                            "name": b.file_name().map(|n| n.to_string_lossy().into_owned())});
            unreadable = Vec::new();
        }
        let skip = |k: &String| DIFF_SKIP.iter().any(|s| k.starts_with(s));
        let ignored = fa.keys().chain(fb.keys()).filter(|k| skip(k)).collect::<BTreeSet<_>>().len();
        let (fa, fb): (BTreeMap<_, _>, BTreeMap<_, _>) = (fa.into_iter().filter(|(k, _)| !skip(k)).collect(),
                                                          fb.into_iter().filter(|(k, _)| !skip(k)).collect());
        let mut d = diff_files(&fa, &fb);
        if !unreadable.is_empty() {
            let under = |k: &str| unreadable.iter().any(|p| k == p || k.starts_with(&format!("{p}/")));
            let removed: Vec<Value> = d["removed"].as_array().cloned().unwrap_or_default();
            let (gone, unread): (Vec<Value>, Vec<Value>) = removed.into_iter().partition(|v| !under(v.as_str().unwrap_or("")));
            d["removed"] = json!(gone);
            d["unreadable"] = json!(unread);
        }
        if profile {
            let text = |m: &BTreeMap<String, Vec<u8>>| m.get(PACKAGES).map(|b| String::from_utf8_lossy(b).into_owned()).unwrap_or_default();
            d["packages"] = package_changes(&text(&fa), &text(&fb));
        }
        d["profile_compared"] = json!(profile);
        d["ignored"] = json!(ignored);
        d["swapped"] = json!(swapped);
        d["a"] = json!({"created": ma["created"], "centurion_version": ma["centurion_version"],
                        "name": a.file_name().map(|n| n.to_string_lossy().into_owned())});
        d["b"] = b_info;
        d["ok"] = json!(true);
        Ok(d)
    })();
    r.unwrap_or_else(fail)
}

/// Every regular file of a configuration archive's user and system parts (and with `profile` its system
/// profile), read into memory and keyed by its path inside the archive. Links are never followed.
fn read_config_files(archive: &Path, base: &Path, profile: bool) -> Result<BTreeMap<String, Vec<u8>>, String> {
    let mut out = BTreeMap::new();
    let tops: &[&str] = if profile { &["user", "system", "system-profile"] } else { &["user", "system"] };
    for top in tops {
        let Some(stage) = unpack(archive, top, base, false)? else { continue };
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
        else if md.file_type().is_symlink() {
            // A link is compared by where it points.
            if let Ok(t) = fs::read_link(&p) { out.insert(k, format!("symbolic link → {}", t.display()).into_bytes()); }
        }
    }
    Ok(())
}

/// Changes between two sets of archive files (path → content). A file that is only reformatted is not a change.
fn diff_files(a: &BTreeMap<String, Vec<u8>>, b: &BTreeMap<String, Vec<u8>>) -> Value {
    let added: Vec<String> = b.keys().filter(|k| !a.contains_key(*k)).cloned().collect();
    let removed: Vec<String> = a.keys().filter(|k| !b.contains_key(*k)).cloned().collect();
    let mut changed: Vec<Value> = Vec::new();
    let mut same = 0u64;
    for (k, old) in a {
        let Some(new) = b.get(k) else { continue };
        if old == new { same += 1; continue; }
        if k == PACKAGES { changed.push(json!({"path": k, "kind": "packages", "keys": []})); continue; }
        match (keys_of(k, old), keys_of(k, new)) {
            (Some((kind, x)), Some((_, y))) => {
                let keys = key_changes(&x, &y);
                if keys.is_empty() { same += 1; } else { changed.push(json!({"path": k, "kind": kind, "keys": keys})); }
            }
            _ => match (text_of(old), text_of(new)) {
                (Some(x), Some(y)) => {
                    let lines = line_changes(x, y);
                    if lines["removed"].as_array().map_or(0, Vec::len) + lines["added"].as_array().map_or(0, Vec::len) == 0 {
                        changed.push(json!({"path": k, "kind": "text", "keys": [], "reordered": true}));
                    } else {
                        changed.push(json!({"path": k, "kind": "text", "keys": [], "lines": lines}));
                    }
                }
                _ => changed.push(json!({"path": k, "kind": "binary", "keys": []})),
            },
        }
    }
    let counts = json!({"added": added.len(), "removed": removed.len(), "changed": changed.len()});
    let truncated = added.len() > DIFF_MAX || removed.len() > DIFF_MAX || changed.len() > DIFF_MAX;
    let cut = |mut v: Vec<String>| { v.truncate(DIFF_MAX); v };
    changed.truncate(DIFF_MAX);
    json!({"added": cut(added), "removed": cut(removed), "changed": changed, "same": same, "truncated": truncated, "counts": counts})
}

fn text_of(b: &[u8]) -> Option<&str> {
    if b.len() > TEXT_MAX || b.contains(&0) { return None; }
    std::str::from_utf8(b).ok()
}

/// The lines only in `a` and only in `b`, each in file order (a line that only moved is not a change).
fn line_changes(a: &str, b: &str) -> Value {
    fn count(t: &str) -> BTreeMap<&str, i64> { let mut m = BTreeMap::new(); for l in t.lines() { *m.entry(l).or_default() += 1; } m }
    let (ca, cb) = (count(a), count(b));
    let only = |t: &str, mine: &BTreeMap<&str, i64>, other: &BTreeMap<&str, i64>| {
        let mut extra: BTreeMap<&str, i64> = mine.iter().map(|(l, n)| (*l, n - other.get(l).copied().unwrap_or(0))).filter(|(_, n)| *n > 0).collect();
        let mut out = Vec::new();
        for l in t.lines() {
            if let Some(n) = extra.get_mut(l).filter(|n| **n > 0) { *n -= 1; out.push(l.chars().take(200).collect::<String>()); }
        }
        out
    };
    let (mut r, mut ad) = (only(a, &ca, &cb), only(b, &cb, &ca));
    let more = r.len().saturating_sub(LINES_MAX) + ad.len().saturating_sub(LINES_MAX);
    r.truncate(LINES_MAX);
    ad.truncate(LINES_MAX);
    json!({"removed": r, "added": ad, "more": more})
}

/// The key → value view of a JSON, .ini or KEY=value file, with its kind; None for anything else.
fn keys_of(path: &str, bytes: &[u8]) -> Option<(&'static str, BTreeMap<String, String>)> {
    let text = std::str::from_utf8(bytes).ok()?;
    if path.ends_with(".json") {
        let v: Value = serde_json::from_str(text).ok()?;
        let mut out = BTreeMap::new();
        flatten_json("", &v, &mut out);
        Some(("json", out))
    } else if path.ends_with(".ini") {
        Some(("ini", flatten_ini(text)))
    } else {
        flatten_kv(text).map(|m| ("keys", m))
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

fn is_kv_key(k: &str) -> bool {
    k.len() <= 128 && k.chars().next().is_some_and(|c| c.is_ascii_alphabetic() || c == '_')
        && k.chars().all(|c| c.is_ascii_alphanumeric() || "_.-".contains(c))
}

/// KEY=value text — a kernel .config ("# CONFIG_X is not set" counts as a value), conf.d, make.conf, sysctl,
/// /etc/environment — when at least 80 % of its non-comment lines are assignments. Anything else is None.
fn flatten_kv(text: &str) -> Option<BTreeMap<String, String>> {
    let mut out = BTreeMap::new();
    let (mut lines, mut kv) = (0usize, 0usize);
    for l in text.lines().map(str::trim) {
        if l.is_empty() { continue; }
        if let Some(k) = l.strip_prefix("# ").and_then(|r| r.strip_suffix(" is not set")).filter(|k| is_kv_key(k)) {
            out.insert(k.to_owned(), "is not set".to_owned());
            lines += 1;
            kv += 1;
            continue;
        }
        if l.starts_with(['#', ';']) { continue; }
        lines += 1;
        let l = l.strip_prefix("export ").unwrap_or(l);
        if let Some((k, v)) = l.split_once('=') {
            let k = k.trim().trim_end_matches('+').trim();
            if is_kv_key(k) {
                out.insert(k.to_owned(), v.trim().trim_matches('"').to_owned());
                kv += 1;
            }
        }
    }
    (lines > 0 && kv * 10 >= lines * 8).then_some(out)
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

/// "category/name-1.2.3-r1" → ("category/name", "1.2.3-r1"): the version starts at the first "-digit" after the
/// category. A line without one is a name alone.
fn split_atom(atom: &str) -> (&str, &str) {
    let b = atom.as_bytes();
    let from = atom.find('/').map_or(0, |i| i + 1);
    (from..b.len()).find(|&i| b[i] == b'-' && b.get(i + 1).is_some_and(u8::is_ascii_digit))
        .map_or((atom, ""), |i| (&atom[..i], &atom[i + 1..]))
}

/// Installed packages (the profile's packages.txt) between two backups: added, removed and upgraded or
/// downgraded (same name, another version; slots are paired in version order).
fn package_changes(a: &str, b: &str) -> Value {
    let group = |t: &str| {
        let mut m: BTreeMap<String, Vec<String>> = BTreeMap::new();
        for l in t.lines().map(str::trim).filter(|l| !l.is_empty()) {
            let (n, v) = split_atom(l);
            m.entry(n.to_owned()).or_default().push(v.to_owned());
        }
        m
    };
    let (ga, gb) = (group(a), group(b));
    let names: BTreeSet<&String> = ga.keys().chain(gb.keys()).collect();
    let (mut added, mut removed, mut changed) = (Vec::new(), Vec::new(), Vec::new());
    let atom = |n: &str, v: &str| if v.is_empty() { n.to_owned() } else { format!("{n}-{v}") };
    for n in names {
        let mut va = ga.get(n).cloned().unwrap_or_default();
        let mut vb = gb.get(n).cloned().unwrap_or_default();
        let common: Vec<String> = va.iter().filter(|v| vb.contains(v)).cloned().collect();
        va.retain(|v| !common.contains(v));
        vb.retain(|v| !common.contains(v));
        va.sort();
        vb.sort();
        let pairs = va.len().min(vb.len());
        for i in 0..pairs { changed.push(json!({"name": n, "old": va[i], "new": vb[i]})); }
        removed.extend(va[pairs..].iter().map(|v| atom(n, v)));
        added.extend(vb[pairs..].iter().map(|v| atom(n, v)));
    }
    let counts = json!({"added": added.len(), "removed": removed.len(), "changed": changed.len()});
    added.truncate(DIFF_MAX);
    removed.truncate(DIFF_MAX);
    changed.truncate(DIFF_MAX);
    json!({"added": added, "removed": removed, "changed": changed, "counts": counts})
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::process::Command;

    fn files(list: &[(&str, &str)]) -> BTreeMap<String, Vec<u8>> {
        list.iter().map(|(k, v)| (k.to_string(), v.as_bytes().to_vec())).collect()
    }

    #[test]
    fn diff_reports_changed_keys() {
        let a = files(&[("user/config/centurion/x.json", r#"{"gpu":{"offset":-30,"name":"A"},"keep":1}"#)]);
        let b = files(&[("user/config/centurion/x.json", r#"{"gpu":{"offset":-25,"name":"A"},"keep":1,"new":true}"#)]);
        let d = diff_files(&a, &b);
        assert_eq!(d["changed"], json!([{"path": "user/config/centurion/x.json", "kind": "json", "keys": [
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
        assert_eq!(d["changed"], json!([{"path": "system/etc/centurion/blob", "kind": "text", "keys": [],
                                         "lines": {"removed": ["1"], "added": ["2"], "more": 0}}]));
        assert_eq!(d["truncated"], false);
    }

    #[test]
    fn ini_keys_by_section() {
        let a = keys_of("x.ini", b"[gpu]\n# note\nclock = 1\n").unwrap().1;
        let b = keys_of("x.ini", b"[gpu]\nclock=2\n[cpu]\nvolt=3\n").unwrap().1;
        let keys: Vec<Value> = key_changes(&a, &b);
        assert_eq!(keys.len(), 2);
        assert_eq!(keys[0]["key"], "cpu.volt");
        assert_eq!(keys[1]["key"], "gpu.clock");
    }

    #[test]
    fn kernel_config_and_conf_files_by_key() {
        let a = b"# Linux kernel config\nCONFIG_A=y\n# CONFIG_B is not set\nCONFIG_C=\"x\"\n";
        let b = b"# Linux kernel config\nCONFIG_A=m\nCONFIG_B=y\nCONFIG_C=\"x\"\n";
        let d = diff_files(&files(&[("system-profile/root/usr/src/linux/.config", std::str::from_utf8(a).unwrap())]),
                           &files(&[("system-profile/root/usr/src/linux/.config", std::str::from_utf8(b).unwrap())]));
        assert_eq!(d["changed"][0]["kind"], "keys");
        assert_eq!(d["changed"][0]["keys"], json!([{"key": "CONFIG_A", "old": "y", "new": "m"},
                                                    {"key": "CONFIG_B", "old": "is not set", "new": "y"}]));
        // prose is not mistaken for assignments
        assert!(flatten_kv("This is a note.\nIt has a=b once.\nAnd more words.\n").is_none());
        assert!(flatten_kv("export PATH=/bin\nLANG=C\n").is_some());
    }

    #[test]
    fn text_files_by_line() {
        let l = line_changes("a\nb\nc\nb\n", "c\nb\na\nd\n");
        assert_eq!(l, json!({"removed": ["b"], "added": ["d"], "more": 0}));
        let d = diff_files(&files(&[("system-profile/root/var/lib/portage/world", "x/a\ny/b\n")]),
                           &files(&[("system-profile/root/var/lib/portage/world", "y/b\nx/a\n")]));
        assert_eq!(d["changed"][0]["reordered"], true);
    }

    #[test]
    fn package_lists() {
        assert_eq!(split_atom("sys-kernel/gentoo-sources-6.6.30"), ("sys-kernel/gentoo-sources", "6.6.30"));
        assert_eq!(split_atom("media-libs/libsdl2-2.30.1-r1"), ("media-libs/libsdl2", "2.30.1-r1"));
        assert_eq!(split_atom("dev-qt/qt6-base-6.7.2"), ("dev-qt/qt6-base", "6.7.2"));
        assert_eq!(split_atom("app-misc/foo"), ("app-misc/foo", ""));
        let p = package_changes("a/x-1.0\nb/y-2.0\nc/slot-1.0\nc/slot-2.0\nd/gone-1\n",
                                "a/x-1.1\nb/y-2.0\nc/slot-2.0\nc/slot-3.0\ne/new-5\n");
        assert_eq!(p["changed"], json!([{"name": "a/x", "old": "1.0", "new": "1.1"}, {"name": "c/slot", "old": "1.0", "new": "3.0"}]));
        assert_eq!(p["added"], json!(["e/new-5"]));
        assert_eq!(p["removed"], json!(["d/gone-1"]));
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
    fn restore_sequence_follows_parents_past_a_twin_increment() {
        let dir = std::env::temp_dir().join(format!("centurion-seq-parent-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let mk = |name: &str, level: u64, parent: Option<&str>| {
            let p = dir.join(name);
            fs::write(&p, b"x").unwrap();
            fs::write(sidecar(&p), json!({"level": level, "chain": "backup-2026-10-01.tar.gz", "parent": parent}).to_string()).unwrap();
        };
        mk("backup-2026-10-01.tar.gz", 0, None);
        mk("backup-2026-10-02_100000-i1.tar.gz", 1, Some("backup-2026-10-01.tar.gz"));
        // its snapshot was lost, so the next increment continued the full image again: a second level 1
        mk("backup-2026-10-03_100000-i1.tar.gz", 1, Some("backup-2026-10-01.tar.gz"));
        mk("backup-2026-10-04_100000-i2.tar.gz", 2, Some("backup-2026-10-03_100000-i1.tar.gz"));
        let seq = restore_sequence(&dir.join("backup-2026-10-04_100000-i2.tar.gz")).unwrap();
        let names: Vec<String> = seq.iter().map(|(p, _)| p.file_name().unwrap().to_string_lossy().into_owned()).collect();
        assert_eq!(names, ["backup-2026-10-01.tar.gz", "backup-2026-10-03_100000-i1.tar.gz", "backup-2026-10-04_100000-i2.tar.gz"]);
        assert_eq!(seq.iter().map(|(_, i)| *i).collect::<Vec<_>>(), [false, true, true]);
        fs::remove_file(dir.join("backup-2026-10-03_100000-i1.tar.gz")).unwrap();
        assert!(restore_sequence(&dir.join("backup-2026-10-04_100000-i2.tar.gz")).unwrap_err().contains("increment 1"));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn recipient_and_key_shapes() {
        assert!(recipient_of(&json!({"recipient": "age1"})).is_err());
        assert!(recipient_of(&json!({})).unwrap().is_none());
        assert!(key_of(&json!({"identity": "not-a-key"})).is_err());
        assert!(key_of(&json!({})).unwrap().is_none());
        assert!(key_of(&json!({"identity": "  "})).unwrap().is_none());
        assert!(key_of(&json!({"passphrase": "two\nlines"})).is_err());
        assert!(matches!(key_of(&json!({"passphrase": "correct horse"})), Ok(Some(Key::Passphrase(_)))));
        // A whole age-keygen file: comments around the key are fine.
        let file = "# created: 2026-10-29\n# public key: age1xyz\nAGE-SECRET-KEY-1QQQQ\n";
        match key_of(&json!({"identity": file})) {
            Ok(Some(Key::Identity(k))) => assert_eq!(k.0, b"AGE-SECRET-KEY-1QQQQ\n"),
            _ => panic!("key file not accepted"),
        }
    }

    fn have(t: &str) -> bool { tool(t).is_some() }

    fn tmp(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("centurion-{tag}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    fn sh(cmd: &str) { assert!(Command::new("sh").arg("-c").arg(cmd).status().unwrap().success(), "{cmd}"); }

    #[test]
    fn encrypted_image_reads_back_with_its_key_only() {
        if !(have("age") && have("tar") && have("gzip")) { return; }
        let dir = tmp("age-test");
        fs::create_dir_all(dir.join("src/etc")).unwrap();
        fs::write(dir.join("src/etc/a.txt"), "hello").unwrap();
        let keygen = |p: &Path| Command::new("age-keygen").arg("-o").arg(p).output().is_ok_and(|o| o.status.success());
        if !keygen(&dir.join("key.txt")) || !keygen(&dir.join("other.txt")) { let _ = fs::remove_dir_all(&dir); return; }
        let text = fs::read_to_string(dir.join("key.txt")).unwrap();
        let recipient = text.lines().find_map(|l| l.strip_prefix("# public key: ")).unwrap().trim().to_owned();
        let other = fs::read_to_string(dir.join("other.txt")).unwrap();
        let img = dir.join("backup-2026-10-29.tar.gz.age");
        sh(&format!("tar -C {} -cf - etc | gzip -c | age -r {} > {}", dir.join("src").display(), recipient, img.display()));

        assert_eq!(sniff(&img), Comp::Age);
        assert!(read_archive(&img, None, true, None, None).is_err());
        // The whole key file is accepted; the key travels through a pipe, never a file.
        let key = key_of(&json!({"identity": text})).unwrap().unwrap();
        assert_eq!(read_archive(&img, None, true, Some(&key), None).unwrap().files, 2);
        assert!(key_opens(&img, &key));
        let wrong = key_of(&json!({"identity": other})).unwrap().unwrap();
        assert!(!key_opens(&img, &wrong));
        let e = read_archive(&img, None, true, Some(&wrong), None).err().unwrap();
        assert!(e.contains("does not open"), "{e}");
        let _ = fs::remove_dir_all(&dir);
    }

    /// age -p through the private terminal: encrypt, then read back with the right and a wrong passphrase.
    #[test]
    fn passphrase_image_round_trip() {
        if !(have("age") && have("tar") && have("gzip")) { return; }
        let dir = tmp("pass-test");
        fs::create_dir_all(dir.join("src/etc")).unwrap();
        fs::write(dir.join("src/etc/shadow"), "root:x").unwrap();
        let plain = dir.join("plain.tar.gz");
        sh(&format!("tar -C {} -cf - etc | gzip -c > {}", dir.join("src").display(), plain.display()));
        let img = dir.join("backup-2026-10-29.tar.gz.age");
        let pass = key_of(&json!({"passphrase": "correct horse battery"})).unwrap().unwrap();
        let (mut c, aux) = age_command(&tool("age").unwrap(), false, false, None, Some(&pass)).unwrap();
        c.stdin(File::open(&plain).unwrap()).stdout(File::create(&img).unwrap()).stderr(Stdio::piped());
        let o = c.spawn().unwrap().wait_with_output().unwrap();
        drop(aux);
        assert!(o.status.success(), "{}", String::from_utf8_lossy(&o.stderr));
        assert!(!fs::read(&img).unwrap().windows(6).any(|w| w == b"root:x"));
        assert!(key_opens(&img, &pass));
        assert_eq!(read_archive(&img, None, true, Some(&pass), None).unwrap().files, 2);
        let wrong = key_of(&json!({"passphrase": "nope"})).unwrap().unwrap();
        assert!(!key_opens(&img, &wrong));
        let e = read_archive(&img, None, true, Some(&wrong), None).err().unwrap();
        assert!(e.contains("passphrase does not open"), "{e}");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn chains_keep_their_encryption() {
        let tip = |enc: Enc| Tip { image: PathBuf::from("/nonexistent/backup-2026-10-01.tar.gz"), snar: PathBuf::new(), level: 1,
                                   chain: "backup-2026-10-01.tar.gz".into(), enc };
        assert_eq!(continues(&tip(Enc::None), &Enc::None, None), Ok(None));
        assert!(continues(&tip(Enc::None), &Enc::Recipient("age1a".into()), None).unwrap().is_some());
        assert!(continues(&tip(Enc::Recipient("age1a".into())), &Enc::Recipient("age1b".into()), None).unwrap().is_some());
        // A passphrase that cannot be checked against the chain stops the backup instead of guessing.
        assert!(continues(&tip(Enc::Passphrase), &Enc::Passphrase, None).is_err());
        let mut full = tip(Enc::None);
        full.level = MAX_LEVEL;
        assert!(continues(&full, &Enc::None, None).unwrap().is_some());
        assert_eq!(Enc::of_info(&json!({"encrypted": true, "encryption": "passphrase"})), Enc::Passphrase);
        assert_eq!(Enc::of_info(&json!({"encrypted": true, "recipient": "age1q"})), Enc::Recipient("age1q".into()));
        assert_eq!(Enc::of_info(&json!({"encrypted": false})), Enc::None);
    }

    #[test]
    fn encrypted_snapshots_are_private() {
        assert_eq!(snar_of(Path::new("/b/backup-2026-10-29.tar.gz")), PathBuf::from("/b/backup-2026-10-29.tar.gz.snar"));
        let p = snar_of(Path::new("/media/usb/backup-2026-10-29.tar.gz.age"));
        assert!(p.starts_with(PRIVATE_SNAR) && p.to_string_lossy().ends_with("-backup-2026-10-29.tar.gz.age.snar"));
        assert_ne!(p, snar_of(Path::new("/media/other/backup-2026-10-29.tar.gz.age")));
    }

    /// The data-loss case: an increment restored over a target must apply what changed between the images
    /// (a deletion, a directory rename) and leave everything else alone — files the target has of its own,
    /// and what the images excluded (here home/*, like /home/* on a real system).
    #[test]
    fn chain_restore_touches_only_what_the_chain_restored() {
        if !(have("tar") && have("gzip")) { return; }
        let base = tmp("chain-test");
        let (src, arch, dst) = (base.join("src"), base.join("arch"), base.join("dst"));
        for d in ["etc/old", "d1/sub", "home/alice", "var"] { fs::create_dir_all(src.join(d)).unwrap(); }
        fs::create_dir_all(&arch).unwrap();
        fs::write(src.join("etc/keep"), "k").unwrap();
        fs::write(src.join("etc/gone"), "g").unwrap();
        fs::write(src.join("etc/old/x"), "x").unwrap();
        fs::write(src.join("d1/g"), "unchanged").unwrap();
        fs::write(src.join("d1/sub/f"), "f").unwrap();
        fs::write(src.join("home/alice/data"), "a").unwrap();
        let mk = |name: &str, level: u64, from: Option<&str>| {
            let snar = arch.join(format!("{name}.snar"));
            if let Some(f) = from { fs::copy(arch.join(format!("{f}.snar")), &snar).unwrap(); }
            std::thread::sleep(Duration::from_millis(1100));  // tar's change detection works in whole seconds
            // posix + xattrs: what system_backup writes (GNU.dumpdir pax records)
            sh(&format!("tar -C {} --xattrs --listed-incremental={} --exclude='./home/*' -cf - . | gzip -c > {}",
                        src.display(), snar.display(), arch.join(name).display()));
            fs::write(sidecar(&arch.join(name)), json!({"level": level, "chain": "backup-2026-10-01.tar.gz"}).to_string()).unwrap();
            // File times come from the kernel's coarse clock: a change made right after tar started could look older.
            std::thread::sleep(Duration::from_millis(50));
        };
        mk("backup-2026-10-01.tar.gz", 0, None);
        fs::remove_file(src.join("etc/gone")).unwrap();
        fs::remove_dir_all(src.join("etc/old")).unwrap();
        fs::rename(src.join("d1"), src.join("d2")).unwrap();
        fs::write(src.join("etc/new"), "n").unwrap();
        mk("backup-2026-10-02_100000-i1.tar.gz", 1, Some("backup-2026-10-01.tar.gz"));
        fs::write(src.join("etc/keep"), "k2").unwrap();
        mk("backup-2026-10-03_100000-i2.tar.gz", 2, Some("backup-2026-10-02_100000-i1.tar.gz"));

        // The target already holds things of its own.
        for d in ["home/bob", "etc/local", "mnt/usb"] { fs::create_dir_all(dst.join(d)).unwrap(); }
        fs::write(dst.join("home/bob/thesis"), "precious").unwrap();
        fs::write(dst.join("home/loose"), "precious").unwrap();
        fs::write(dst.join("etc/local/mine"), "precious").unwrap();
        fs::write(dst.join("etc/mine.conf"), "precious").unwrap();
        fs::write(dst.join("mnt/usb/backups"), "precious").unwrap();

        let t = fs::canonicalize(&dst).unwrap();
        let r = system_restore(&json!({"archive": arch.join("backup-2026-10-03_100000-i2.tar.gz"), "target": t, "confirm": t}));
        assert_eq!(r["ok"], true, "{r}");
        assert_eq!(r["steps"], 3);
        let read = |p: &str| fs::read_to_string(dst.join(p)).ok();
        // what the images say
        assert_eq!(read("etc/keep").as_deref(), Some("k2"));
        assert_eq!(read("etc/new").as_deref(), Some("n"));
        assert_eq!(read("etc/gone"), None, "{r}");
        assert!(!dst.join("etc/old").exists(), "{r}");
        assert_eq!(read("d2/g").as_deref(), Some("unchanged"), "the renamed directory keeps its unchanged files: {r}");
        assert_eq!(read("d2/sub/f").as_deref(), Some("f"));
        assert!(!dst.join("d1").exists(), "{r}");
        // what they must not touch
        for p in ["home/bob/thesis", "home/loose", "etc/local/mine", "etc/mine.conf", "mnt/usb/backups"] {
            assert_eq!(read(p).as_deref(), Some("precious"), "{p} was removed: {r}");
        }
        assert!(r["deleted"].as_u64().unwrap() >= 3, "{r}");
        assert_eq!(r["renamed"], 1, "{r}");
        let _ = fs::remove_dir_all(&base);
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
        std::thread::sleep(Duration::from_millis(1100));  // "created" has whole seconds
        fs::write(cfg.join("gpu.json"), r#"{"gpu":{"offset":-25},"keep":1}"#).unwrap();
        fs::write(cfg.join("added.json"), "{}").unwrap();
        let b = config_backup(&json!({"dest_dir": dest_s, "include_profile": false, "tag": "b"}));
        assert_eq!(b["ok"], true, "{b}");
        let d = config_diff(&json!({"a": a["archive"], "b": b["archive"]}));
        assert_eq!(d["ok"], true, "{d}");
        assert_eq!(d["added"], json!(["user/config/centurion/added.json"]));
        assert_eq!(d["changed"][0]["keys"], json!([{"key": "gpu.offset", "old": "-30", "new": "-25"}]));
        // Picked newest first: still compared oldest to newest.
        let r = config_diff(&json!({"a": b["archive"], "b": a["archive"]}));
        assert_eq!(r["swapped"], true);
        assert_eq!(r["added"], json!(["user/config/centurion/added.json"]));
        // Against what is here now.
        fs::write(cfg.join("gpu.json"), r#"{"gpu":{"offset":-20},"keep":1}"#).unwrap();
        let c = config_diff(&json!({"a": b["archive"], "b": "current"}));
        assert_eq!(c["ok"], true, "{c}");
        assert_eq!(c["b"]["current"], true);
        assert_eq!(c["changed"][0]["keys"], json!([{"key": "gpu.offset", "old": "-25", "new": "-20"}]));
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
