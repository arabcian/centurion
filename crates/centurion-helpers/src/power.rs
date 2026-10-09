//! Power source of the machine, shared by every tool.

use std::time::{Duration, Instant};

/// Mains or USB-PD online → AC; chargers present but offline → battery;
/// no chargers reported → go by the battery status. Same rule as the GUI (scenes::onAc).
/// The one implementation for every Rust tool (centurion-gamemode, the Intel undervolt daemon, autotune): they
/// used to carry three different rules (Mains only, read error = battery, ...).
///
/// A sysfs attribute that exists but cannot be read (the EC is busy answering _PSR/_BST) is NOT "0":
/// it used to be treated as "offline", i.e. a failed read meant "on battery". Now it means "cannot tell"
/// (None). Batteries with Device scope (wireless mice, headsets) are not the machine's own power.
pub fn on_ac() -> Option<bool> {
    let (mut any_supply, mut any_bat, mut discharging) = (false, false, false);
    let (mut supply_unreadable, mut bat_unreadable) = (false, false);
    for e in std::fs::read_dir("/sys/class/power_supply").ok()?.flatten() {
        // Ok(None) = no such attribute; Err(()) = it exists but the read failed (or came back empty).
        let rd = |f: &str| -> Result<Option<String>, ()> {
            match std::fs::read_to_string(e.path().join(f)) {
                Ok(s) if s.trim().is_empty() => Err(()),
                Ok(s) => Ok(Some(s.trim().to_owned())),
                Err(err) if err.kind() == std::io::ErrorKind::NotFound => Ok(None),
                Err(_) => Err(()),
            }
        };
        if rd("scope").ok().flatten().as_deref() == Some("Device") { continue; }
        let ty = match rd("type") {
            Ok(Some(t)) => t,
            Ok(None) => continue,
            Err(()) => { supply_unreadable = true; continue; }
        };
        match ty.as_str() {
            "Mains" | "USB" => {
                any_supply = true;
                match rd("online") {
                    Ok(Some(v)) if v == "1" => return Some(true),
                    Err(()) => supply_unreadable = true,
                    _ => {}
                }
            }
            "Battery" => {
                any_bat = true;
                match rd("status") {
                    Ok(Some(v)) => discharging |= v == "Discharging",
                    Err(()) => bat_unreadable = true,
                    Ok(None) => {}
                }
            }
            _ => {}
        }
    }
    if supply_unreadable { return None; }
    if any_supply { return Some(false); }
    if bat_unreadable { return None; }
    if any_bat { Some(!discharging) } else { None }
}

/// on_ac() that rides out a busy EC: a few tries, 200 ms apart, before giving up.
pub fn on_ac_settled() -> Option<bool> {
    for i in 0..5 {
        if let Some(v) = on_ac() { return Some(v); }
        if i < 4 { std::thread::sleep(std::time::Duration::from_millis(200)); }
    }
    None
}

/// Debounce times, same as the GUI's SceneEngine: going to battery waits longer (a USB-C PD hard
/// reset or a brick that browns out under load drops "online" for a couple of seconds).
pub const DEBOUNCE_TO_BATTERY: Duration = Duration::from_millis(4000);
pub const DEBOUNCE_TO_AC: Duration = Duration::from_millis(2000);

/// Debounced power source for a polling loop. An unreadable source keeps the last known one;
/// a change must hold continuously for the debounce time before it is reported.
pub struct Source { ac: bool, candidate: Option<(bool, Instant)> }

impl Source {
    /// `fallback`: what to assume while the source has never been readable (no supplies at all = a desktop).
    pub fn new(fallback: bool) -> Source { Source { ac: on_ac_settled().unwrap_or(fallback), candidate: None } }
    pub fn ac(&self) -> bool { self.ac }
    /// Feeds one reading; returns the (debounced) source.
    pub fn feed(&mut self, now: Option<bool>, at: Instant) -> bool {
        match now {
            None => {}                                   // cannot tell: no change, the candidate keeps waiting
            Some(v) if v == self.ac => self.candidate = None,
            Some(v) => match self.candidate {
                Some((c, since)) if c == v => {
                    let need = if v { DEBOUNCE_TO_AC } else { DEBOUNCE_TO_BATTERY };
                    if at.duration_since(since) >= need { self.ac = v; self.candidate = None; }
                }
                _ => self.candidate = Some((v, at)),
            },
        }
        self.ac
    }
    pub fn poll(&mut self) -> bool { self.feed(on_ac(), Instant::now()) }
    /// Time until a pending change could be committed (poll again then), if one is pending.
    pub fn pending(&self) -> Option<Duration> {
        let (v, since) = self.candidate?;
        let need = if v { DEBOUNCE_TO_AC } else { DEBOUNCE_TO_BATTERY };
        Some(need.saturating_sub(since.elapsed()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn debounce() {
        let t0 = Instant::now();
        let mut s = Source { ac: true, candidate: None };
        assert!(s.feed(Some(false), t0));                               // first battery reading: not yet
        assert!(s.feed(None, t0 + Duration::from_secs(1)));             // unreadable: nothing changes
        assert!(s.feed(Some(false), t0 + Duration::from_secs(3)));      // 3 s < 4 s
        assert!(!s.feed(Some(false), t0 + Duration::from_secs(5)));     // held long enough
        assert!(!s.feed(Some(true), t0 + Duration::from_secs(6)));
        assert!(!s.feed(Some(false), t0 + Duration::from_secs(7)));     // blip back: candidate dropped
        assert!(!s.feed(Some(true), t0 + Duration::from_secs(8)));
        assert!(s.feed(Some(true), t0 + Duration::from_secs(10)));      // 2 s on AC
    }
}
