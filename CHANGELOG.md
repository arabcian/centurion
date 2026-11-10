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
