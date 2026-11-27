# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions follow the
`[workspace.package] version` in `Cargo.toml`, which the GUI and every crate share.

## [Unreleased]

### Security
- Unlock PIN: the helpers that write new values to the hardware or the kernel (tune-helper, intel-uv-helper,
  ryzen-co-helper, amdgpu-helper, legion-gpu-helper fan/power, legion-profile-helper fan table / fan targets /
  firmware switches, nvcurve-root-helper, fwattr-helper) stay password-free in polkit but refuse those writes unless
  the request comes from the Centurion window unlocked with a 4-digit PIN (lock button; a refused write asks for it).
  Closing the window locks it again; scene steps keep running from the tray. The PIN is checked by root
  (salted, iterated SHA-256 in `/etc/centurion/pin.json`, 0600); from the 5th wrong PIN each try waits longer, the
  10th disables it until it is set again with the administrator password. Reads, stored presets, resets and the
  boot services are not affected.
- Approved scene values: scene and CPU-profile files live in the user's home, so the gated steps of a scene
  (firmware limits, CPU curve / undervolt, fan table, GPU power) now write only values the user approved. Root keeps
  a per-user list of approved step digests (`/etc/centurion/scene-approved.json`); applying a scene from the Scenes
  tab or the tray with the window unlocked approves its values. Automatic applies (power source, login, resume,
  shortcut, D-Bus, drift watch) and anything while the window is closed reuse approved values only: an edited scene
  is refused until it is applied once from the unlocked window. After this update, apply each automatic scene once
  by hand.
- Scene `command` line: it now needs the same approval as the values root writes (new gated `tune-helper` op
  `scene_command`). Applying a scene from the unlocked window approves its command; automatic applies run it only if
  that exact line was approved before. Apply each scene that has a command once by hand.
- Keyboard lighting: the udev `uaccess` rule on the ITE controller's hidraw node is gone (a session process could
  send it any HID report); lighting goes through `lighting-helper` and pkexec (silent for a wheel/sudo admin at the
  machine). The installer removes the old rule.
- `health-reader` (kernel-log scan, ksnap, boot history…): the polkit default for `com.centurion.health.read` is now
  `auth_admin_keep` instead of `yes`; only a wheel/sudo admin at the machine gets it silently.
- Scene files are opened with the owning user's file-system identity (`setfsuid`), so a symlinked `~/.config` or
  `scenes` directory cannot make root read a file the user cannot.
- CI: every `uses:` in the workflow is pinned to a commit SHA (tag or branch noted in a comment).
- New `tune-admin-helper` (polkit `com.centurion.tune.admin`, the administrator password every time): boot preset,
  boot calibration, wake sources, wireless power lock and the unlock PIN. The preset store stays in `tune-helper`:
  no password, but refused unless the window is unlocked with the PIN, like every other new value.
- `sysadmin-helper` split: configuration files, module load/unload and NVIDIA driver options moved to
  `sysadmin-config-helper` (polkit `com.centurion.sysadmin.config`, the password every time); runtime actions stay.
  Added lines may no longer use modprobe `install`/`remove` (except `/bin/false`/`/bin/true`), udev
  `RUN`/`PROGRAM`/`IMPORT{program}`, or sysctl keys that run a command or lower a kernel protection
  (`kernel.core_pattern=|…`, `kernel.modprobe`, `kernel.yama.ptrace_scope`, `fs.protected_*`, …).
- `/etc/fstab` edits: the changed lines are compared with the disks present (UUID / PARTUUID / LABEL / PARTLABEL /
  device node, file system type, mount point, field count, explicit `suid`/`dev`), with "did you mean" suggestions;
  saving with findings needs a second confirmation.
- Intel undervolt: no offset below -200 mV on any path (pkexec, boot file, daemon, scenes, search), -50 mV on Core
  Ultra CPUs. The Intel tab's spin boxes follow the CPU's floor.

- Keyboard lighting administrator rights: the Lighting page drops them when the Centurion window closes (reading
  the keyboard again needs *Use administrator rights*), and the tray's Keyboard Lighting menu can enable them; those
  end one minute after the tray menu closes. Reload keeps using rights that were granted.

### Changed
- nvcurve: the curve readback now says whether the driver's offset-free base clocks (V3 tuples) are in use and, if
  not, why: the driver refused the request, reports no tuples, or the V1 cross-check rejected them (with the points
  that disagree). Shown as `Base clocks:` in `nvcurve read`, as `base_note` in its JSON and in the helper's result
  files, and in the V/F history entry of each apply/reset. Before, a missing base only showed up as a silent `null`
  (history: "Unknown"). The cross-check itself is unchanged.
- 61 rows that neither calibration nor Autotune touch and no built-in preset sets (network latency and buffering, the
  Wi-Fi/Bluetooth rows, elevator and discard fine tuning, NUMA/KSM/khugepaged internals, unmeasured scheduler feature
  bits) moved off the Optimizations tab to three System tools pages: Network tuning, Storage tuning and Kernel tuning.
  Same keys, presets, Restore originals and boot defaults; only the page and its own Apply checked button differ.
- Memory: the tray app uses about half the memory it did (anonymous memory 60 → 33 MB at idle on a test machine,
  RSS 107 → 80 MB). Under the application style sheet every widget is costly for as long as the program runs, so
  Health builds Diagnostics → This Boot, Kernel Parameters and Sleep & Wake when they are first opened, and the
  System tools catalog only while it is on screen; a combo box gets its popup (nine widgets) when it is first shown
  instead of when the window is built. Pages look and behave the same.

### Fixed
- Health → Backups → Restore of an increment: the whole chain is checked before the first image is unpacked — each
  image's decompressor (a chain may mix gzip/xz/zstd), that the key or passphrase opens every encrypted image, and
  that no image is empty. Before, a decompressor missing at step 3 or a foreign key at step 2 stopped the restore
  after steps 1–2 had already been written to the target (which can be the running system).

### Added
- What's new window after an update, starting with *Important*: the PIN, approving scenes and presets once, the
  lighting administrator rights and re-installing everything together. *Don't show again* hides it for good.
- Optimize: *Apply preset* in the experimental layout sits in the preset row (it was lost from the layout and floated
  over it); the greyed *Apply checked* explains why while locked.
- Health → Backups: every backup (system image and configuration backup) now also writes `<archive>.RESTORE.txt`
  beside it — a Turkish/English, step-by-step guide to restoring that exact archive by hand with Centurion not
  running (live USB): plain, encrypted (age key or passphrase), incremental (every file of the chain in order, without
  tar's own `--listed-incremental`), or both; the exact `tar`/`age`/decompressor commands and flags, mount points,
  fstab/bootloader steps. Retention and the automatic-backup pruning remove the guide with its archive.
- System tools → Kernel: *Info…* next to Load / Unload runs `modinfo` for the module names typed in the field
  (several allowed), or for every loaded module when it is empty, and adds each module's current parameter values from
  `/sys/module/<name>/parameters`. Read-only, runs as the user.
- Six knobs found missing in a full /sys dump: *No EC wake-ups in s2idle* (`pm.ec_no_wakeup`, s2idle battery
  drain), *Freeze file systems before suspend* and *Removable-disk polling interval* (Optimizations → Devices);
  *KSM pages_to_scan*, *KSM sleep_millisecs* and *DAMON proactive reclaim* (System tools → Kernel tuning). Manual
  only: neither calibration nor Autotune sets them.
- Product-report follow-ups (docs/REPORT_FEATURES_2026-11-25.md):
  - Scenes: every apply records its trigger (manual, tray, shortcut, D-Bus, power source, login, resume, game end,
    CO Search, …) and the final state of each component; the Scenes tab shows them under *Last apply*, so a refused
    GPU step no longer hides the fan and CPU steps that went through. Both are kept in `scene-transactions.jsonl`.
  - Home: a power-profile change is confirmed by reading the profile back.
  - Health → Cooling: *Compare two runs* (e.g. one per fan curve) under equal conditions — steady / peak temperature,
    fan speed, clock and the new *time at the temperature limit* figure that `centurion-cooltest` now records.
  - Firmware: BIOS CPU overclocking values are compared with the firmware's read-back on the first start after the
    reboot (requested = read back, limited by firmware, or unreadable).
  - Lighting (Spectrum): *4-zone preview* of the profile, with what a 4-zone keyboard cannot show.
  - Ryzen CO Search: the first wrong result of a trial is kept with its core, CO value, kernel, data seed, the idle
    wait before the failing wake burst and the test command; `centurion-cpustress` error lines carry
    `kernel_index`, `data_seed` and `idle_ms`.
  - Intel Undervolt: *Context transitions* — the boot service journals every full apply (boot, AC ↔ battery,
    resume, scene) with each field's read-back and marks values refused by a firmware lock.
  - NVIDIA Curve: *Record used region* marks the curve points the GPU actually ran at during a session.
  - AMD GPU: a trial revert reads the driver's values back and lists matched / different / unreadable fields.
  - Autotune report: per-candidate latency / throughput / power trade-off and, with changed weights, which
    candidate would win and which weight decided it.
  - ABBA history: a coverage block (blocks done of planned, exclusions, measured and unmeasurable objectives,
    decision interval).
  - Frametime comparison: says so when the average improves but P99 gets worse, or the reverse.
  - CPU test history: tested CPUs, load, temperature range and duration per result; short scans are labelled.
  - Evidence Lab: an *incident focus* filters all five reports by one CPU / PCI address / Xid and is kept across
    tabs, rescans and restarts.
- Scheduling-domain rows: `imbalance_pct`, `min_interval`, `max_interval` and `busy_factor` of the MC (one cache
  domain) and PKG (balancing between CCDs) levels, written to every CPU's domain in debugfs `sched/domains/`. The
  level is found by the domain's `name`, so the rows follow the topology; measured by the Scheduler calibration.
- The on/off reader and writer now understand `true`/`false` files, and the swap VMA readahead row is a plain switch.
- 11 network latency and buffering rows: softirq packet and time budgets, NAPI weight, GRO batch, TCP output
  limit, autocorking, pacing ratios, ECN, and the socket buffer ceilings `rmem_max` and `wmem_max`. Manual only: the
  Network group has no calibration phase.
- Home → hardware inventory: four rows from plain sysfs/procfs reads, no helper and no password: Scheduler
  (EEVDF, BORE on or off, preemption model, sched_ext when present), CPU layout (cores and threads, SMT, L3 size per
  cache domain), CPU power (cpufreq driver, governor, EPP, boost) and Memory policy (THP mode, swappiness, zram).
- 20 more knobs found by a full `/sys` dump: cache-aware scheduling `epoch_period` and `epoch_affinity_timeout`,
  the rseq slice extension, `sched_rt_runtime_us`, `sched_rr_timeslice_ms`, the five NUMA-balancing scan knobs,
  `fault_around_bytes`, `tlb_single_page_flush_ceiling`, `vm.laptop_mode`, `vm.oom_kill_allocating_task`, KSM
  `use_zero_pages` and `smart_scan`, the tiering promote rate limit and the mac80211 AQL and mt76 threaded-NAPI
  switches. Seven are measured by the calibration; the rest are manual only and listed in `calib::EXCLUDED` with the
  reason (or, for the Wi-Fi rows, have no calibration phase).
- Latency and power rows from the same dump: mt76 Wi-Fi runtime power management, its idle timeout and deep sleep,
  Bluetooth LE minimum and maximum connection interval, and the swap VMA readahead switch (measured by the calibration).
  The cache-aware scheduling rows now also look in `sched/llc_balancing/`, where kernel 7.3 keeps them.
- Health → Advanced → *Kernel parameters*: the live value of every readable setting under a chosen part of `/sys`
  (scheduler, x86, CPU, memory, module parameters, or everything), debugfs included, with a filter. debugfs is
  root-only, so the page reads it through the new `health-reader {"op":"ksnap"}` (`ksnap.rs`): the same silent,
  read-only polkit action as the rest of `health-reader`, no new helper and no password. It never follows
  symlinks, skips write-only and binary files, opens debugfs only in `sched`, `x86`, `pm_qos` and `extfrag` (a
  root-only read of other debugfs files can reset a GPU or touch hardware registers), never reads serial numbers,
  cgroup, security or pstore trees, scrubs kernel addresses, and is bounded in entries, bytes, depth and time.
- Optimizations refreshes the root-only copy of the debugfs values every time the tab is shown
  (`health-reader {"op":"debugfs_snapshot"}`), so scheduler rows show live values and not those of the last apply.
- 13 more EEVDF scheduler feature bits in Optimizations → Scheduler: `PICK_BUDDY`, `DELAY_ZERO`, `WA_IDLE`,
  `WA_WEIGHT`, `WA_BIAS`, `UTIL_EST`, `NI_RANDOM` (measured by the calibration, chosen by autotune like the other
  bits) and `ATTACH_AGE_LOAD`, `NONTASK_CAPACITY`, `LB_MIN`, `NI_RATE`, `HRTICK_DL`, `PARANOID_AVG` (manual only,
  listed in `calib::EXCLUDED` with the reason). A test keeps every feature bit either measured or excluded.
- PipeWire Doctor → *Settings…*: tick-to-set table of well-known options with a hint for each (sample rate and
  allowed rates, buffer sizes, log level, Pulse request sizes, Bluetooth headset profile, new-output volume,
  auto-mute, idle-suspend delay and ALSA headroom). Written to `50-centurion.conf` files in `~/.config/pipewire`
  and `~/.config/wireplumber` (no root; they override the system files without changing them), then the sound
  servers are restarted; *Remove all changes* deletes the files.
- EDID / VRR shows more per display: physical size, bit depth, colour formats, HDR modes with peak / black level,
  colour spaces, audio, manufacture date, mode count, connector state, and the backlight devices. *Panel
  controls…* sets the built-in screen's brightness and refresh rate (as the user, through `centurion-display`).
- Health → Network → Wi-Fi shows more about the connection and the adapter. *Connection* adds access point,
  time connected, channel / band / width, Wi-Fi generation, the link-rate details, estimated usable speed,
  transmit power, data transferred and lost beacons. *Hardware* (new) gives adapter name, driver, bus, firmware,
  hardware ID, radio and rfkill state, power save, regulatory country, the bands with their channels, power and
  Wi-Fi generations, and the modes the adapter supports. *Nearby networks* (new) lists the networks the system
  has already scanned with signal, channel, security and age, tells how many networks share your channel, and
  has a *Rescan* button (NetworkManager). An adapter picker appears on machines with more than one adapter, and
  *Copy details* puts the page on the clipboard. All reads stay unprivileged; no new root helper or password.
- Health → System tools can change the system, not only read it, through a new root helper `sysadmin-helper`
  (polkit `com.centurion.sysadmin`: the administrator password, then polkit's keep window; the existing
  password-free helpers are unchanged). Tool pages: CPU boost / clock cap, drop caches, zswap, fstrim, SMART
  short / long self-test, USB autosuspend per device, PCIe ASPM policy, load / unload a module, fwupd install.
  Report findings with a concrete fix get a button (radio unblock, CPU cap / boost, zswap, huge-page defrag,
  HDA power saving, PipeWire restart, `options nvidia_drm modeset=1`). Sleep: suspend mode (s2idle / deep) for
  this boot. dGPU power: *Enable runtime PM* when power/control was left on.
- *Configuration files* (System tools): sysctl, modprobe, modules-load and udev drop-ins of Centurion's own,
  `/etc/sysctl.conf` and `/etc/fstab`, edited as a table (sysctl, modprobe, modules-load) or as text. Saving
  shows the difference, validates the lines (fstab with `findmnt --verify`), keeps the last 10 versions in
  `/var/lib/centurion/config-backups/` and writes atomically; any version can be restored.
- Health diagnostics rebuilt as interpreted reports (`docs/HEALTH_DIAGNOSTICS.md`): Evidence Lab (H02–H09),
  Investigations (H01–H18) and the System tools S01–S18 now give a verdict, findings with what to do, tables and
  their limits, with *Copy report* / *Save report…*; translated. Investigations opens on *Guided triage*, which
  runs the checks of a symptom (GPU, CPU / CO, crashes after an update, storage, Wi-Fi / Bluetooth, sleep, USB).
- H01 compares boots by kernel, NVIDIA driver, BIOS and microcode recorded per boot by the boot guard
  (`exposure.json` → `versions`), with a binomial test against luck.
- H12 reads the kernel's `suspend_stats` and recognises boots that went to sleep and never resumed (sleep hook
  marker `/var/lib/centurion/sleep-inflight`, `/var/log/centurion/sleep-failed.log`).
- H06 decodes `critical_warning` bits and error-log status codes (a rejected admin command is not a drive fault).
- S03 decodes display identity, range limits and FreeSync / HDMI VRR ranges; S13 lists the charger's USB-PD
  profiles; S18 flags zstd / xz firmware the kernel cannot decompress and devices without a driver; S10 flags
  zram + zswap together and memory pressure; S04 separates link width loss from idle down-clocking.

### Changed
- Intel *Voltage & safety* (Studio UI): *Voltage offsets* / *Extras* and *Limits* / *Limit reasons* sit in
  equal-width, equal-height pairs; *Startup & daemon* merged into the voltage page; pages scroll when a CPU
  offers more controls; an empty voltage list explains why (AMD CPU or hidden OC mailbox). Classic UI unchanged.
- Intel tab tooltips describe each option by itself instead of naming other undervolt tools.
- Network protection → Lookup → Port check accepts up to 1024 ports (was 64); at most 256 connections are tried at once.

### Fixed
- CO Search: opening the window with no journal file asked "Invalid CO search journal" every time (a missing file
  was treated as an unreadable one); the question now appears only when a journal exists.
- CO Search: the "Invalid CO search journal" question came back on every open when the journal could not be
  removed (e.g. owned by root) or the reset before removing it failed, both silently. It now names the file, its
  owner and the error, says when removal fails (with the command that removes it), and an unreadable journal can
  be removed after a failed reset.
- TCC offset above 15 °C is refused on Haswell/Broadwell (4-bit field there); MCHBAR mask table gains 0x9B61
  (Comet Lake-U) as in `throttled`.
- PCIe AER totals were never read: the kernel prints `TOTAL_ERR_COR 14`, the parser required a colon. Evidence
  Lab and Events & alerts showed every AER device as Unknown.
- NVMe Deep Health read smartctl field names from nvme-cli JSON (spare and life used missing) and showed the
  temperature in kelvin.
- Sleep / Resume matrix counted log lines that are never saved (always 0); NVIDIA parameters reported `0x02`
  and `2` as different; the incident list offered every kernel warning; CO Cross-Link printed no attempt count.

### Added
- Ryzen Curve → CO Search rewritten: all-core or per-core search, fast / medium / long levels, 60 s CPU cycles
  (`centurion-cpustress --load co`: 40 s of steady FFT / NTT load in the manner of Prime95 Small FFT and y-cruncher,
  then 20 s of load changes that move the clock and the idle states), single-core trials on the best-ranked cores,
  a confirmation pass, its own scene (no temperature cutoff of its own: the CPU firmware's thermal limits apply),
  AC power only, resume after a crash, and the result
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
- Ryzen Curve → CO Search: discarding an interrupted search in the same boot (the application had died, the
  machine had not) resets the CPU curve to stock first; the journal is removed only after a confirmed reset. The
  trial's offset could stay applied until the next reboot, while the dialog said the curve was not changed.
- Ryzen Curve → CO Search: only a machine restart records a crash guard. Quitting the application during a trial
  (after its confirmed stock reset) runs that trial again on resume; the application dying while the machine kept
  running counts as a failure in that search only. Both used to forbid the value on that core for good.
- Ryzen Curve → CO Search runs on AC power only: it does not start on battery, and unplugging the charger stops it
  with a stock reset. A battery running out used to end a trial like a crash (a false crash guard), and the lower
  battery power limits gave results that need not hold on AC.
- Ryzen Curve → CO Search, per-core: the first failure after a pass now ends that core at its last passing value;
  nothing more negative is tried on it again (it used to keep refining between the pass and the failure). The
  descent step is one setting (default 1) instead of coarse / refine steps. A crash guard forbids the crashed value
  and everything below it on that physical core in every later search; the two-reboot lockout is gone, since a
  guarded value can never be replayed. Cores can be unticked to stay at stock. A journal that cannot be resumed
  can now be discarded instead of blocking every later start. See `docs/CO_SEARCH_PERCORE_RULES_2026-11-20.md`.
- Ryzen Curve → CO Search, per-core: every core has its own Start, typed in the table or filled from a saved
  Ryzen Curve profile (*Preset*), so saved profiles can be tested and pushed further with the chosen step; a failed
  Start climbs back towards stock by the step. The global start moved to all-core (*All-core start*, default -5).
  The per-core confirmation is replaced by a 20-minute verification of the whole set that alternates single-core
  FFT-SSE / NTT / FFT-AVX cycles with all-core loads, twice up to the package power limit.
- Ryzen Curve → CO Search: a crash watcher. During every trial the MCA error counters and kernel events are
  committed to the recovery journal as they appear; after a hard reboot they, the machine checks the kernel finds
  at the next boot and pstore records are judged. Only a core's own units can blame it (shared L3 / memory /
  fabric, several cores, a core at stock or a lockup blame nobody), the blame only narrows the final
  verification's back-off and is believed once per search. *Save result as profile…* (named, also for a stopped
  or discarded search, marked not verified) and *Use result as Start*.
- `centurion-cpustress --load power`: dense AVX2 / AVX-512 FMA work on independent vectors (twice the FLOP rate of
  `avx512`), the load that reaches the package power limit; the CO verification's earlier all-core parts stayed
  far below it. One-ulp errors are kept by stirred integer accumulators (the map alone can round them away).
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
