# Centurion

**Legion Space / Vantage for Linux — and then some.**
One tray app for Lenovo Legion laptops: power profiles, firmware power limits,
fan curves, CPU and GPU undervolting, per-key RGB, game-aware system tuning with
a measured **Autotune**, **Scenes** that switch all of it at once, and a
**Health** suite (kernel-error history, dGPU power diagnostics, network guard,
backups).

Native Qt6 GUI · privileged work in small validating Rust helpers · no
telemetry · OpenRC and systemd. Nothing runs in the background unless you
enable one of the optional boot services.

> **Not an official Lenovo product.** Centurion is an independent, community-made
> project. It is not affiliated with, endorsed by or supported by Lenovo.
> Lenovo, Legion, LOQ, IdeaPad, Legion Space and Vantage are trademarks of
> Lenovo; they are used here only to name the hardware it runs on.

> ⚠️ This tool writes low-level hardware and firmware settings.
> Read [DISCLAIMER.md](DISCLAIMER.md) before using it.

**Using it?** Start with the [Handbook](docs/HANDBOOK.md) (task-by-task).
Reference: [TECHNICAL.md](docs/TECHNICAL.md) · [AUTOTUNE.md](docs/AUTOTUNE.md).

## What is inside

Tabs appear only when the hardware, driver or tool behind them exists.

| Tab | What you get |
|---|---|
| **Home** | Power profile cards (Quiet / Balanced / Performance / Custom). *Hardware* box (system, kernel, CPU, GPU, RAM, BIOS, EC firmware) and *Live* box (CPU per CCD, dGPU state, iGPU, fans, NVMe, power rails, battery). *Device* box: battery charge mode, GPU mode (MUX), iGPU-only mode, Panel Overdrive, Fn lock, camera, USB charging when off (and on battery), white keyboard backlight level, Instant Boot, Custom in Fn+Q, Flip to start, Super-key lock, touchpad lock — each only where the firmware reports it. **Device info…** shows the model series, firmware interfaces and capability list. Fans: per-fan Auto / Max / set, **Turbo fan** (EC full speed), custom **Fan curve** editor. **Memory timings** viewer and BIOS editor. Boot-guard status with *Resume boot presets* / *Resume login scene*. |
| **Scenes** | One name for the whole machine: power profile, firmware limits, CPU curve (Ryzen / Intel), NVIDIA curve, Optimizations preset, keyboard lighting, fan boost, Custom-mode fan curve, a run command. AC / battery auto-switching, pause switch, game-start scene, export / import. |
| **Firmware** | CPU PL1 / PL2 / PL3 and temperature target, GPU cTGP, Dynamic Boost ceiling / floor (written over WMI where the kernel refuses), BIOS CPU overclocking (applied next boot). |
| **NVIDIA Curve** | Drag-and-edit V/F curve with zoom and pan, core / memory offsets, VRAM clock lock, GPU core clock cap, named profiles, ★ default applied at boot, driver module options, full reset. Status: temps (incl. VRAM / hotspot), throttle reasons, PowerMizer. |
| **Ryzen Curve** *(AMD)* | Per-core and all-core Curve Optimizer (CPPC-ranked, CCD-aware layout), profiles; *Reset* clears both. |
| **Intel Undervolt** *(Intel)* | Voltage offsets, IccMax, TCC offset, PL1 / PL2, AC / battery profiles, ThrottleStop.ini import, limit-reason counter, live monitor, boot / resume apply. |
| **AMD GPU** *(amdgpu)* | Overdrive clocks / voltage per GPU generation, power limit, fan curve (RDNA3+), power profile. Every Apply is a **trial** — reverts in 20 s unless you press *Keep*; one-click undervolt steps. |
| **Optimizations** | 100+ documented CPU / memory / scheduler / storage / network / device / power knobs in 8 groups, built-in presets, **Autotune**, game launch hooks (Lutris, Steam), boot-parameter advisor, *Restore originals* and *↺ Return to boot-guard defaults*. |
| **Lighting** *(Spectrum, or 4-zone RGB)* | Spectrum keyboards: per-key RGB on a drawing of your own keyboard, firmware effects, 6 hardware profiles, brightness, lid logo, accent lights. 4-zone RGB keyboards: effect, speed, brightness and four zone colours. |
| **Health** | *Monitor* (Xid / GSP / MCE / AER / lockup events, kept across boots) · *dGPU power* (D3cold / runtime-PM report) · *Network* (connections, IP blacklist, Wine/.exe guard, connection log, whois) · system tool pages (Sensors, CPU, Memory, DMI, PCI, PCIe detail, USB, Storage, SMART, NVIDIA, Graphics, Kernel, Battery) · *Kernel log* (history across boots) · *Backup* (Centurion configuration + full system image, with restore). |

The **tray menu** carries the everyday actions: Scene (with auto-switch and
pause), Power Profile, CPU Curve, CPU Undervolt (Intel), GPU Curve,
Optimizations, Keyboard Lighting, Fans, Show / Hide, Quit. Every setting has a
tooltip that explains what it does and which value to use.

**dGPU-friendly by design.** Home never calls `nvidia-smi`: the GPU row reads
the sysfs runtime-PM state plus the EC temperature, so an open window does not
wake a sleeping RTX. Live clock / power / utilisation exist only in the NVIDIA
tab (which does wake it).

## Command-line tools

| Command | Purpose |
|---|---|
| `centurion [--window] [--version]` | GUI + tray (`--window` opens the window; a second launch just shows the first) |
| `nvcurve read\|write\|profile\|memlock\|sensors\|powermizer\|reset-all` | NVIDIA V/F curve CLI (generic; not Legion-gated) |
| `centurion-gamemode PRE\|POST\|RUN\|WRAP\|APPLY\|UNDERVOLT\|SCENE\|RESTORE\|STATUS\|APPROVE` | Game hook for Lutris / Steam, scripted scene switching |
| `centurion-autotune <powersave\|gaming\|throughput\|desktop>`, `probe`, `audit [--fix]`, `report [SEC]` | Autotune presets, disk probe, safety audit, memory report |
| `centurion-calibrate [--budget MIN] [--all] [--depth lean\|deep\|max] [--scene NAME] [--list] [--show] [--restore]`, `--verify NAME`, `--field LOG...`, `--gpu-coupling`, `--schedule-boot` | Measure this machine's knobs (the "machine signature"), check presets, learn from real games |
| `centurion-boot-guard status\|reset\|…` | Boot-crash protection state |
| `centurion-netguard daemon\|status\|list\|whois IP` | Network guard daemon and queries |
| `centurion-intel-uv read\|apply\|reset\|monitor\|measure\|throttlestop FILE\|turbo\|daemon` | Standalone Intel undervolt (root) |

## Boot services

Enable only what you use. Nothing does anything until you set the matching
default / preset in the GUI.

| Service | Role | OpenRC runlevel |
|---|---|---|
| `centurion-boot-guard` | crash protection for the rest, records clean shutdowns (recommended) | default |
| `nvcurve-autoload` | ★ default GPU curve | default |
| `centurion-tune` | ⏻ Optimizations boot preset | boot |
| `centurion-netguard` | network guard / connection log (Health → Network; needs nftables) | default |
| `centurion-intel-uv` / `centurion-intel-uv-daemon` | Intel undervolt at boot / resume, or with AC switching (Intel only) | boot / default |

```sh
rc-update add centurion-boot-guard default       # OpenRC
systemctl enable centurion-boot-guard            # systemd
```

## Safety model

- **Validated helpers.** The GUI never touches hardware. Each change goes
  through a small Rust helper in `/usr/libexec/centurion/` that
  reads one JSON request, checks it against fixed paths and live kernel
  ranges, and exits.
- **Boot guard.** A boot that ends within 3 minutes without a clean shutdown
  pauses every boot preset (and the login scene) until you press *Resume*.
- **Firmware-persistent changes always ask** for the administrator password
  (BIOS memory timings, BIOS CPU OC, GPU MUX, iGPU-only override). Backups and
  restores ask every time too.
- **Reversible.** *Restore originals* (tuning), *Reset* (curves), *Factory
  reset profile* (lighting), *Return to boot-guard defaults* (all tuning knobs
  to the values captured at boot), AMD GPU trial apply.
- **Write log.** Hardware writes (acpi_call, sysfs, BIOS variable) are logged
  with their caller to `/var/log/centurion/writes.log`.
- **Machine gate.** Runs only on Lenovo Legion / LOQ / IdeaPad Gaming (see
  below).

### Security levels

Chosen at install time (`--security-level=N` or asked interactively):

| Level | Behaviour |
|---|---|
| 1 (default) | local, active `wheel` user runs helpers without a password |
| 2 | every helper asks for the admin password (polkit caches it briefly) |
| 3 | as 2, except applying an already-approved Optimizations preset by name |

## Supported hardware

**Lenovo Legion, LOQ and IdeaPad Gaming laptops only.** The program checks the
machine at start (DMI vendor and model name) and refuses to run anywhere else —
the GUI shows why and exits, and every root helper refuses before touching
hardware, because the firmware and EC settings it writes are Lenovo-specific.

- **Developed and tested on:** Lenovo Legion Pro 7 16AFR10H (Ryzen 9 9955HX3D, RTX 5080).
- **Should work on:** other Legion / LOQ models whose kernel exposes
  `lenovo-wmi-gamezone`, `lenovo-wmi-other` and `ideapad_laptop` — tabs and
  rows only appear when the hardware and driver behind them exist.
- **Model-specific (16AFR10H BIOS):** custom fan curve, BIOS memory timings,
  GPU power limits via WMI, GPU MUX switch.
- **Lighting tab:** keyboards with the Spectrum per-key controller (USB
  `048d:c1xx` Gen10, `c9xx` Gen7–9 / Legion 9, `c6xx` LOQ Gen10 / Legion 5
  Gen11) and the older 4-zone RGB keyboards (`048d:c9xx`). White-backlight
  keyboards get a level selector on Home. Only the Gen10 `c1xx` keyboard has
  been tested on real hardware.
- **Other models, by firmware capability:** optional features (Flip to start,
  key / touchpad lock, EC fan readings, Over Drive and Turbo-fan fallbacks) are
  offered only when the machine's own capability list or `IsSupport…` method
  reports them; *Home → Device info…* prints that list for bug reports.
- Intel Legion support has been checked against a Core Ultra 7 255HX.

Reports from other models are very welcome — open an issue with your model
number (e.g. `83F5`) and what worked.

## Install

### From source

```sh
git clone <this repository> centurion
cd centurion
sudo ./install.sh
```

`install.sh` builds everything tuned for this machine (native CPU, LTO, PGO,
hardening) and installs to `/usr`. Options: `--no-native` for portable
binaries, `--no-lto`, `--no-pgo`, `--no-harden`, `--no-build`,
`--remove-legacy` to remove the old Python version, `--clang`,
`--security-level=N`. Remove with `sudo ./uninstall.sh`.
To build with clang/LLVM: `sudo ./install-clang.sh` (same options; uses lld
when installed and `llvm-profdata` for PGO).

### Upgrading from Legion Power Manager (LPM)

Centurion is the renamed Legion Power Manager. Install it **over the existing
LPM install** — no uninstall first: `install.sh` (and `install-clang.sh`)
detects the old install and migrates it automatically:

- settings, Scenes, Optimizations presets, calibration data, logs and backups
  (`/etc`, `/var/lib`, `/var/log`, `~/.config`, `~/.local/state`, …) move to the
  `centurion` paths;
- `lpm-*` boot services are stopped and the matching `centurion-*` services are
  enabled in the same runlevels / systemd targets;
- Lutris and Steam launch hooks are rewritten from `lpm-gamemode` to
  `centurion-gamemode` (a `.pre-centurion` copy is kept; close Steam first);
- old binaries, helpers, polkit/udev rules, menu and autostart entries are removed.

Afterwards log out and back in once.

### Gentoo

```sh
./make-dist.sh    # → dist/centurion-2.0.0.tar.xz (crates vendored, builds offline)
```

Copy the tarball into your `DISTDIR` and emerge the ebuild from
`packaging/gentoo/sys-power/centurion/` in a local overlay.

### After installing

1. Log out and back in once (keyboard-lighting udev rule and polkit rules take effect).
2. Start **Centurion** from the menu — it lives in the tray and
   starts at login. Missing tray → the window opens instead.
3. Enable the [boot services](#boot-services) you want.

## Quick start

- **Power profile:** Home → pick a card, or tray → *Power Profile*.
- **Undervolt the GPU:** NVIDIA → *Read Curve* → set an offset or drag points →
  *Apply Offsets* → *Save As…* → ★ *Default* to apply it at boot.
- **Undervolt the CPU:** Ryzen → all-core or per-core offset → *Apply* → save a profile.
- **Tune for games:** Optimizations → ⚙ *Autotune* (or pick a preset) → *Apply
  checked*; ★ *Use for games* and paste the hook from *Game launch* into
  Lutris / Steam.
- **One-click setups:** Scenes → *New…* → *Capture current* → adjust → *Save*;
  tick *Switch scenes with the power source* for AC / battery.
- **Keyboard colours:** Lighting → select keys → *Paint selection* → *Apply to profile*.
- **Back everything up:** Health → Backup → *Centurion configuration*.

## Optimizations, Autotune, calibration

**Optimizations** has one sub-tab per group — CPU, Memory, Scheduler, Storage,
Network, Devices, Power, Stability — plus *Game launch* (Lutris / Steam hooks,
game-start scene) and *Boot options* (kernel-parameter advisor; nothing edits
your bootloader). Rows the machine lacks are greyed out. On 2+ CCD chips,
governor / EPP / boost / max-frequency are per CCD (the V-Cache die is
labelled).

**⚙ Autotune** profiles the machine and fills the tab with a preset for a goal
(power saving · gaming · throughput · desktop). Nothing is written until you
press *Apply checked*.

- **Anchored at boot defaults.** The values at boot (kernel + distro + your
  sysctl, before TLP) are the reference; settings move at most 2× from them
  unless evidence justifies more; settings the pressure guard had to roll back
  are penalised, then retired.
- **Weighted, not "bigger is better".** Each trade-off is scored over latency,
  throughput, power, memory footprint and stability; a value is written only if
  it clearly beats leaving the setting alone. **Weights…** edits the weights per goal.
- **Measured and bounded.** Dirty limits come from the disk's measured write
  rate; watermark / THP values stay inside RAM-derived limits; boot parameters
  win; `kernel.watchdog` is never disabled.
- **Safety net.** The helper refuses values that fail the audit, and a 120 s
  pressure guard restores memory / writeback settings if I/O or memory stalls.
- **Calibration** (`sudo centurion-calibrate`, ~12–15 min by default) measures what
  each knob actually does on this machine — idle, under load and on the disk —
  and accumulates a *machine signature* that Autotune uses to dose each knob.
  Stop Scenes and use *↺ Return to boot-guard defaults* first, so the
  baseline is clean.
  Each session records its conditions (power source, profile and firmware limits,
  die temperature, fan, display) and the model keeps them apart; *Calibrate at next
  boot…* runs sessions unattended once the machine is idle on AC.
- **Checked against reality.** `centurion-calibrate --verify` measures a whole preset
  against the boot state; with *A/B field comparison* centurion-gamemode alternates real
  game sessions and `centurion-calibrate --field` learns from their frame-time logs (FLM,
  MangoHud); crashes, Xid and D3cold failures while a knob was changed add to its
  stability cost.

```sh
centurion-autotune desktop                 # show a preset (powersave|gaming|throughput|desktop)
centurion-autotune gaming --save           # save it as "Auto Gaming"
centurion-autotune audit [--fix]           # find / repair unsafe values in scenes and presets
sudo centurion-calibrate --budget 15       # measure the machine
```

After updating from an older version run `centurion-autotune audit --fix`, then
re-save your scenes. Details: [docs/AUTOTUNE.md](docs/AUTOTUNE.md).

## Health suite

- **Monitor** — scans the kernel log for NVIDIA Xid / GSP, MCE, AER, soft
  lockups and guard rollbacks; events are saved across boots (open / clear the saved log).
- **dGPU power** — runtime-PM state, sibling PCI functions, upstream port,
  driver power report and NVreg parameters, open handles that keep the GPU
  awake, *Copy report*.
- **Network** — *Connections* (live sockets with their process), *Blocked*,
  *Log*, *Rules*. Optional guard cuts off any non-whitelisted Wine / .exe
  program at its first packet; IP / CIDR / range blacklist (bulk bans are
  removable as one named group); right-click menus (details, kill connection,
  whitelist, IP info, blacklist IP / subnet / owner's network); connection
  log with reverse-DNS and whois. Needs the `centurion-netguard` service and nftables.
- **Backup** — *Centurion configuration* (scenes, presets, curves, lighting, boot
  profiles, network rules, calibration; optional system profile of Portage
  config, kernel config, fstab, package list) and *System image*
  (`tar --acls --xattrs` of `/` through pigz / zstd / xz, with verify,
  retention and restore to any target).

## Troubleshooting

- **Freeze or instability after a preset:** *Restore originals*, then
  `centurion-autotune audit`. Boot presets pause automatically after a crash.
- **Tab or row missing:** the hardware, driver or tool behind it is absent — see Requirements.
- **dGPU never sleeps:** Health → dGPU power → look at *open handles* and runtime PM.
- **Games fail after parking a CCD, or Xid errors:** Health → Monitor; check
  *Optimizations → CPU* CCD settings and the NVIDIA driver options.
- **BIOS setting reverts at boot:** check `/var/log/centurion/writes.log`
  for what Centurion wrote and when.
- **Lighting asks for admin rights:** re-login or replug the keyboard once so
  the udev `uaccess` rule applies.

## Requirements

**Build:** Rust ≥ 1.75 (cargo), a C++20 compiler, CMake ≥ 3.19, Qt ≥ 6.4 (Widgets, Network, Svg).

**Runtime:**

| Needed for | Dependency |
|---|---|
| everything | polkit (`pkexec`), Linux with `platform_profile` |
| firmware limits, fans, battery, device toggles | kernel drivers `lenovo-wmi-gamezone`, `lenovo-wmi-other`, `ideapad_laptop` |
| GPU power limits, fan curve, GPU mode, instant boot, EC temps | `acpi_call` kernel module |
| NVIDIA tab | proprietary NVIDIA driver (NvAPI / NVML are loaded at runtime) |
| AMD GPU tab | `amdgpu` with `ppfeaturemask` bit `0x4000` for Overdrive |
| Ryzen tab | root-owned `ryzenadj` in `/usr/bin`, `/usr/sbin`, `/usr/local/{bin,sbin}` or `/opt/ryzenadj` |
| live memory timings, extra CPU sensors *(optional)* | `ryzen_smu`, `zenpower` / `zenergy` |
| BIOS memory timings | efivarfs (`/sys/firmware/efi/efivars`) |
| Lighting tab without a password | udev + systemd-logind or elogind (`uaccess`) |
| Health → Backup (system image) | GNU `tar` with ACL / xattr support; `pigz` (falls back to `gzip`), optionally `zstd`, `xz` |
| Health → Network | `nft` and the `centurion-netguard` service; kernel: `NETFILTER_NETLINK_QUEUE`, `NF_TABLES`, `NF_TABLES_INET`, `NFT_QUEUE`, `NF_CONNTRACK`, `NFT_CT`, `INET_DIAG`, `INET_TCP_DIAG`, `INET_UDP_DIAG`, `INET_DIAG_DESTROY` |

Missing pieces only disable the tab or row that needs them.

## How it works

```
gui/                    Qt6 GUI + tray (C++20)
crates/centurion-helpers/     root helpers, centurion-gamemode / autotune / calibrate / boot-guard / netguard
crates/centurion-spectrum/    Spectrum keyboard protocol (hidraw)
crates/nvcurve/         NVIDIA V/F curve core, CLI and root helper (NvAPI + NVML via dlopen)
packaging/              OpenRC / systemd units, polkit, udev, Gentoo ebuild
docs/                   HANDBOOK, TECHNICAL, AUTOTUNE
```

The GUI runs as your user. polkit lets a `wheel` user at the machine run the
everyday helpers without a password; firmware-persistent changes always ask.
The lighting helper normally runs as you through a udev `uaccess` rule.

Data lives in `~/.config/centurion/` (scenes, presets, AMD GPU
profiles), `~/.config/ryzen-curve-optimizer/` (CPU profiles), `/etc/nvcurve/`
(GPU profiles), `/etc/centurion/` (boot preset, approved presets,
network rules), `/var/lib/centurion/` (boot guard, calibration) and
`/var/log/centurion/` (write log, connection log).

## Credits and inspiration

Model tables, capability IDs and the 4-zone keyboard report come from reading
**LenovoLegionToolkit**; WMI method numbers were cross-checked against the
kernel's `lenovo-wmi-*` documentation and **LenovoLegionLinux** (see NOTICE).

- **[Lenovo Legion Toolkit](https://github.com/LenovoLegionToolkit-Team/LenovoLegionToolkit)**
  (GPL-3.0) — the reference for Legion hardware on Windows. The Spectrum
  keyboard protocol, the keyboard ID table and the lighting effect rules were
  learned from its source; the code here is an independent Rust / C++
  implementation. Thank you to Bartosz Cichecki, the LenovoLegionToolkit-Team
  and every LLT contributor.
- **[legion-spectrum-control](https://github.com/alstergee/legion-spectrum-control)**
  (MIT) — key legends for the Lighting tab; confirmed the Gen10 keyboard layout.
- **Lenovo Legion Space / Vantage** — the feature set this app brings to Linux.
- **The Linux kernel `lenovo-wmi-*` and `ideapad_laptop` drivers**, **RyzenAdj**,
  **ThrottleStop**, **intel-undervolt** and **throttled** — for the interfaces
  and ideas the CPU tabs build on.
- **[LenovoLegionLinux](https://github.com/johnfanv2/LenovoLegionLinux)** — for
  paving the way for Legion support on Linux.
- **LACT** and **TLP** — reference for the AMD GPU tab and the device power settings; both implemented independently.

Third-party license texts: [NOTICE](NOTICE).

## License

Copyright © 2026 arabcian.
Free software under the **GNU General Public License v3.0 or later** — see
[LICENSE](LICENSE). No warranty; see [DISCLAIMER.md](DISCLAIMER.md).
