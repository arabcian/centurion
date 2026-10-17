//! Native SMU mailbox access for the Curve Optimizer (replaces the `ryzenadj` subprocess).
//!
//! Every constant below was taken from the ZenTune/zenmaster extraction (smu_mailboxes.csv,
//! smu_opcodes.csv) **and** cross-checked against RyzenAdj upstream (lib/nb_smu_ops.c, lib/api.c,
//! lib/cpuid.c). A value is used only where both sources agree:
//!
//!  * mailbox addresses: all 9 families agree (MP1 for APUs, PSMU/RSMU for Dragon/Fire Range);
//!  * opcodes: set-coall / set-coper agree for every family below. Not used because upstream does
//!    not confirm them: the CSV's extra RSMU CO opcodes for FP6/FT6 (0xB1, 0x52, 0x5D, 0x53), the
//!    AM5_V1 MP1 CO opcodes (0x35/0x36), and every `get-coper-options` query (0xC3/0xE1/0xD5);
//!  * family detection: RyzenAdj's family/model table (ZenTune's "name contains hx" guess is not used);
//!  * arg0 encoding: bit-identical to what `ryzenadj --set-coall/--set-coper` sent (see `enc_*`).
//!
//! Zen 2 parts (Renoir/Lucienne/Mendocino/VanGogh) are deliberately absent: no per-core CO in
//! their firmware (the GUI already hides the tab below family 0x19).
//!
//! Safety rules: opcode and mailbox come only from `Spec` (no caller-supplied opcode), the SMU
//! answer is polled for at most one second (upstream loops forever), the self-test of RyzenAdj
//! (`smu_service_test`) must pass before the first CO write, and every bus transaction runs under
//! an exclusive flock with a bounded wait.

use std::fs::{File, OpenOptions};
use std::io;
use std::os::unix::fs::{FileExt, MetadataExt, OpenOptionsExt};
use std::os::unix::io::AsRawFd;
use std::time::{Duration, Instant};

const SMU_TIMEOUT: Duration = Duration::from_secs(1);
const LOCK_TIMEOUT: Duration = Duration::from_secs(5);
const KMOD_DIR: &str = "/sys/kernel/ryzen_smu_drv";
const PCI_CFG: &str = "/sys/bus/pci/devices/0000:00:00.0/config";
const PCI_LOCKS: [&str; 2] = ["/run/lock/access_pci.lock", "/run/centurion/access_pci.lock"];
const NB_INDEX: u64 = 0xB8;
const NB_DATA: u64 = 0xBC;
const RSP_OK: u32 = 0x01;
const TEST_MSG: u32 = 0x01;
const TEST_ARG: u32 = 0x47;

// ── families, mailboxes, opcodes ─────────────────────────────────────────────

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Family { Cezanne, Rembrandt, Phoenix, HawkPoint, DragonRange, KrackanPoint, StrixPoint, StrixHalo, FireRange }

/// RyzenAdj lib/cpuid.c (decimal family/model as /proc/cpuinfo prints them), CO-capable parts only.
pub fn detect(family: u32, model: u32) -> Option<Family> {
    use Family::*;
    Some(match (family, model) {
        (0x19, 80) => Cezanne,
        (0x19, 64) | (0x19, 68) => Rembrandt,
        (0x19, 97) => DragonRange,
        (0x19, 116) | (0x19, 120) => Phoenix,
        (0x19, 117) => HawkPoint,
        (0x1A, 32) | (0x1A, 36) => StrixPoint,
        (0x1A, 68) => FireRange,
        (0x1A, 96) => KrackanPoint,
        (0x1A, 112) => StrixHalo,
        _ => return None,
    })
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Mbox { pub msg: u32, pub rsp: u32, pub arg: u32 }

const MP1_CEZANNE: Mbox = Mbox { msg: 0x3B10528, rsp: 0x3B10564, arg: 0x3B10998 };
const MP1_REMBRANDT: Mbox = Mbox { msg: 0x3B10528, rsp: 0x3B10578, arg: 0x3B10998 }; // Rembrandt, Phoenix, Hawk Point
const MP1_STRIX: Mbox = Mbox { msg: 0x3B10928, rsp: 0x3B10978, arg: 0x3B10998 };     // Krackan, Strix Point, Strix Halo
const PSMU_RANGE: Mbox = Mbox { msg: 0x3B10524, rsp: 0x3B10570, arg: 0x3B10A40 };    // Dragon / Fire Range (RSMU)

#[derive(Debug)]
pub struct Spec {
    pub family: Family,
    pub name: &'static str,
    pub mailbox: &'static str,
    mbox: Mbox,
    op_all: u32,
    op_per: u32,
    /// Per-core slot layout is known (single L3 up to 8 cores, or 2 CCDs x 8 like Fire Range).
    /// Strix Point / Krackan mix Zen5 and Zen5c cores; no source defines their slot order.
    pub per_core: bool,
    pub max_ccd: i64,
    pub co_min: i64,
    pub co_max: i64,
}

const CO_MAX: i64 = 20;
static SPECS: [Spec; 9] = [
    Spec { family: Family::Cezanne,      name: "Cezanne/Barcelo", mailbox: "MP1",  mbox: MP1_CEZANNE,   op_all: 0x55, op_per: 0x54, per_core: true,  max_ccd: 1, co_min: -30, co_max: CO_MAX },
    Spec { family: Family::Rembrandt,    name: "Rembrandt",       mailbox: "MP1",  mbox: MP1_REMBRANDT, op_all: 0x4C, op_per: 0x4B, per_core: true,  max_ccd: 1, co_min: -50, co_max: CO_MAX },
    Spec { family: Family::Phoenix,      name: "Phoenix",         mailbox: "MP1",  mbox: MP1_REMBRANDT, op_all: 0x4C, op_per: 0x4B, per_core: true,  max_ccd: 1, co_min: -50, co_max: CO_MAX },
    Spec { family: Family::HawkPoint,    name: "Hawk Point",      mailbox: "MP1",  mbox: MP1_REMBRANDT, op_all: 0x4C, op_per: 0x4B, per_core: true,  max_ccd: 1, co_min: -50, co_max: CO_MAX },
    Spec { family: Family::DragonRange,  name: "Dragon Range",    mailbox: "RSMU", mbox: PSMU_RANGE,    op_all: 0x07, op_per: 0x06, per_core: true,  max_ccd: 2, co_min: -50, co_max: CO_MAX },
    Spec { family: Family::KrackanPoint, name: "Krackan Point",   mailbox: "MP1",  mbox: MP1_STRIX,     op_all: 0x4C, op_per: 0x4B, per_core: false, max_ccd: 1, co_min: -50, co_max: CO_MAX },
    Spec { family: Family::StrixPoint,   name: "Strix Point",     mailbox: "MP1",  mbox: MP1_STRIX,     op_all: 0x4C, op_per: 0x4B, per_core: false, max_ccd: 1, co_min: -50, co_max: CO_MAX },
    Spec { family: Family::StrixHalo,    name: "Strix Halo",      mailbox: "MP1",  mbox: MP1_STRIX,     op_all: 0x4C, op_per: 0x4B, per_core: true,  max_ccd: 2, co_min: -50, co_max: CO_MAX },
    Spec { family: Family::FireRange,    name: "Fire Range",      mailbox: "RSMU", mbox: PSMU_RANGE,    op_all: 0x07, op_per: 0x06, per_core: true,  max_ccd: 2, co_min: -50, co_max: CO_MAX },
];

pub fn spec_for(f: Family) -> &'static Spec {
    SPECS.iter().find(|s| s.family == f).expect("every Family has a Spec")
}

impl Spec {
    pub fn check_coall(&self, v: i64) -> Result<(), String> {
        if (self.co_min..=self.co_max).contains(&v) { Ok(()) }
        else { Err(format!("all-core offset {v} out of range for {} (allowed {}..{})", self.name, self.co_min, self.co_max)) }
    }

    pub fn check_coper(&self, ccd: i64, ccx: i64, core: i64, v: i64) -> Result<(), String> {
        if !self.per_core {
            return Err(format!("per-core offsets are not enabled for {}: its core-slot order is not defined by any \
                                verified source; use the all-core offset", self.name));
        }
        if !(0..self.max_ccd).contains(&ccd) { return Err(format!("ccd {ccd} does not exist on {} (0..{})", self.name, self.max_ccd - 1)); }
        if ccx != 0 { return Err(format!("ccx {ccx} does not exist on {} (only 0)", self.name)); }
        if !(0..8).contains(&core) { return Err(format!("core {core} out of range (0..7)")); }
        if (self.co_min..=self.co_max).contains(&v) { Ok(()) }
        else { Err(format!("offset {v} out of range for {} (allowed {}..{})", self.name, self.co_min, self.co_max)) }
    }
}

/// arg0 of set-coall: the i32 reinterpreted as u32, exactly what `ryzenadj --set-coall=-20`
/// produced (strtoul wraps to 0xFFFFFFEC). Low 20 bits equal AMD's 0x100000-|v| form.
pub fn enc_coall(v: i64) -> u32 { v as i32 as u32 }

/// arg0 of set-coper: ((ccd << 4 | ccx) << 4 | core) << 20 | (offset & 0xFFFF).
pub fn enc_coper(ccd: i64, ccx: i64, core: i64, v: i64) -> u32 {
    ((((ccd << 4 | ccx) << 4 | core) as u32) << 20) | ((v as u32) & 0xFFFF)
}

// ── SMN bus ──────────────────────────────────────────────────────────────────

pub trait Smn {
    fn rd(&mut self, addr: u32) -> io::Result<u32>;
    fn wr(&mut self, addr: u32, val: u32) -> io::Result<()>;
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Kind { Kmod, Pci }

pub struct Bus {
    kind: Kind,
    dev: File,
    lock: Option<File>, // PCI path only; the ryzen_smu path locks its own smn descriptor
}

struct Guard(libc::c_int);
impl Drop for Guard { fn drop(&mut self) { unsafe { libc::flock(self.0, libc::LOCK_UN); } } }

fn lock_bounded(f: &File) -> Result<Guard, String> {
    let fd = f.as_raw_fd();
    let t = Instant::now();
    loop {
        if unsafe { libc::flock(fd, libc::LOCK_EX | libc::LOCK_NB) } == 0 { return Ok(Guard(fd)); }
        let e = io::Error::last_os_error();
        if !matches!(e.kind(), io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted) { return Err(format!("SMN lock: {e}")); }
        if t.elapsed() >= LOCK_TIMEOUT { return Err("SMN bus busy: another tool holds the lock".into()); }
        std::thread::sleep(Duration::from_millis(5));
    }
}

/// ryzen_smu >= 0.1.7 (same floor as RyzenAdj's osdep_linux.c).
fn kmod_usable() -> bool {
    let Ok(v) = std::fs::read_to_string(format!("{KMOD_DIR}/drv_version")) else { return false };
    let n: Vec<u32> = v.trim().split('.').map(|p| p.parse().unwrap_or(0)).collect();
    n.len() == 3 && (n[0], n[1], n[2]) >= (0, 1, 7)
}

fn open_lock() -> Result<File, String> {
    let mut last = String::new();
    for (i, p) in PCI_LOCKS.iter().enumerate() {
        if i == 1 { crate::secure_dir("/run/centurion")?; }
        match OpenOptions::new().read(true).write(true).create(true).mode(0o600)
            .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC).open(p)
        {
            Ok(f) => {
                let md = f.metadata().map_err(|e| format!("{p}: {e}"))?;
                if !md.is_file() || md.uid() != 0 { return Err(format!("{p} is not a root-owned regular file")); }
                return Ok(f);
            }
            Err(e) => last = format!("{p}: {e}"),
        }
    }
    Err(format!("cannot open the SMN lock file ({last})"))
}

impl Bus {
    pub fn open() -> Result<Bus, String> {
        if kmod_usable() {
            if let Ok(dev) = OpenOptions::new().read(true).write(true).custom_flags(libc::O_CLOEXEC).open(format!("{KMOD_DIR}/smn")) {
                return Ok(Bus { kind: Kind::Kmod, dev, lock: None });
            }
        }
        let dev = OpenOptions::new().read(true).write(true).custom_flags(libc::O_CLOEXEC).open(PCI_CFG).map_err(|e| format!(
            "no SMN access: ryzen_smu >= 0.1.7 not loaded and {PCI_CFG}: {e} \
             (kernel lockdown / Secure Boot blocks PCI config writes; load ryzen_smu)"))?;
        let mut id = [0u8; 12];
        dev.read_exact_at(&mut id, 0).map_err(|e| format!("{PCI_CFG}: {e}"))?;
        // AMD (0x1022) host bridge (class 06:00), the root complex RyzenAdj talks to.
        if u16::from_le_bytes([id[0], id[1]]) != 0x1022 || id[11] != 0x06 || id[10] != 0x00 {
            return Err("00:00.0 is not an AMD host bridge; refusing to use its config space".into());
        }
        Ok(Bus { kind: Kind::Pci, dev, lock: Some(open_lock()?) })
    }

    pub fn backend(&self) -> &'static str { if self.kind == Kind::Kmod { "ryzen_smu" } else { "pci" } }

    fn guard(&self) -> Result<Guard, String> { lock_bounded(self.lock.as_ref().unwrap_or(&self.dev)) }
}

impl Smn for Bus {
    fn rd(&mut self, addr: u32) -> io::Result<u32> {
        let mut b = [0u8; 4];
        match self.kind {
            Kind::Kmod => {
                self.dev.write_all_at(&addr.to_le_bytes(), 0)?;
                self.dev.read_exact_at(&mut b, 0)?;
            }
            Kind::Pci => {
                self.dev.write_all_at(&(addr & !3).to_le_bytes(), NB_INDEX)?;
                self.dev.read_exact_at(&mut b, NB_DATA)?;
            }
        }
        Ok(u32::from_le_bytes(b))
    }

    fn wr(&mut self, addr: u32, val: u32) -> io::Result<()> {
        match self.kind {
            Kind::Kmod => {
                let mut buf = [0u8; 8];
                buf[..4].copy_from_slice(&addr.to_le_bytes());
                buf[4..].copy_from_slice(&val.to_le_bytes());
                self.dev.write_all_at(&buf, 0)
            }
            Kind::Pci => {
                self.dev.write_all_at(&addr.to_le_bytes(), NB_INDEX)?;
                self.dev.write_all_at(&val.to_le_bytes(), NB_DATA)
            }
        }
    }
}

// ── mailbox protocol ─────────────────────────────────────────────────────────

#[derive(Debug)]
pub enum SmuError {
    /// Bus error or no answer in time: the mailbox state is unknown, stop sending.
    Fatal(String),
    /// The SMU answered with something other than OK.
    Rejected(String),
}
impl SmuError {
    pub fn message(&self) -> &str { match self { SmuError::Fatal(m) | SmuError::Rejected(m) => m } }
    pub fn is_fatal(&self) -> bool { matches!(self, SmuError::Fatal(_)) }
}

fn rsp_text(r: u32) -> String {
    match r {
        0xFF => "SMU: failed (0xFF)".into(),
        0xFE => "SMU: unknown command, this CPU/firmware does not implement it (0xFE)".into(),
        0xFD => "SMU: rejected, prerequisite not met (0xFD)".into(),
        0xFC => "SMU: rejected, busy (0xFC)".into(),
        r => format!("SMU: unexpected response {r:#x}"),
    }
}

/// One mailbox transaction (RyzenAdj `smu_service_req` order): clear rsp, 6 args, message,
/// then poll rsp with a deadline. Caller holds the bus lock.
pub fn mailbox<S: Smn>(s: &mut S, mb: Mbox, opcode: u32, arg0: u32, timeout: Duration) -> Result<u32, SmuError> {
    let io = |e: io::Error| SmuError::Fatal(format!("SMN access: {e}"));
    s.wr(mb.rsp, 0).map_err(io)?;
    s.wr(mb.arg, arg0).map_err(io)?;
    for i in 1..6 { s.wr(mb.arg + 4 * i, 0).map_err(io)?; }
    s.wr(mb.msg, opcode).map_err(io)?;
    let start = Instant::now();
    let mut spins = 0u32;
    loop {
        let r = s.rd(mb.rsp).map_err(io)?;
        if r != 0 { return Ok(r); }
        spins += 1;
        if spins > 100 {
            if start.elapsed() >= timeout { return Err(SmuError::Fatal(format!("SMU did not answer within {} ms", timeout.as_millis()))); }
            std::thread::sleep(Duration::from_micros(100));
        }
    }
}

/// RyzenAdj `smu_service_test`: the arg window must hold a written value, and TestMessage
/// (0x01) must answer OK. Proves the mailbox addresses and the write path before any CO write.
fn selftest<S: Smn>(s: &mut S, mb: Mbox, timeout: Duration) -> Result<(), String> {
    let e = |e: io::Error| format!("SMN access: {e}");
    s.wr(mb.rsp, 0).map_err(e)?;
    s.wr(mb.arg, TEST_ARG).map_err(e)?;
    if s.rd(mb.arg).map_err(e)? != TEST_ARG {
        return Err("SMN window is not writable (Secure Boot / lockdown?): load ryzen_smu >= 0.1.7".into());
    }
    match mailbox(s, mb, TEST_MSG, TEST_ARG, timeout) {
        Ok(RSP_OK) => Ok(()),
        Ok(r) => Err(format!("SMU self-test failed: {}", rsp_text(r))),
        Err(x) => Err(format!("SMU self-test failed: {}", x.message())),
    }
}

// ── public API ───────────────────────────────────────────────────────────────

pub struct Smu { spec: &'static Spec, bus: Bus }

fn cpu_ids() -> Option<(u32, u32)> {
    let info = crate::cpuinfo_head();
    if !info.lines().any(|l| l.starts_with("vendor_id") && l.contains("AuthenticAMD")) { return None; }
    let num = |k: &str| info.lines().find(|l| l.split(':').next().map(str::trim) == Some(k))
        .and_then(|l| l.split(':').nth(1)).and_then(|v| v.trim().parse::<u32>().ok());
    Some((num("cpu family")?, num("model")?))
}

impl Smu {
    /// Detects the family, opens the SMN bus and runs the self-test. Writes nothing to the CO state.
    pub fn open() -> Result<Smu, String> {
        let (fam, model) = cpu_ids().ok_or("not an AMD CPU")?;
        let spec = detect(fam, model).map(spec_for).ok_or_else(|| format!(
            "CPU family {fam:#x} model {model}: no verified Curve Optimizer interface (supported: Cezanne, \
             Rembrandt, Phoenix, Hawk Point, Dragon Range, Krackan, Strix Point, Strix Halo, Fire Range)"))?;
        let mut bus = Bus::open()?;
        let _g = bus.guard()?;
        selftest(&mut bus, spec.mbox, SMU_TIMEOUT)?;
        drop(_g);
        Ok(Smu { spec, bus })
    }

    pub fn spec(&self) -> &'static Spec { self.spec }
    pub fn backend(&self) -> &'static str { self.bus.backend() }

    fn send(&mut self, opcode: u32, arg0: u32) -> Result<(), SmuError> {
        let _g = self.bus.guard().map_err(SmuError::Fatal)?;
        match mailbox(&mut self.bus, self.spec.mbox, opcode, arg0, SMU_TIMEOUT)? {
            RSP_OK => Ok(()),
            r => Err(SmuError::Rejected(rsp_text(r))),
        }
    }

    pub fn set_coall(&mut self, v: i64) -> Result<(), SmuError> {
        self.spec.check_coall(v).map_err(SmuError::Rejected)?;
        self.send(self.spec.op_all, enc_coall(v))
    }

    pub fn set_coper(&mut self, ccd: i64, ccx: i64, core: i64, v: i64) -> Result<(), SmuError> {
        self.spec.check_coper(ccd, ccx, core, v).map_err(SmuError::Rejected)?;
        self.send(self.spec.op_per, enc_coper(ccd, ccx, core, v))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;

    /// Fake SMU: answers `rsp` for the configured opcodes, records every message sent.
    struct Fake { regs: HashMap<u32, u32>, mb: Mbox, ok_ops: Vec<u32>, silent: bool, sent: Vec<(u32, u32)> }
    impl Fake {
        fn new(mb: Mbox, ok_ops: &[u32]) -> Fake { Fake { regs: HashMap::new(), mb, ok_ops: ok_ops.to_vec(), silent: false, sent: vec![] } }
    }
    impl Smn for Fake {
        fn rd(&mut self, a: u32) -> io::Result<u32> { Ok(*self.regs.get(&a).unwrap_or(&0)) }
        fn wr(&mut self, a: u32, v: u32) -> io::Result<()> {
            self.regs.insert(a, v);
            if a == self.mb.msg && !self.silent {
                self.sent.push((v, self.regs[&self.mb.arg]));
                let r = if v == TEST_MSG || self.ok_ops.contains(&v) { 1 } else { 0xFE };
                self.regs.insert(self.mb.rsp, r);
            }
            Ok(())
        }
    }

    #[test]
    fn encodings_match_ryzenadj() {
        // `ryzenadj --set-coall=-20` -> strtoul wraps -> 0xFFFFFFEC; low 20 bits = 0x100000 - 20.
        assert_eq!(enc_coall(-20), 0xFFFF_FFEC);
        assert_eq!(enc_coall(-20) & 0xFFFFF, 0x100000 - 20);
        assert_eq!(enc_coall(0), 0);
        assert_eq!(enc_coall(7), 7);
        // Same vectors as the old ryzen-co-helper test.
        assert_eq!(enc_coper(1, 0, 3, -20), 0x10300000 | 0xFFEC);
        assert_eq!(enc_coper(0, 0, 0, 5), 5);
        assert_eq!(enc_coper(15, 15, 15, -50), 0xFFF00000 | 0xFFCE);
    }

    #[test]
    fn detection_matches_ryzenadj_cpuid() {
        use Family::*;
        for (f, m, want) in [(0x19, 80, Cezanne), (0x19, 64, Rembrandt), (0x19, 68, Rembrandt), (0x19, 97, DragonRange),
                             (0x19, 116, Phoenix), (0x19, 120, Phoenix), (0x19, 117, HawkPoint), (0x1A, 32, StrixPoint),
                             (0x1A, 36, StrixPoint), (0x1A, 68, FireRange), (0x1A, 96, KrackanPoint), (0x1A, 112, StrixHalo)] {
            assert_eq!(detect(f, m), Some(want), "{f:#x}/{m}");
        }
        for (f, m) in [(0x17, 96), (0x17, 104), (0x17, 160), (0x19, 33), (0x1A, 0), (0x1B, 68)] {
            assert_eq!(detect(f, m), None, "{f:#x}/{m}");
        }
    }

    #[test]
    fn tables_match_both_sources() {
        // (family, mailbox msg/rsp/arg, coall op, coper op) -- identical in RyzenAdj and smu_*.csv.
        let want: [(Family, [u32; 3], u32, u32); 9] = [
            (Family::Cezanne,      [0x3B10528, 0x3B10564, 0x3B10998], 0x55, 0x54),
            (Family::Rembrandt,    [0x3B10528, 0x3B10578, 0x3B10998], 0x4C, 0x4B),
            (Family::Phoenix,      [0x3B10528, 0x3B10578, 0x3B10998], 0x4C, 0x4B),
            (Family::HawkPoint,    [0x3B10528, 0x3B10578, 0x3B10998], 0x4C, 0x4B),
            (Family::KrackanPoint, [0x3B10928, 0x3B10978, 0x3B10998], 0x4C, 0x4B),
            (Family::StrixPoint,   [0x3B10928, 0x3B10978, 0x3B10998], 0x4C, 0x4B),
            (Family::StrixHalo,    [0x3B10928, 0x3B10978, 0x3B10998], 0x4C, 0x4B),
            (Family::DragonRange,  [0x3B10524, 0x3B10570, 0x3B10A40], 0x07, 0x06),
            (Family::FireRange,    [0x3B10524, 0x3B10570, 0x3B10A40], 0x07, 0x06),
        ];
        for (f, a, all, per) in want {
            let s = spec_for(f);
            assert_eq!([s.mbox.msg, s.mbox.rsp, s.mbox.arg], a, "{f:?}");
            assert_eq!((s.op_all, s.op_per), (all, per), "{f:?}");
        }
    }

    #[test]
    fn range_and_slot_checks() {
        let fire = spec_for(Family::FireRange);
        assert!(fire.check_coper(1, 0, 7, -50).is_ok());
        assert!(fire.check_coper(2, 0, 0, 0).is_err());   // no third CCD
        assert!(fire.check_coper(0, 1, 0, 0).is_err());   // ccx must be 0
        assert!(fire.check_coper(0, 0, 8, 0).is_err());
        assert!(fire.check_coper(0, 0, 0, -51).is_err());
        assert!(fire.check_coper(0, 0, 0, 21).is_err());
        assert!(spec_for(Family::Phoenix).check_coper(1, 0, 0, 0).is_err()); // single die
        assert!(spec_for(Family::Cezanne).check_coall(-31).is_err());
        assert!(spec_for(Family::Cezanne).check_coall(-30).is_ok());
        assert!(spec_for(Family::StrixPoint).check_coper(0, 0, 0, -10).is_err()); // layout unverified
        assert!(spec_for(Family::StrixPoint).check_coall(-10).is_ok());
    }

    #[test]
    fn mailbox_sequence_and_only_spec_opcodes() {
        let s = spec_for(Family::Phoenix);
        let mut f = Fake::new(s.mbox, &[s.op_all, s.op_per]);
        assert!(selftest(&mut f, s.mbox, Duration::from_millis(50)).is_ok());
        let r = mailbox(&mut f, s.mbox, s.op_per, enc_coper(0, 0, 3, -20), Duration::from_millis(50)).unwrap();
        assert_eq!(r, 1);
        assert_eq!(f.sent.last(), Some(&(0x4B, enc_coper(0, 0, 3, -20))));
        for i in 1..6 { assert_eq!(f.regs[&(s.mbox.arg + 4 * i)], 0, "arg{i} must be zeroed"); }
        // Unknown opcode on this firmware -> 0xFE, reported, not treated as success.
        let r = mailbox(&mut f, s.mbox, 0x99, 0, Duration::from_millis(50)).unwrap();
        assert_eq!(r, 0xFE);
        assert!(rsp_text(r).contains("unknown command"));
    }

    #[test]
    fn silent_smu_times_out_instead_of_hanging() {
        let s = spec_for(Family::FireRange);
        let mut f = Fake::new(s.mbox, &[]);
        f.silent = true;
        let t = Instant::now();
        let e = mailbox(&mut f, s.mbox, s.op_all, 0, Duration::from_millis(60)).unwrap_err();
        assert!(e.is_fatal());
        assert!(t.elapsed() < Duration::from_secs(1));
        assert!(selftest(&mut f, s.mbox, Duration::from_millis(60)).is_err());
    }

    #[test]
    fn selftest_rejects_unwritable_window() {
        struct Ro(Mbox);
        impl Smn for Ro {
            fn rd(&mut self, _: u32) -> io::Result<u32> { Ok(0) }
            fn wr(&mut self, _: u32, _: u32) -> io::Result<()> { let _ = self.0; Ok(()) }
        }
        let s = spec_for(Family::FireRange);
        assert!(selftest(&mut Ro(s.mbox), s.mbox, Duration::from_millis(20)).unwrap_err().contains("not writable"));
    }
}
