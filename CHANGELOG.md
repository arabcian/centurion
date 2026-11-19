# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions follow the
`[workspace.package] version` in `Cargo.toml`, which the GUI and every crate share.

## [Unreleased]

### Added
- Ryzen Curve → CO Search rewritten: all-core or per-core search, fast / medium / long levels, 60 s CPU cycles
  (`centurion-cpustress --load co`: 40 s of steady FFT / NTT load in the manner of Prime95 Small FFT and y-cruncher,
  then 20 s of load changes that move the clock and the idle states), single-core trials on the best-ranked cores,
  a confirmation pass, its own scene and temperature limit (default 95 °C), resume after a crash, and the result
  saved as a Ryzen Curve profile. Replaces the 20 s all-core SSE test.
- Health → Backups: distribution-aware defaults. `/etc/os-release` (ID / ID_LIKE) selects Gentoo, Arch (Manjaro,
  EndeavourOS, CachyOS), Debian/Ubuntu (Mint, Pop!_OS), Fedora/RHEL, openSUSE, Void or Alpine; the image's exclude
  list follows that layout (pacman sync DBs, apt lists, snap mounts, the Gentoo repository, snapper/Timeshift
  snapshots, core dumps). *Defaults ▾* offers this system's list, another distribution's, and optional exclusions
  (Steam games, caches, Docker/Podman, VM disks, journal, Ollama models…). Active swap files are always left out
  (`distro.rs`, mirrored by `gui/src/distroprofile.cpp`, kept in step by a test). A list saved with the old
  built-in default follows the new one; edited lists are kept.
  An incremental image whose exclude list or /home choice differs from its chain's starts a new full image
  (the sidecar keeps the chain's `rules`), so a chain restore never mixes two different selections.
- System profile: the package manager configuration and package list of every supported family (pacman, dpkg,
  rpm, xbps, apk; package queries time out after 20 s), plus `packages-explicit.txt` (world set / explicitly
  installed). Network configuration (netplan, ifupdown, ifcfg), which can hold Wi-Fi passphrases, is not copied. Install hints (age, polkit,
  nftables, acpi_call) and the restore-target placeholder follow the distribution.
- Events & alerts: an explanation pane and dialog per event — meaning, usual causes, steps, urgency and details
  read from the record (process, PCI device, drive, killed process, kernel function) — for every Xid group, GSP,
  MCE, AER, lockups, NVMe, file systems, i915/xe/amdgpu, Wi-Fi, thermal, OOM, oops and D3cold (`eventinfo.cpp`).
- Kernel log: 50 lines of context before an event (was 20), *Show the whole log*, `a|b` filters, quick filters
  per subsystem, *Save view…*, *What does this mean?*, *Copy this line*, *Show only lines like this*.
- Boot history: filter (unclean / events / curves), patterns (unclean rate per curve, session lengths), comparison
  with the previous boot, *Kernel log of this boot*, *Copy details*, *Save report…*; 50 saved kernel lines.
- Network protection: *Firewall* tab (default inbound / outbound policy, ordered port rules, live rule check with hit counters and other
  firewalls), open / LAN only / close from *Open ports*, blocked packets in the connection log; *Lookup* tab (whois for addresses, CIDR
  blocks, AS numbers and domains with routing data, DNS records, TCP port check, ping, traceroute).
- Network protection: *Open ports* tab (who can reach each listening socket, well-known services), CSV export
  of every list, rules export/import, traffic totals per program, busiest programs on the Connections tab.
- Drive health: power-on age, write rate, wear projection, heat and unsafe-shutdown notes.
- Health navigation: a tooltip per page.

### Security
- Root helpers have a hard run-time bound (`watchdog`): the GUI cannot kill a helper once
  pkexec has started it as root, so it no longer runs on unobserved after a GUI timeout.
- `fwattr-helper`, `ryzen-co-helper` and `legion-profile-helper` serialise their writes
  with a per-helper lock (`/run/centurion/<helper>.lock`).
- systemd units: `SystemCallFilter=~@reboot @obsolete @cpu-emulation`; `centurion-tune`
  also drops capabilities it never uses.
- `deny.toml`: RustSec advisories, licences and sources checked by `cargo deny` in CI.

### Fixed
- `centurion-calibrate`: `--budget`/`--io-budget` refuse infinite (`inf`, `1e400`) and oversized values.
  `--io-budget inf` turned the daily write budget that protects the drive off (it saturated to `u64::MAX`).
- `centurion-calibrate --verify`: an interrupt is a failed verification wherever it lands. Between two
  runs it left no error, and a stopped verification could be recorded as passed.
- `centurion-calibrate --verify`: an OOM kill under the preset fails the verification and is reported
  (`oom_kills` in the record); the run was dropped without a word and the preset could still pass.
- `centurion-calibrate`: a run is dropped for severe interference on its final weight, after the SLC and
  display factors (a run at 0.2 x 0.3 was kept while one at 0.125 was dropped).
- `centurion-calibrate`: a phase's quality record counts its own thrown-away runs, refusals, power waits
  and outside activity (they were the session's running totals); a refused value is no longer also
  counted as a run thrown away.
- Autotune: an unknown `mode` is an error (it ran the most expensive mode, deep); with nothing to change
  the advice says so; `autotune-seen.json` is replaced whole (a torn write lost every goal's hysteresis).
- Optimizations → Explore…: computes the trade-offs of the result on screen without reloading the table
  (the user's edits were lost) or switching the mode to Balanced; it adds no second history entry. The
  advice line names how many settings would change, which is what its verdict is judged on.
- The "seen" threshold is called Bonferroni (it was called Šidák; the two differ by < 0.01 here).
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
- `centurion-calibrate`: progressive sessions leave the base stage again after an OOM kill or a value
  the kernel refuses. A value no run can hold (unsafe on its own, or refused in the phase's newest
  session) stayed below 6 runs for good, so every later session was a base session: no pairs, no
  crowding, no heat, no dose refinement.
- `centurion-calibrate`: the storage phase is not planned on a drive without a readable temperature
  sensor (no storage run starts without one). Every run was skipped one by one and the phase spent its
  share of the session on nothing - without end under `--all`; its time now goes to the other phases.
- `centurion-calibrate`: 8 runs thrown away in a row (a machine that stays busy, a drive that stays
  hot, firmware power capping, `CENTURION_PREFLIGHT_STRICT`) end the phase with what it measured,
  instead of spending the rest of its budget on runs that are dropped.
- `centurion-calibrate --verify`: an error that ends the runs (the charger gone, the ballast braked,
  the write budget) reports and keeps the ABBA blocks measured so far; the verification still fails.
- The optimistic-predictions warning counts every session of a `--sessions N` run or a boot
  calibration: their confirmation checks were one group (the time the signature was loaded), so a
  night of sessions never reached the six groups the warning needs.
- `centurion-calibrate`: the measured cost per run takes out the heat soaks as timed, not "hot runs x
  soak length" (hot runs that follow each other only top up): the next session planned more runs
  than fit.
- Autotune's application advice is judged on the settings that would change something. It counted
  every decided setting, so every report said "high-impact" (12 settings or more) and the two
  milder verdicts could not be reached; the reply also carries `changes`.
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
