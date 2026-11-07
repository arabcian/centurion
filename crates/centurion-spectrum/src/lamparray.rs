//! HID LampArray (usage page 0x59, Windows "Dynamic Lighting") on the Legion
//! lighting controller — USB 048D:C193 on the Legion Pro 7 16AFR10H, next to
//! the Spectrum interface. Host-driven: colours live in the controller's RAM
//! while autonomous mode is off and the stored Spectrum profile is untouched
//! (verified on 16AFR10H: the Spectrum state read back identical, and the
//! keyboard resumed its own effect once autonomous mode was switched back on).
//!
//! 16AFR10H: 319 lamps (0–101 bound to keys by HID usage, 102–318 the front
//! and rear accent strips), minimum update interval 32.2 ms.
//!
//! Report ids are the ones that controller declares; [`find`] only accepts an
//! interface whose descriptor maps the same ids to the same LampArray usages.
//! LampMultiUpdate is taken to carry 8 lamps, as that descriptor declares.

use super::{hid_ids, hidraw_dirs, is_spectrum_product, Error, Result, Rgb};
use std::fs::{self, File, OpenOptions};
use std::io;
use std::os::fd::AsRawFd;
use std::path::{Path, PathBuf};

const ATTRIBUTES: u8 = 1; // LampArrayAttributesReport
const LAMP_REQUEST: u8 = 2; // LampAttributesRequestReport
const LAMP_RESPONSE: u8 = 3; // LampAttributesResponseReport
const MULTI_UPDATE: u8 = 4; // LampMultiUpdateReport
const RANGE_UPDATE: u8 = 5; // LampRangeUpdateReport
const CONTROL: u8 = 6; // LampArrayControlReport
const MULTI_LAMPS: usize = 8;
const UPDATE_COMPLETE: u8 = 1;
/// Report id → usage pairs (`85 id 09 usage`) the descriptor must contain.
const LAYOUT: [[u8; 4]; 6] = [
    [0x85, ATTRIBUTES, 0x09, 0x02], [0x85, LAMP_REQUEST, 0x09, 0x20], [0x85, LAMP_RESPONSE, 0x09, 0x22],
    [0x85, MULTI_UPDATE, 0x09, 0x50], [0x85, RANGE_UPDATE, 0x09, 0x60], [0x85, CONTROL, 0x09, 0x70],
];

fn ioc(nr: u64, len: usize) -> u64 { super::hid_ioc(nr, len) }

/// Descriptor of a LampArray top-level collection with the expected report ids.
fn is_lamparray(desc: &[u8]) -> bool {
    desc.starts_with(&[0x05, 0x59, 0x09, 0x01]) && LAYOUT.iter().all(|p| desc.windows(4).any(|w| w == p))
}

fn find_dir() -> Option<PathBuf> {
    hidraw_dirs().into_iter().find(|d| {
        hid_ids(d).map_or(false, |(v, p)| is_spectrum_product(v, p))
            && fs::read(d.join("device/report_descriptor")).map_or(false, |b| is_lamparray(&b))
    })
}

/// hidraw node of the LampArray interface (sysfs only, nothing is opened).
pub fn find() -> Option<PathBuf> {
    find_dir().map(|d| Path::new("/dev").join(d.file_name().unwrap()))
}

/// USB product id of the device carrying the LampArray interface (sysfs only).
pub(crate) fn product() -> Option<u16> {
    hid_ids(&find_dir()?).map(|(_, p)| p)
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Lamp {
    pub id: u16,
    /// Position in micrometres.
    pub x: u32,
    pub y: u32,
    pub z: u32,
    /// LampPurposes bits: 1 control, 2 accent, 4 branding, 8 status, 0x10 illumination.
    pub purposes: u32,
    /// HID keyboard usage the lamp sits under, 0 = not on a key.
    pub key: u8,
}

pub struct LampArray {
    file: File,
    path: PathBuf,
    count: u16,
    min_update_us: u32,
}

impl LampArray {
    pub fn open() -> Result<Self> {
        let path = find().ok_or(Error::NotFound)?;
        let file = OpenOptions::new().read(true).write(true).open(&path)?;
        let mut a = LampArray { file, path, count: 0, min_update_us: 0 };
        let b = a.get(ATTRIBUTES, 23)?;
        a.count = u16::from_le_bytes([b[1], b[2]]);
        a.min_update_us = u32::from_le_bytes([b[19], b[20], b[21], b[22]]);
        if a.count == 0 { return Err(Error::BadReport("LampArray reports 0 lamps")); }
        Ok(a)
    }

    pub fn path(&self) -> &Path { &self.path }
    pub fn count(&self) -> u16 { self.count }
    pub fn min_update_us(&self) -> u32 { self.min_update_us }
    pub fn raw_fd(&self) -> i32 { self.file.as_raw_fd() }

    fn set(&self, buf: &[u8]) -> Result<()> {
        if unsafe { libc::ioctl(self.file.as_raw_fd(), ioc(0x06, buf.len()) as _, buf.as_ptr()) } < 0 {
            return Err(io::Error::last_os_error().into());
        }
        Ok(())
    }

    fn get(&self, id: u8, len: usize) -> Result<Vec<u8>> {
        let mut b = vec![0u8; len];
        b[0] = id;
        if unsafe { libc::ioctl(self.file.as_raw_fd(), ioc(0x07, len) as _, b.as_mut_ptr()) } < 0 {
            return Err(io::Error::last_os_error().into());
        }
        Ok(b)
    }

    pub fn lamps(&self) -> Result<Vec<Lamp>> {
        (0..self.count).map(|i| {
            self.set(&[LAMP_REQUEST, i as u8, (i >> 8) as u8])?;
            parse_lamp(&self.get(LAMP_RESPONSE, 29)?)
        }).collect()
    }

    /// Off = the host drives the lamps (RAM only); on = the controller's own (Spectrum) effect.
    pub fn set_autonomous(&self, on: bool) -> Result<()> { self.set(&control(on)) }

    /// One frame: a colour per lamp, in lamp-id order.
    pub fn show(&self, colors: &[Rgb]) -> Result<()> {
        if colors.len() != self.count as usize { return Err(Error::Range("one colour per lamp")); }
        frame_reports(colors).iter().try_for_each(|r| self.set(r))
    }
}

pub fn control(autonomous: bool) -> [u8; 2] { [CONTROL, autonomous as u8] }

/// Autonomous mode back on through a raw fd: one ioctl, nothing else, so it is
/// safe inside a signal handler (the stream's last resort on SIGTERM / abort).
pub fn restore_autonomous_fd(fd: i32) {
    let b = control(true);
    unsafe { libc::ioctl(fd, ioc(0x06, b.len()) as _, b.as_ptr()); }
}

fn parse_lamp(b: &[u8]) -> Result<Lamp> {
    if b.len() < 29 || b[0] != LAMP_RESPONSE { return Err(Error::BadReport("LampAttributesResponse")); }
    let u32_at = |o: usize| u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]]);
    Ok(Lamp { id: u16::from_le_bytes([b[1], b[2]]), x: u32_at(3), y: u32_at(7), z: u32_at(11), purposes: u32_at(19), key: b[28] })
}

/// Reports for one frame. Runs of 8+ equal colours go as one range update, everything else as
/// multi updates of 8 lamps; only the last report carries "update complete", so the frame shows at once.
pub fn frame_reports(colors: &[Rgb]) -> Vec<Vec<u8>> {
    let mut out = Vec::new();
    let mut pending: Vec<(u16, Rgb)> = Vec::new();
    let mut i = 0;
    while i < colors.len() {
        let mut j = i + 1;
        while j < colors.len() && colors[j] == colors[i] { j += 1; }
        if j - i >= MULTI_LAMPS {
            out.push(range(i as u16, (j - 1) as u16, colors[i]));
        } else {
            pending.extend((i..j).map(|k| (k as u16, colors[k])));
        }
        i = j;
    }
    out.extend(pending.chunks(MULTI_LAMPS).map(multi));
    if let Some(last) = out.last_mut() {
        let flags = if last[0] == RANGE_UPDATE { 1 } else { 2 };
        last[flags] |= UPDATE_COMPLETE;
    }
    out
}

fn rgbi(c: Rgb) -> [u8; 4] { [c.0, c.1, c.2, 1] }

fn range(first: u16, last: u16, c: Rgb) -> Vec<u8> {
    let mut b = vec![RANGE_UPDATE, 0];
    b.extend(first.to_le_bytes());
    b.extend(last.to_le_bytes());
    b.extend(rgbi(c));
    b
}

fn multi(lamps: &[(u16, Rgb)]) -> Vec<u8> {
    let mut b = vec![0u8; 3 + 2 * MULTI_LAMPS + 4 * MULTI_LAMPS];
    b[0] = MULTI_UPDATE;
    b[1] = lamps.len() as u8;
    for (k, (id, c)) in lamps.iter().enumerate() {
        b[3 + 2 * k..5 + 2 * k].copy_from_slice(&id.to_le_bytes());
        b[19 + 4 * k..23 + 4 * k].copy_from_slice(&rgbi(*c));
    }
    b
}

#[cfg(test)]
mod tests {
    use super::*;

    // hidraw7 of a Legion Pro 7 16AFR10H (048D:C193).
    const DESC_16AFR10H: &str = "05 59 09 01 a1 01 85 01 09 02 a1 02 09 03 15 00 27 ff ff 00 00 75 10 95 01 b1 03 09 04 09 05 09 \
        06 09 07 09 08 15 00 27 ff ff ff 7f 75 20 95 05 b1 03 c0 85 02 09 20 a1 02 09 21 15 00 27 ff ff \
        00 00 75 10 95 01 b1 02 c0 85 03 09 22 a1 02 09 21 15 00 27 ff ff 00 00 75 10 95 01 b1 02 09 23 \
        09 24 09 25 09 27 09 26 15 00 27 ff ff ff 7f 75 20 95 05 b1 02 09 28 09 29 09 2a 09 2b 09 2c 09 \
        2d 15 00 26 ff 00 75 08 95 06 b1 02 c0 85 04 09 50 a1 02 09 03 09 55 15 00 25 08 75 08 95 02 b1 \
        02 09 21 15 00 27 ff ff 00 00 75 10 95 08 b1 02 09 51 09 52 09 53 09 54 09 51 09 52 09 53 09 54 \
        15 00 26 ff 00 75 08 95 20 b1 02 c0 85 05 09 60 a1 02 09 55 15 00 25 08 75 08 95 01 b1 02 09 61 \
        09 62 15 00 27 ff ff 00 00 75 10 95 02 b1 02 09 51 09 52 09 53 09 54 15 00 26 ff 00 75 08 95 04 \
        b1 02 c0 85 06 09 70 a1 02 09 71 15 00 25 01 75 08 95 01 b1 02 c0 c0";

    fn bytes(s: &str) -> Vec<u8> { s.split_whitespace().map(|h| u8::from_str_radix(h, 16).unwrap()).collect() }

    #[test]
    fn descriptor_match() {
        let d = bytes(DESC_16AFR10H);
        assert!(is_lamparray(&d));
        // The Spectrum interface of the same device (vendor page 0xFF89) is not a LampArray.
        assert!(!is_lamparray(&bytes("06 89 ff 09 11 a1 01 85 5a 09 01 15 00 26 ff 00")));
        // Same usages under other report ids: refused rather than guessed.
        let mut moved = d.clone();
        let at = moved.windows(4).position(|w| w == [0x85, 0x06, 0x09, 0x70]).unwrap();
        moved[at + 1] = 0x07;
        assert!(!is_lamparray(&moved));
    }

    #[test]
    fn lamp_response() {
        // Lamp 0 of the 16AFR10H: Esc at (21.8, 67.0, 10.0) mm, purpose control, usage 0x29.
        let mut b = vec![3u8, 0, 0];
        for v in [21_800u32, 67_000, 10_000, 0, 1] { b.extend(v.to_le_bytes()); }
        b.extend([0xff, 0xff, 0xff, 1, 1, 0x29]);
        assert_eq!(parse_lamp(&b).unwrap(), Lamp { id: 0, x: 21_800, y: 67_000, z: 10_000, purposes: 1, key: 0x29 });
        assert!(parse_lamp(&b[..20]).is_err());
    }

    #[test]
    fn control_report() {
        assert_eq!(control(false), [6, 0]);
        assert_eq!(control(true), [6, 1]);
    }

    #[test]
    fn uniform_frame_is_one_range_update() {
        let r = frame_reports(&[Rgb(1, 2, 3); 319]);
        assert_eq!(r, vec![vec![5, 1, 0, 0, 0x3e, 0x01, 1, 2, 3, 1]]);
    }

    #[test]
    fn mixed_frame() {
        // 3 single lamps, a run of 10, 1 single: one range + one multi (4 lamps), complete on the last.
        let mut c = vec![Rgb(9, 0, 0), Rgb(0, 9, 0), Rgb(0, 0, 9)];
        c.extend([Rgb(5, 5, 5); 10]);
        c.push(Rgb(7, 7, 7));
        let r = frame_reports(&c);
        assert_eq!(r.len(), 2);
        assert_eq!(r[0], vec![5, 0, 3, 0, 12, 0, 5, 5, 5, 1]);
        let m = &r[1];
        assert_eq!(m.len(), 51);
        assert_eq!(&m[..3], &[4, 4, UPDATE_COMPLETE]);
        assert_eq!(&m[3..11], &[0, 0, 1, 0, 2, 0, 13, 0]);
        assert_eq!(&m[19..35], &[9, 0, 0, 1, 0, 9, 0, 1, 0, 0, 9, 1, 7, 7, 7, 1]);
        assert!(m[35..].iter().all(|&x| x == 0));
    }

    #[test]
    fn multi_updates_split_by_eight() {
        let c: Vec<Rgb> = (0..20u8).map(|i| Rgb(i, 0, 0)).collect();
        let r = frame_reports(&c);
        assert_eq!(r.iter().map(|m| m[1]).collect::<Vec<_>>(), vec![8, 8, 4]);
        assert_eq!(r.iter().map(|m| m[2]).collect::<Vec<_>>(), vec![0, 0, UPDATE_COMPLETE]);
    }
}
