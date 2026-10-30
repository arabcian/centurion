//! Custom boot logo (Lenovo "Logo DIY").
//!
//! The firmware keeps the feature in two UEFI variables and reads the picture from the EFI
//! system partition:
//!
//!   LBLDESP-871455d0-5576-4fb8-9865-af0824463b9e  10 bytes: u8 enabled, u32 width, u32 height,
//!                                                  u8 format mask (1 JPEG, 8 GIF, 0x10 BMP, 0x20 PNG)
//!   LBLDVC-871455d1-5576-4fb8-9865-af0824463c9f   40 bytes: u32 version, 32-byte digest, 4 bytes pad
//!   <ESP>/EFI/Lenovo/Logo/mylogo_<w>x<h>.<ext>
//!
//! Version 0x00020003 and newer: the digest is the SHA-256 of the logo file. Older versions: the
//! CRC-32 of the file's first 512 bytes in the first 4 bytes, the rest zero. (Layout, names and the
//! two digest rules as documented by LenovoLegionToolkit's BootLogo; the 16AFR10H reads
//! `00 | 00 0a 00 00 | 40 06 00 00 | 3f` and `03 00 02 00` + 36 zero bytes, i.e. off, 2560×1600,
//! SHA-256 version.) Implemented from that description; no code is shared.
//!
//! Only baseline JPEGs at exactly the firmware's resolution are written: the GUI re-encodes every
//! picture first, and the firmware's image parsers are a known attack surface (LogoFAIL,
//! CVE-2023-40238), so the simplest well-formed input is all that reaches them. The bytes arrive in
//! the request itself; this root helper never opens a path the user chose.
//!
//! Both variables must already exist; nothing here creates a UEFI variable. Every write is read
//! back, and a failed step puts back what the earlier steps changed.

use serde_json::{json, Value};
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

const DESC_VAR: &str = "/sys/firmware/efi/efivars/LBLDESP-871455d0-5576-4fb8-9865-af0824463b9e";
const SUM_VAR: &str = "/sys/firmware/efi/efivars/LBLDVC-871455d1-5576-4fb8-9865-af0824463c9f";
const DESC_LEN: usize = 4 + 10; // efivarfs: u32 attributes, then the data
const SUM_LEN: usize = 4 + 40;
const SHA256_VERSION: u32 = 0x0002_0003;
const FMT_JPEG: u8 = 0x01;
const LOGO_DIR: &str = "EFI/Lenovo/Logo";
const ESP_PART_TYPE: &str = "c12a7328-f81f-11d2-ba4b-00a0c93ec93b";
/// Largest picture accepted (the request carries it hex-encoded).
pub const MAX_LOGO: usize = 4 << 20;

#[derive(Debug, PartialEq)]
struct Desc { enabled: bool, width: u32, height: u32, formats: u8 }

fn u32_at(b: &[u8], o: usize) -> u32 { u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]]) }

fn parse_desc(var: &[u8]) -> Option<Desc> {
    if var.len() != DESC_LEN { return None; }
    let d = &var[4..];
    let (width, height) = (u32_at(d, 1), u32_at(d, 5));
    (width > 0 && height > 0 && width <= 16384 && height <= 16384)
        .then_some(Desc { enabled: d[0] != 0, width, height, formats: d[9] })
}

fn format_names(mask: u8) -> Vec<&'static str> {
    [(0x01, "jpeg"), (0x08, "gif"), (0x10, "bmp"), (0x20, "png")].iter()
        .filter(|&&(b, _)| mask & b != 0).map(|&(_, n)| n).collect()
}

fn read_var(path: &str, len: usize) -> Result<Vec<u8>, String> {
    let b = fs::read(path).map_err(|_| "this firmware has no custom boot logo support (UEFI variable missing)".to_string())?;
    if b.len() != len { return Err(format!("{path}: {} bytes, {len} expected; not touched", b.len())); }
    Ok(b)
}

/// Writes the whole variable and reads it back.
fn write_var(path: &str, body: &[u8]) -> Result<(), String> {
    crate::memory_spd::efivar_write(path, body)?;
    match fs::read(path) {
        Ok(b) if b == body => Ok(()),
        _ => Err(format!("{path}: read-back differs after the write")),
    }
}

/// Puts `orig` back after a failed step; a variable the failed write never changed is left alone.
fn restore_var(path: &str, orig: &[u8]) -> bool {
    fs::read(path).map_or(false, |b| b == orig) || write_var(path, orig).is_ok()
}

// ── digests ─────────────────────────────────────────────────────────────────

fn sha256(data: &[u8]) -> [u8; 32] {
    const K: [u32; 64] = [
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    ];
    let mut h: [u32; 8] = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19];
    let mut msg = data.to_vec();
    msg.push(0x80);
    while msg.len() % 64 != 56 { msg.push(0); }
    msg.extend_from_slice(&((data.len() as u64) * 8).to_be_bytes());
    for block in msg.chunks_exact(64) {
        let mut w = [0u32; 64];
        for i in 0..16 { w[i] = u32::from_be_bytes([block[4 * i], block[4 * i + 1], block[4 * i + 2], block[4 * i + 3]]); }
        for i in 16..64 {
            let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
            let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16].wrapping_add(s0).wrapping_add(w[i - 7]).wrapping_add(s1);
        }
        let mut v = h;
        for i in 0..64 {
            let s1 = v[4].rotate_right(6) ^ v[4].rotate_right(11) ^ v[4].rotate_right(25);
            let ch = (v[4] & v[5]) ^ (!v[4] & v[6]);
            let t1 = v[7].wrapping_add(s1).wrapping_add(ch).wrapping_add(K[i]).wrapping_add(w[i]);
            let s0 = v[0].rotate_right(2) ^ v[0].rotate_right(13) ^ v[0].rotate_right(22);
            let maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            let t2 = s0.wrapping_add(maj);
            v = [t1.wrapping_add(t2), v[0], v[1], v[2], v[3].wrapping_add(t1), v[4], v[5], v[6]];
        }
        for (a, b) in h.iter_mut().zip(v) { *a = a.wrapping_add(b); }
    }
    let mut out = [0u8; 32];
    for (i, x) in h.iter().enumerate() { out[4 * i..4 * i + 4].copy_from_slice(&x.to_be_bytes()); }
    out
}

/// CRC-32 (IEEE, reflected, as zlib computes it).
fn crc32(data: &[u8]) -> u32 {
    let mut c = 0xFFFF_FFFFu32;
    for &b in data {
        c ^= b as u32;
        for _ in 0..8 { c = if c & 1 != 0 { (c >> 1) ^ 0xEDB8_8320 } else { c >> 1 }; }
    }
    !c
}

/// The 32 digest bytes the firmware expects for `img` under checksum `version`.
fn digest(version: u32, img: &[u8]) -> [u8; 32] {
    if version >= SHA256_VERSION { return sha256(img); }
    let mut d = [0u8; 32];
    d[..4].copy_from_slice(&crc32(&img[..img.len().min(512)]).to_le_bytes());
    d
}

// ── picture check ───────────────────────────────────────────────────────────

/// (width, height) of a well-formed baseline JPEG (SOF0, 8-bit, 1 or 3 components); anything else
/// — progressive, arithmetic, truncated, trailing data — is an error.
fn jpeg_baseline_size(b: &[u8]) -> Result<(u32, u32), String> {
    if b.len() < 4 || b[..2] != [0xFF, 0xD8] || b[b.len() - 2..] != [0xFF, 0xD9] {
        return Err("not a complete JPEG file".into());
    }
    let mut i = 2;
    while i + 4 <= b.len() {
        if b[i] != 0xFF { return Err("malformed JPEG segment".into()); }
        let m = b[i + 1];
        if m == 0xFF { i += 1; continue; }
        if m == 0x01 || (0xD0..=0xD7).contains(&m) { i += 2; continue; }
        let len = u16::from_be_bytes([b[i + 2], b[i + 3]]) as usize;
        if len < 2 || i + 2 + len > b.len() { return Err("malformed JPEG segment".into()); }
        match m {
            0xC0 => {
                if len < 8 { return Err("malformed JPEG frame header".into()); }
                let s = &b[i + 4..];
                let (h, w, n) = (u16::from_be_bytes([s[1], s[2]]) as u32, u16::from_be_bytes([s[3], s[4]]) as u32, s[5]);
                if s[0] != 8 || !(n == 1 || n == 3) { return Err("only 8-bit greyscale or colour JPEGs are written".into()); }
                return Ok((w, h));
            }
            0xC1..=0xCF if !matches!(m, 0xC4 | 0xC8 | 0xCC) => return Err("only baseline JPEGs are written (this one is progressive or extended)".into()),
            0xDA | 0xD9 => return Err("JPEG has no frame header".into()),
            _ => i += 2 + len,
        }
    }
    Err("JPEG has no frame header".into())
}

fn decode_hex(s: &str) -> Option<Vec<u8>> {
    if s.len() % 2 != 0 { return None; }
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(s.get(i..i + 2)?, 16).ok()).collect()
}

// ── EFI system partition ────────────────────────────────────────────────────

/// "\040" etc. in /proc/self/mountinfo fields.
fn unescape_mount(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'\\' && i + 3 < b.len() {
            if let Ok(v) = u8::from_str_radix(&s[i + 1..i + 4], 8) { out.push(v); i += 4; continue; }
        }
        out.push(b[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// The writable vfat mount of the ESP. `part_type(maj:min)` is the GPT type udev recorded (None =
/// no udev data); without any udev data the single vfat mount with an EFI directory is taken.
fn esp_from_mountinfo(mi: &str, part_type: &dyn Fn(&str) -> Option<String>, has_efi_dir: &dyn Fn(&Path) -> bool)
    -> Result<PathBuf, String> {
    let mut typed = Vec::new();
    let mut untyped = Vec::new();
    for line in mi.lines() {
        let Some((pre, post)) = line.split_once(" - ") else { continue };
        let f: Vec<&str> = pre.split(' ').collect();
        if f.len() < 6 || post.split(' ').next() != Some("vfat") || f[3] != "/" { continue; }
        if !f[5].split(',').any(|o| o == "rw") { continue; }
        let mp = PathBuf::from(unescape_mount(f[4]));
        match part_type(f[2]) {
            Some(t) if t.eq_ignore_ascii_case(ESP_PART_TYPE) => typed.push(mp),
            Some(_) => {}
            None => untyped.push(mp),
        }
    }
    typed.dedup();
    if let [one] = typed.as_slice() { return Ok(one.clone()); }
    if typed.len() > 1 { return Err("more than one EFI system partition is mounted; not guessing".into()); }
    let cand: Vec<PathBuf> = untyped.into_iter().filter(|p| has_efi_dir(p)).collect();
    match cand.as_slice() {
        [one] => Ok(one.clone()),
        [] => Err("the EFI system partition is not mounted read-write (/efi, /boot/efi or /boot)".into()),
        _ => Err("several vfat partitions with an EFI directory are mounted; not guessing".into()),
    }
}

fn esp_mount() -> Result<PathBuf, String> {
    for p in ["/efi", "/boot/efi", "/boot"] { let _ = fs::read_dir(p); } // triggers systemd automounts
    let mi = fs::read_to_string("/proc/self/mountinfo").map_err(|e| format!("mountinfo: {e}"))?;
    let part_type = |devno: &str| -> Option<String> {
        let db = fs::read_to_string(format!("/run/udev/data/b{devno}")).ok()?;
        Some(db.lines().find_map(|l| l.strip_prefix("E:ID_PART_ENTRY_TYPE=")).unwrap_or("").to_owned())
    };
    let has_efi = |p: &Path| fs::read_dir(p).into_iter().flatten().flatten()
        .any(|e| e.file_name().eq_ignore_ascii_case("EFI") && e.path().is_dir());
    esp_from_mountinfo(&mi, &part_type, &has_efi)
}

/// <ESP>/EFI/Lenovo/Logo, matched case-insensitively (FAT) but created with this spelling.
fn logo_dir(esp: &Path) -> PathBuf {
    let mut cur = esp.to_path_buf();
    for part in LOGO_DIR.split('/') {
        let found = fs::read_dir(&cur).into_iter().flatten().flatten()
            .find(|e| e.file_name().eq_ignore_ascii_case(part) && e.path().is_dir());
        cur = found.map(|e| e.path()).unwrap_or_else(|| cur.join(part));
    }
    cur
}

fn logo_files(dir: &Path) -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = fs::read_dir(dir).into_iter().flatten().flatten()
        .filter(|e| e.file_type().map_or(false, |t| t.is_file())).map(|e| e.path()).collect();
    v.sort();
    v
}

fn remove_logo_files(dir: &Path) -> Result<(), String> {
    for p in logo_files(dir) { fs::remove_file(&p).map_err(|e| format!("{}: {e}", p.display()))?; }
    Ok(())
}

fn write_file_synced(p: &Path, data: &[u8]) -> Result<(), String> {
    let mut f = fs::File::create(p).map_err(|e| format!("{}: {e}", p.display()))?;
    f.write_all(data).and_then(|_| f.sync_all()).map_err(|e| format!("{}: {e}", p.display()))?;
    if let Some(d) = p.parent() { let _ = fs::File::open(d).and_then(|d| d.sync_all()); }
    Ok(())
}

// ── operations ──────────────────────────────────────────────────────────────

/// health-reader {"op":"boot_logo"}: read-only.
pub fn status() -> Value {
    let desc = match fs::read(DESC_VAR).ok().as_deref().and_then(parse_desc) {
        Some(d) => d,
        None => return json!({"ok": true, "supported": false}),
    };
    let version = fs::read(SUM_VAR).ok().filter(|b| b.len() == SUM_LEN).map(|b| u32_at(&b, 4));
    let mut out = json!({
        "ok": true, "supported": version.is_some(), "enabled": desc.enabled,
        "width": desc.width, "height": desc.height, "formats": format_names(desc.formats),
        "jpeg": desc.formats & FMT_JPEG != 0,
        "checksum": version.map(|v| if v >= SHA256_VERSION { "sha256" } else { "crc32" }),
    });
    match esp_mount() {
        Ok(esp) => {
            let files: Vec<String> = logo_files(&logo_dir(&esp)).iter().map(|p| p.display().to_string()).collect();
            out["esp"] = json!(esp.display().to_string());
            out["files"] = json!(files);
        }
        Err(e) => out["esp_error"] = json!(e),
    }
    out
}

/// legion-firmware-helper {"op":"boot_logo_set","jpeg_hex":"ffd8…"}.
pub fn set(jpeg_hex: &str) -> Value {
    match set_inner(jpeg_hex) {
        Ok(v) => v,
        Err((e, state)) => json!({"ok": false, "error": e, "state": state}),
    }
}

fn set_inner(jpeg_hex: &str) -> Result<Value, (String, &'static str)> {
    let orig = |e: String| (e, "original");
    if jpeg_hex.len() > 2 * MAX_LOGO { return Err(orig(format!("picture larger than {} MiB", MAX_LOGO >> 20))); }
    let img = decode_hex(jpeg_hex).ok_or_else(|| orig("'jpeg_hex' is not hex".into()))?;
    let desc_var = read_var(DESC_VAR, DESC_LEN).map_err(orig)?;
    let sum_var = read_var(SUM_VAR, SUM_LEN).map_err(orig)?;
    let desc = parse_desc(&desc_var).ok_or_else(|| orig("LBLDESP holds no usable resolution; not touched".into()))?;
    if desc.formats & FMT_JPEG == 0 { return Err(orig("the firmware does not accept JPEG boot logos".into())); }
    let (w, h) = jpeg_baseline_size(&img).map_err(orig)?;
    if (w, h) != (desc.width, desc.height) {
        return Err(orig(format!("picture is {w}×{h}; the firmware wants exactly {}×{}", desc.width, desc.height)));
    }
    let dir = logo_dir(&esp_mount().map_err(orig)?);
    let file = dir.join(format!("mylogo_{w}x{h}.jpg"));
    crate::wlog::log("bootlogo", &format!("set {} ({} bytes)", file.display(), img.len()));

    fs::create_dir_all(&dir).map_err(|e| orig(format!("{}: {e}", dir.display())))?;
    // A previous custom logo is kept in memory so a failed step can put it back.
    let prev: Vec<(PathBuf, Vec<u8>)> = logo_files(&dir).into_iter()
        .map(|p| fs::read(&p).map(|b| (p.clone(), b)).map_err(|e| orig(format!("{}: {e}", p.display()))))
        .collect::<Result<_, _>>()?;
    let restore_files = || -> bool {
        let _ = remove_logo_files(&dir);
        prev.iter().all(|(p, b)| write_file_synced(p, b).is_ok())
    };
    let undo = |e: String, vars_back: bool| (e, if vars_back && restore_files() { "original" } else { "unknown" });
    remove_logo_files(&dir).map_err(|e| undo(e, true))?;
    write_file_synced(&file, &img).map_err(|e| undo(e, true))?;

    let version = u32_at(&sum_var, 4);
    let mut new_sum = sum_var.clone();
    new_sum[8..40].copy_from_slice(&digest(version, &img));
    if let Err(e) = write_var(SUM_VAR, &new_sum) {
        let back = restore_var(SUM_VAR, &sum_var);
        return Err(undo(e, back));
    }
    let mut new_desc = desc_var.clone();
    new_desc[4] = 1;
    if let Err(e) = write_var(DESC_VAR, &new_desc) {
        let back = restore_var(DESC_VAR, &desc_var) & restore_var(SUM_VAR, &sum_var);
        return Err(undo(e, back));
    }
    Ok(json!({"ok": true, "file": file.display().to_string(),
              "checksum": if version >= SHA256_VERSION { "sha256" } else { "crc32" }, "reboot_required": true}))
}

/// legion-firmware-helper {"op":"boot_logo_reset"}: back to the Lenovo logo. The enable flag goes
/// first (with it off the firmware ignores the rest), then the digest (zeroed, version kept), then
/// the files.
pub fn reset() -> Value {
    let r = (|| -> Result<Value, String> {
        let desc_var = read_var(DESC_VAR, DESC_LEN)?;
        let sum_var = read_var(SUM_VAR, SUM_LEN)?;
        crate::wlog::log("bootlogo", "reset");
        if desc_var[4] != 0 {
            let mut d = desc_var.clone();
            d[4] = 0;
            write_var(DESC_VAR, &d)?;
        }
        if sum_var[8..40].iter().any(|&b| b != 0) {
            let mut s = sum_var.clone();
            s[8..40].fill(0);
            write_var(SUM_VAR, &s)?;
        }
        let removed = match esp_mount() {
            Ok(esp) => { let dir = logo_dir(&esp); let n = logo_files(&dir).len(); remove_logo_files(&dir)?; json!(n) }
            Err(_) => Value::Null,
        };
        Ok(json!({"ok": true, "files_removed": removed, "reboot_required": true}))
    })();
    r.unwrap_or_else(|e| json!({"ok": false, "error": e}))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex(b: &[u8]) -> String { b.iter().map(|x| format!("{x:02x}")).collect() }

    #[test]
    fn sha256_vectors() {
        assert_eq!(hex(&sha256(b"")), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        assert_eq!(hex(&sha256(b"abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        assert_eq!(hex(&sha256(b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
                   "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
        assert_eq!(hex(&sha256(&[b'a'; 1000])), "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
    }

    #[test]
    fn crc_and_digest() {
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
        let img: Vec<u8> = (0..2000u32).map(|i| i as u8).collect();
        let old = digest(0x0002_0002, &img);
        assert_eq!(old[..4], crc32(&img[..512]).to_le_bytes());
        assert!(old[4..].iter().all(|&b| b == 0));
        assert_eq!(digest(SHA256_VERSION, &img), sha256(&img));
    }

    #[test]
    fn desc_as_read_on_16afr10h() {
        let var = [7, 0, 0, 0, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x40, 0x06, 0x00, 0x00, 0x3f];
        assert_eq!(parse_desc(&var), Some(Desc { enabled: false, width: 2560, height: 1600, formats: 0x3f }));
        assert_eq!(format_names(0x3f), vec!["jpeg", "gif", "bmp", "png"]);
        assert_eq!(parse_desc(&var[..13]), None);
    }

    fn jpeg(sof: u8, w: u16, h: u16, comps: u8) -> Vec<u8> {
        let mut b = vec![0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x4A, 0x46];          // SOI, APP0 (short)
        b.extend([0xFF, 0xDB, 0x00, 0x03, 0x00]);                                   // DQT (short)
        b.extend([0xFF, sof, 0x00, 0x0B, 0x08]);
        b.extend(h.to_be_bytes());
        b.extend(w.to_be_bytes());
        b.extend([comps, 0x01, 0x22, 0x00]);
        b.extend([0xFF, 0xDA, 0x00, 0x02, 0x12, 0x34, 0xFF, 0xD9]);                 // SOS, data, EOI
        b
    }

    #[test]
    fn jpeg_checks() {
        assert_eq!(jpeg_baseline_size(&jpeg(0xC0, 2560, 1600, 3)), Ok((2560, 1600)));
        assert!(jpeg_baseline_size(&jpeg(0xC2, 2560, 1600, 3)).unwrap_err().contains("baseline"));
        assert!(jpeg_baseline_size(&jpeg(0xC0, 2560, 1600, 4)).is_err());
        let mut cut = jpeg(0xC0, 10, 10, 3);
        cut.truncate(cut.len() - 1);
        assert!(jpeg_baseline_size(&cut).is_err());
        assert!(jpeg_baseline_size(b"\x89PNG\r\n\x1a\n").is_err());
        assert_eq!(decode_hex("ffD8"), Some(vec![0xFF, 0xD8]));
        assert_eq!(decode_hex("fz"), None);
        assert_eq!(decode_hex("f"), None);
    }

    #[test]
    fn esp_selection() {
        let mi = "\
22 1 259:2 / / rw,relatime shared:1 - ext4 /dev/nvme0n1p2 rw
30 22 259:1 / /boot/efi rw,relatime shared:2 - vfat /dev/nvme0n1p1 rw,fmask=0077
31 22 8:1 / /run/media/u/STICK rw,nosuid shared:3 - vfat /dev/sda1 rw
32 22 8:2 / /mnt/ro\\040esp ro,relatime shared:4 - vfat /dev/sda2 ro
";
        let typed = |d: &str| Some(if d == "259:1" { ESP_PART_TYPE.to_owned() } else { "0fc63daf-8483-4772-8e79-3d69d8477de4".to_owned() });
        assert_eq!(esp_from_mountinfo(mi, &typed, &|_| true), Ok(PathBuf::from("/boot/efi")));
        // No udev data: the one vfat mount with an EFI directory.
        let none = |_: &str| None;
        assert_eq!(esp_from_mountinfo(mi, &none, &|p| p == Path::new("/boot/efi")), Ok(PathBuf::from("/boot/efi")));
        assert!(esp_from_mountinfo(mi, &none, &|_| true).is_err());
        assert!(esp_from_mountinfo(mi, &|_| Some(String::new()), &|_| true).is_err());
        assert_eq!(unescape_mount("/mnt/ro\\040esp"), "/mnt/ro esp");
    }
}
