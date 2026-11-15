# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions follow the
`[workspace.package] version` in `Cargo.toml`, which the GUI and every crate share.

## [Unreleased]

### Security
- Root helpers have a hard run-time bound (`watchdog`): the GUI cannot kill a helper once
  pkexec has started it as root, so it no longer runs on unobserved after a GUI timeout.
- `fwattr-helper`, `ryzen-co-helper` and `legion-profile-helper` serialise their writes
  with a per-helper lock (`/run/centurion/<helper>.lock`).
- systemd units: `SystemCallFilter=~@reboot @obsolete @cpu-emulation`; `centurion-tune`
  also drops capabilities it never uses.
- `deny.toml`: RustSec advisories, licences and sources checked by `cargo deny` in CI.

### Fixed
- Health → Events & alerts: a dGPU that fails to wake from D3cold is reported when it happens. The GUI's
  kernel-log filter did not know that line, so the (critical) event waited for the next unrelated one.
- Health → Overview, *Sleep drain*: without a sleep long enough on battery to measure, the card says so
  instead of "draining 0.0 % per hour"; the count is worded as what it is (all sleeps of the last 7 days).
- Health → System tools: pages are grouped by their untranslated name. In Turkish *Thermal* ("Termal")
  was filed under *Kernel & firmware* instead of *Devices*.
- Health → Drive health: the "not installed" hint names this distribution's package manager (it said
  `emerge` everywhere).
- `centurion-calibrate`: a phase that ends early (Ctrl-C, input during a boot calibration, the
  charger gone, the ballast braked, the write budget) keeps the runs measured since its last
  commit; they were dropped, up to a whole space-filling stage.
- `centurion-calibrate`: a numeric option with a missing, malformed or negative value is an
  error instead of the default (`--budget nan` ran without a time limit).
- Autotune's "changed since last run" names new calibration data as the cause on a signature
  at its row cap too (it compared the row count only).
- `writes.log` rotation is locked and keeps two generations (`.1`, `.2`); two helpers
  rotating at once could lose the previous log.
- Untranslated pkexec/polkit error messages and the Blackwell hotspot tooltip.
- NVIDIA tab tooltips are translatable (Turkish added).
- Loading an NVIDIA profile rejects non-integer, non-finite or out-of-range curve deltas again.

### Changed
- Single-colour label styles use the theme's `tone` property instead of inline stylesheets.
- One version for GUI and crates (crates were 0.1.0). Helpers report `helper_version`;
  the GUI warns once when it differs from its own.
- Launcher detection looks every 30 s instead of 15 s while no launcher runs.
- `EXPERIMENTAL-*.txt` moved to `docs/`; the outdated root `HANDBOOK.md` removed
  (`docs/HANDBOOK.md` is the current one).

### Added
- Health → Overview: *Drive health* card, from the daily SMART record (`smart.json`): drives with a SMART
  warning, wear or new media errors in the last 30 days. Read-only; SMART itself is still read only by
  *Drive health* and the daily check.
- Diagnostic report: IPv6 addresses (`<ip>`) and the user name in `/home/…` paths (`<user>`) are masked too.
- Autotune and `centurion-calibrate --show` warn when the confirmation/verify measurements fall
  short of the predictions systematically (optimistic predictions); `centurion-autotune <goal>`
  prints the application advice in its text output.
- `centurion-export prometheus [FILE]` / `mqtt HOST`: the D-Bus `SensorValues` for Prometheus
  (textfile collector) and MQTT (`mosquitto_pub`, retained); no port is opened.
- Tray: *Check for updates…* — one request to the GitHub releases API, only when clicked.
- Accessible names/descriptions for the painted widgets (V/F curve, fan curve, keyboard, charts);
  the charts describe their data to screen readers.
- `cargo-fuzz` targets (`fuzz/`: tarscan, tune-helper values, amdgpu OD) and a `clang-tidy`
  CMake target (`.clang-tidy`); clippy (`-D warnings`) and the nvcurve static test run in CI.
- CI (`.github/workflows/ci.yml`): Rust tests, cargo-deny, GUI build and tests under
  ASan/UBSan (`-DCENTURION_SANITIZE=ON`).
- AppStream metainfo (`io.github.arabcian.centurion.metainfo.xml`).
- logrotate configuration for `calibrate-boot.log` (the only log that does not trim itself).

## [2.0.0] - 2026-10-24
