# Centurion — technical notes

Implementation details, design decisions and the full tuning reference. For installation and everyday use see the [README](../README.md).

## Tab reference

- **Power profile (Home tab).** Pick a profile card; it's applied immediately
  through `legion-profile-helper`, no reboot needed.
- **GPU curve (NVIDIA tab).** *Read Current Curve* first, drag points or set
  Core/Memory offsets, *Apply Offsets*. *Save As…* keeps a named profile;
  ★ *Default* auto-applies one at every boot (needs
  `systemctl enable --now nvcurve-autoload` / `rc-update add nvcurve-autoload default`,
  see *Boot-time services*).
- **CPU curve (Ryzen tab).** Per-core Curve Optimizer offsets, or one offset
  for every core with *Apply All-Core*. *Disable* a slot if your CPU has fewer
  physical cores than SMU slots (common on cut-down/partially-populated CCDs).
- **CPU undervolt (Intel Undervolt tab, Intel CPUs only).** *Read current*,
  tick the values to write, *Apply*. Voltage offsets for core / cache / iGPU /
  system agent / analog I/O (OC mailbox MSR 0x150), IccMax, TCC offset and
  PL1/PL2 (MSR 0x610, mirrored to MCHBAR). A readback mismatch means the BIOS
  locks undervolting (Plundervolt, CVE-2019-11157). *⏻ Apply at boot & resume*
  stores the values for the `centurion-intel-uv` service; offsets are lost on S3,
  so the unit (systemd) / elogind hook (OpenRC) re-applies them after resume.
  Separate AC / battery profiles, BD PROCHOT, cTDP, PL lock, positive offsets
  (opt-in), ThrottleStop.ini import, live throttle/VCore/RAPL monitor.
  Optional daemon (`centurion-intel-uv-daemon` service): switches profile with the
  charger, re-applies after resume and every interval (the EC restores PL1/PL2),
  and runs hwphint EPP switching by load or RAPL power.
  Without the GUI: `sudo centurion-intel-uv read | apply … | reset | monitor | measure |
  throttlestop FILE | turbo on|off | daemon`.
- **GPU power limits without a firmware range (Firmware Attributes tab).**
  `gpu_nv_ctgp`, `gpu_nv_ppab` and `gpu_nv_cpu_boost` are listed by
  lenovo-wmi-other with min = max = 0, so the kernel rejects sysfs writes. The
  tab writes these three through the Lenovo WMI method (\_SB.GZFD.WMAE) with
  acpi_call instead and reads them back from it (boost limits 0–25 W, cTGP
  unclamped for testing). Needs the acpi_call module.
- **System tuning (Optimizations tab).** Pick a built-in preset (top dropdown),
  *Load*, review the checked rows, *Apply checked*. ★ *Use for games* wires it
  into Lutris/Steam (see the Game launch sub-tab for the exact hooks); ⏻ *Apply
  at boot* stores it as the boot preset (needs enabling the `centurion-tune` service,
  see *Boot-time services*). *Restore originals* always available at the top of the tab.
  **Every row's tooltip explains what it does, when to change it, and what
  numbers to enter for common scenarios — hover before asking "what do I put
  here?"**; the section further down also walks through several tuning goals
  end to end.

## Boot-time services

Both nvcurve-autoload (★ Default GPU profile) and centurion-tune (⏻ Optimizations
boot preset) are installed for either init system; enable only the one your
distro actually runs:

    # systemd
    systemctl enable --now nvcurve-autoload.service
    systemctl enable --now centurion-tune.service
    systemctl enable centurion-intel-uv.service           # Intel only

    # OpenRC
    rc-update add nvcurve-autoload default
    rc-update add centurion-tune boot
    rc-update add centurion-intel-uv boot                 # Intel only

Neither does anything until you've actually set a ★ default profile / ⏻ boot
preset from the GUI — enabling the service early is harmless.

## Installed files and permissions

    /usr/bin/centurion                    GUI + tray
    /usr/bin/nvcurve                                 nvcurve CLI
    /usr/bin/centurion-gamemode                            Lutris/Steam game-mode hook (user, no setuid)
    /usr/bin/centurion-intel-uv                            standalone Intel undervolt CLI (root)
    /usr/libexec/centurion/
        legion-profile-helper  fwattr-helper  ryzen-co-helper
        tune-helper  intel-uv-helper  legion-gpu-helper
        legion-firmware-helper  lighting-helper  amdgpu-helper  legion-ec-sensors  health-reader
        netguard-helper  backup-helper
        centurion-boot-guard                                          (root:root 0755)
        nvcurve-root-helper                                     (root:root 0700)
    /usr/share/polkit-1/actions/com.centurion.policy
    /etc/polkit-1/rules.d/49-centurion.rules
    /usr/lib/udev/rules.d/70-centurion-lighting.rules   keyboard lighting (uaccess)
    /etc/xdg/autostart/centurion.desktop  starts in the tray on login
    /etc/init.d/nvcurve-autoload                     OpenRC (GPU profile)
    /etc/init.d/centurion-tune                             OpenRC (tuning boot preset, runlevel boot)
    /etc/init.d/centurion-calibrate-boot                   OpenRC (calibration scheduled for the next boot; idle otherwise)
    /usr/lib/systemd/system/nvcurve-autoload.service systemd (GPU profile)
    /usr/lib/systemd/system/centurion-tune.service         systemd (tuning boot preset)
    /usr/lib/systemd/system/centurion-calibrate-boot.service systemd (calibration scheduled for the next boot)

Authorization (polkit): a `wheel` or `sudo` member at the machine (local,
active session) runs the helpers without a password. Everyone else (SSH, other
seats) is always asked for the administrator password. There is no longer a
graduated "security level" — the single tier is the password-free grant for a
local admin. `legion-firmware-helper` is the exception on top of that: it writes settings that persist in
firmware and can keep the laptop from booting — BIOS memory timings
(AodSetupRpl, including restoring a backup), BIOS CPU overclocking (PBO scalar,
boost override, all-core CO), the GPU MUX mode, and the forced "iGPU only"
override. Its action `com.centurion.firmware.write` asks for the
administrator password on every use (no caching, no silent grant), so no
program running as the user can change these unnoticed. The other helpers
refuse these operations.

Unchanged data locations: `/etc/nvcurve/{config.json,profiles/}` (GPU),
`~/.config/ryzen-curve-optimizer/profiles/` (CPU). New: `~/.config/centurion/
{tune-presets/,tune.json}` (tuning presets, ★ game preset),
`/etc/centurion/tune-boot.json` (root-owned boot preset),
`/run/centurion/tune/state.json` (saved originals; tmpfs).
Calibration and its records: `/var/lib/centurion/{signature.json,exposure.json,calibrate-on-boot.json,calibrate-inflight.json}`,
`/run/centurion/{calibrating.json,calibrate-status.json,co-state.json}`; while
`calibrating.json` names a live process, the hardware helpers (profile, firmware attributes,
GPU WMI, Curve Optimizer, Intel undervolt) refuse writes from anyone but that calibration and
the scene engine defers automatic switches (docs/AUTOTUNE.md, *Measurement conditions*).

## Supported machines

Hardware-safety rules taken from LenovoLegionToolkit (behaviour only, no code):

- China-market names without "Legion" in DMI (Y9000*, R9000*, Y7000*, R7000*,
  GeekPro G5000*) are accepted, in the GUI and in every helper.
- Firmware quirks are keyed on the BIOS prefix (`SMCN19WW` → `SMCN`):
  J2CN boards go Quiet → Balanced → Performance instead of straight to
  Performance; K1CN boards step through another mode when leaving Custom.
  Done in legion-profile-helper, so Home, tray, scenes and centurion-gamemode all
  get it; the reply's `via` names the intermediate mode.
- The Spectrum keyboard is only talked to on a 048D:C1xx / C6xx / C9xx hidraw
  interface whose HID descriptor declares a 960-byte feature report and that
  answers the compatibility query; the 4-zone keyboard only on a 048D:C9xx
  interface declaring a 33-byte feature report. Any other interface of the
  same device gets no ioctl at all.
- Custom mode is not offered (GUI) and refused (helper) on first-generation
  boards before the BIOS that fixed it: G9CN < 24, GKCN < 46, H1CN < 39,
  HACN < 31, HHCN < 20.
- IdeaPad Gaming: the EC's Fn-lock bit means "F1–F12 primary", so the Fn lock
  switch is shown inverted there.
- `machine.rs` carries the machine-type → series table and the generation rule
  ("16AFR10H" → 10); they select quirks only, never an address.
- Everything else stays capability-based: each control appears only where
  its sysfs node / WMI method answers (see the tabs' `present()` checks).

Checked by the GUI at start (`sysinfo::supportedMachine`) and by every Rust
helper in `centurion_helpers::init()` (`crates/centurion-helpers/src/machine.rs`), plus
`centurion-gamemode` and `centurion-intel-uv`: `/sys/class/dmi/id/sys_vendor` must be
`LENOVO` and `product_version` or `product_family` must contain the word
*Legion*, *LOQ* or *IdeaPad Gaming* (whole words, case-insensitive — e.g.
"Legion Pro 7 16AFR10H", "LOQ 15IRH8", "IdeaPad Gaming 3 15ACH6").
Elsewhere helpers answer `{"ok": false, "unsupported": true, ...}` and exit 1;
`centurion-gamemode RUN` / `WRAP` still start the game, just without tuning, so a
stale Lutris hook never blocks a launch. The standalone `nvcurve` CLI is not
restricted (it is a generic NVIDIA tool); its root helper is only reached
through the GUI and `nvcurve-autoload`.

## Tray & app behaviour

- Starts hidden in the tray; `--window` opens the window. Closing hides to the
  tray, Quit exits. No tray after ~6 s → the window is shown instead.
- A second launch never starts a second copy: it asks the running one to show
  its window (per-user QLocalServer, user-only socket permissions).
- Tray menu: Power Profile, CPU Curve (Ryzen), GPU Curve (NVIDIA) with saved
  profiles, reset entries and a 2.5 s cooldown, rebuilt on every open.
- The icon is drawn at runtime (same glyph as before); an SVG is installed for menus.

## Scenes

A scene is one name for the whole machine: power profile, firmware limits,
CPU curve (Ryzen CO or Intel undervolt), NVIDIA curve, Optimizations preset,
keyboard lighting (profile and brightness) and an optional command. It does not copy those settings — it names the
profiles the other tabs already save, so editing a profile updates every
scene that uses it. Each component can be left *Unchanged*.

Scenes are applied in dependency order through the same root helpers as the
tabs (power profile first, because firmware limits are only accepted in
Custom). A failing component is reported and the rest still run. Applying
never opens a dialog: results show in the Scenes tab, or as a tray
notification when the window is hidden.

- **CPU power limits are written in firmware order.** The EC keeps
  SPL (PL1) ≤ SPPT (PL2) ≤ FPPT (PL3) and silently drops a write that breaks
  it (lenovo-wmi-other ignores the WMI return code, so sysfs says "ok").
  `fwattr-helper` (and the WMAE fallback in `legion-gpu-helper`) therefore
  writes raises top-down (FPPT → SPPT → SPL), then lowers bottom-up, and the
  cross-loading limit `ppt_cpu_cl` last; every value is read back after a
  short settle, values that did not stick are written once more, and anything
  still different is reported as "firmware kept X (asked Y)". Before this the
  batch went alphabetically (`ppt_cpu_cl`, `ppt_pl1_spl`, …).

- **Optimizations presets are switched, not stacked.** A scene's preset is
  applied with `replace`: every knob the new preset does not set goes back to
  its original value, so nothing from the previous scene lingers.
  *Restore originals* puts everything back. While a game session holds game
  mode (`centurion-gamemode PRE`), scenes leave the tuning alone.
- **Firmware limits** are captured from the current firmware-attribute values
  (*Capture current*). The WMI-only GPU values (cTGP, Dynamic Boost) are
  included once the Firmware Attributes tab has read them this session.
- **Automatic switching** applies the chosen scene when the charger is
  plugged in or pulled (debounced) and once at login. A charger that stays
  connected counts as AC even when the battery dips under full load.
- **Run command** executes as the user, without a shell — for things this
  app does not manage (display refresh rate, audio profile…).

- **Game start** (Optimizations → Game launch → *System at game start*): pick
  a scene and the first game started through `centurion-gamemode PRE` / `WRAP`
  switches to it; the last `POST` returns to the scene that was active before
  (or to the AC / battery scene when automatic switching is on). The
  *Undervolt CPU / GPU* boxes decide whether the scene's curves are applied —
  unticked, the scene loads without touching them. The scene's Optimizations
  part is skipped while playing: the ★ game preset owns tuning. Automatic
  AC / battery switching waits until the last game has exited.
  `centurion-gamemode SCENE <name>` applies a scene from a script.
  Helper timeouts: only a pkexec still waiting for polkit is cancelled (after
  60 s). An authorized helper runs as root and cannot be cancelled, so it is
  waited for (up to 180 s) instead of being reported as failed — before, a
  slow game-preset apply made PRE leave the game scene while the helper went
  on and opened the game session, and the game ran on the pre-game scene.
  RUN waits for a live PRE (start lock) up to 10 min.

Files: `~/.config/centurion/scenes/<name>.json` and
`~/.config/centurion/scenes.json` (automatic switching); the
active scene is shared with centurion-gamemode in
`$XDG_RUNTIME_DIR/centurion/scene.json`. The tray
has a *Scene* menu with every scene and the auto-switch toggle.

## AMD GPU tuning (AMD GPU tab)

Shown when an `amdgpu` card exists (a discrete Radeon, or the CPU's integrated
graphics in Hybrid mode). The form is generated from what the driver reports
for the selected card, so each generation only gets the controls it has:

| `pp_od_clk_voltage` shape | Generations | Controls |
|---|---|---|
| per-state `OD_SCLK`/`OD_MCLK` with mV, range `VDDC` | Polaris, Vega 10 | clock + voltage of every state |
| `OD_VDDC_CURVE` | Vega 20, RDNA1 | min/max GPU clock, max memory clock, 3-point V/F curve |
| `OD_SCLK` 0/1, optional `OD_VDDGFX_OFFSET` | RDNA2, RDNA3, APUs | GPU/memory clock window, voltage offset |
| `OD_SCLK_OFFSET` | RDNA4 | GPU clock offset, memory clock, voltage offset |

Plus performance level, power profile (`pp_power_profile_mode`, CUSTOM not
offered), power limit (hwmon `power1_cap`) and the PMFW fan settings of RDNA3+
(`gpu_od/fan_ctrl/*`: zero-RPM, minimum speed, target temperature, acoustic
limits, 5-point curve).

- **Overdrive** needs bit `0x4000` in `amdgpu.ppfeaturemask`. Without it the tab
  shows the exact parameter (current mask | 0x4000) and Optimizations → Boot
  options lists it; nothing edits the bootloader.
- **amdgpu-helper** takes the complete wanted state of one card, re-reads every
  range from sysfs, rejects the whole request if anything is out of range, and
  only then writes: level `manual` → OD `r`, edits, `c` → power cap → fan
  (value + `c`) → profile → wanted level. A failed OD write restores the stock
  table before reporting. Extra limits on top of the driver's: clocks ≤ 4000
  MHz, voltages ≤ 1300 mV, voltage offset ≤ 0 (undervolt only). A value equal
  to what the driver currently reports is always accepted (some firmware ships
  stock points outside its own OD_RANGE).
- **Trial apply.** Every Apply runs as a trial: unless *Keep* is pressed within
  20 s, the previous settings are written back. *Undervolt step* lowers the
  voltage offset by 10 mV per trial — step until the game or stress test
  crashes, keep the last step that held. Nothing is applied at boot, so a hang
  is undone by a reboot.
- **Stock reference** (for the presets) and the last kept summary live in
  `~/.config/centurion/amdgpu-state.ini`; profiles in
  `~/.config/centurion/amdgpu/*.json` (with the card's PCI id).
- `CENTURION_AMDGPU_DRM=<dir>` points the tab at a fake `/sys/class/drm` tree (dev aid).

## nvcurve: sensors, PowerMizer, full reset

- **Temperatures NVML doesn't give on GeForce** (`nvcurve sensors`, NVIDIA tab):
  hotspot = NvAPI thermal channel 9 up to Ada, the aggregated hotspot register
  `0x00AD0AA0` on Blackwell; VRAM = channel 15 (GDDR6/6X) or, on Blackwell
  (GDDR7), the hottest per-partition register sensor with channel 10 as
  fallback. `--raw` lists every readable channel.
- **Throttle reasons** (NVML clock event reasons) in `sensors` and the tab's
  *Limit* field.
- **PowerMizer** (driver ≥ 580): `nvcurve powermizer status|set`, tab combo.
- **`nvcurve reset-all`** / *Reset All*: curve offsets on every point, NVML core
  and memory offsets in **every** P-state, core/VRAM clock locks, power limit,
  PowerMizer → Auto.
- **NVML ClockOffset fix**: `nvmlClockOffset_v1_t` is 24 bytes (offset + min +
  max); the old 16-byte struct was always rejected on version mismatch, so the
  per-P-state API never ran. Core offsets now use it; memory offsets still go
  through the per-domain call first (the ×2 raw scaling was measured on that
  path) — `CENTURION_NVML_MEM_NEW_API=1` tries the new API first.
- **V/F point classification** from the driver's per-point records in the
  GetClockBoostMask buffer (`type`, `bVoltageBased`); the old flags heuristic
  is the fallback. `nvcurve read` prints which one was used.
- **0 MHz floor**: negative offsets that would take a point to ≤ 0 MHz are raised
  to the point's floor (base frequency from the V3 status when the driver has
  it); read-back verification accepts exactly that adjustment.

## Firmware interfaces and how each address was checked

Everything Centurion sends to Lenovo firmware, with the independent sources each
number was compared against: **LLT** = LenovoLegionToolkit, **K** = the
kernel's `Documentation/wmi/devices/lenovo-wmi-{gamezone,other}.rst` and
`drivers/platform/x86/lenovo/`, **LLL** = LenovoLegionLinux `legion-laptop.c`,
**HW** = decompiled DSDT + live test on the 16AFR10H (SMCN19/20WW).

| Interface | What Centurion uses | Checked against |
|---|---|---|
| GameZone `887B54E3-…` (WMAA) | 40/41/42 GSync (MUX), 49/50/51 Over Drive, 56 BIOS OC mode, 63/64/65 iGPU mode | K, LLL, HW |
| | 21/23/22 Super-key lock, 24/26/25 touchpad lock (IsSupport / Get / Set; 1 = disabled) | K, LLL, LLT (semantics) |
| | 43 SmartFan version, 55 thermal mode, 62 IsACFitForOC, 47 GetPowerChargeMode — read-only | K, LLL, LLT |
| Other method `DC2A8805-…` (WMAE) | 17 GetFeatureValue / 18 SetFeatureValue, `{u32 id, u32 value}` | K, LLL, HW |
| | ids: 0x0101–0x0104 CPU limits, 0x0201/0x0202/0x020B GPU, 0x04020000 fan full speed, 0x0301000{1,2} instant boot, 0x00100000 Fn+Q custom, 0x05050000 GPU temp | LLT, K, LLL, HW |
| | 0x00030000 flip to start, 0x001A0000 Over Drive, 0x0403000{1,2,4} fan RPM — only when the capability list marks them valid | LLT, LLL |
| Capability data 00 `362A3AFE-…` | `{u32 id, u32 flags, u32 default}`; flags bit0 valid, bit1 get, bit2 set | K |
| Fan method `92549549-…` (WMAB) | 5 Fan_Get_Table, 6 Fan_Set_Table (`FSTM, FSID, FSTL, 10×u16`) | LLL, LLT, HW |
| | 1 Fan_Get_FullSpeed, 2 Fan_Set_FullSpeed — fallback where 0x04020000 is not listed | LLL, LLT |
| VPC2004 (`ideapad_laptop`) | sysfs `fn_lock`, `usb_charging`, `camera_power`, `charge_types`, LED `platform::kbd_backlight` | K |
| | HALS bit 15 / SALS 0x13, 0x12: USB charging on battery (bits 6/7 as in K) | LLT only |
| UEFI `FBSWIF-d743491e-…` | 4 data bytes, byte 0 = flip to start; attributes and other bytes preserved | LLT, LLL |
| CPU method (WMAC 0x0E), WQA6 | BIOS CPU OC tunes 0x414D4401–03 | LLT (ids), HW |
| Spectrum HID | 960-byte feature report id 7 | LLT, HW |
| 4-zone HID | 33-byte feature report `CC 16 effect speed brightness 4×RGB 00 ltr rtl` | LLT |

Notes:

- The fan table is written with `FSTL = 0` (as LLL does); LLT writes 10. LLL's
  DSDT notes say the firmware ignores the field; the HW-verified form is kept.
- Rows marked "LLT only" have a single source: USB charging on battery is
  therefore read back after every write, and a firmware that accepts the
  command without changing the bit is remembered (per machine + BIOS) and the
  switch is not offered again.
- Deliberately **not** implemented because the references disagree or the
  address could not be confirmed: lid-logo / port lights through
  `LENOVO_LIGHTING_METHOD` (LLT writes state 0 for "off", LLL writes 2),
  battery night charge (Windows driver IOCTL only), first-generation Custom
  mode limits (`LENOVO_CPU_METHOD` / `LENOVO_GPU_METHOD` buffer layouts),
  the pre-capability "feature flags" methods, GameZone AI sub-mode.
- On the 16AFR10H nothing verified earlier changes path: the WMAE full-speed
  switch is pinned for the SMCN BIOS family, Over Drive still asks GameZone
  first, and the three old WMAE switches keep their old rule.

## Keyboard lighting (Lighting tab)

Laptops with the "Spectrum" per-key RGB keyboard (ITE 8258 controller, USB
`048d:c1xx` on Gen10 such as the Legion Pro 7 16AFR10H / 16IAX10H, `c9xx` on
Gen7–9 and Legion 9, `c6xx` on LOQ Gen10 / Legion 5 Gen11) get the Lighting tab
described here. Machines with the older 4-zone RGB keyboard (`048d:c9xx`,
33-byte report) get a small Lighting tab instead (`zone4tab.cpp`: effect,
speed, brightness, four colours; write-only controller, last applied values
kept in `~/.config/centurion/zone4.ini`; helper ops `z4_probe` /
`z4_set`). Without either controller no Lighting tab is created; a white
backlight is a level selector on Home (the kernel's `platform::kbd_backlight`).

- **Profiles.** The controller stores six lighting profiles itself (shared
  with Windows / Legion Space, kept across reboots). Choosing a profile
  activates it and loads its effect list into the editor.
- **Editor.** The keyboard is drawn from the controller's own key map (keys,
  18 rear + 10 front/side accent lights, lid logo). Select lights by click /
  drag (Ctrl/Shift adds), then *Paint selection* (a static colour), *Turn off
  selection*, or build effects (static, colour change / pulse / wave, smooth,
  rain, ripple, type lighting, rainbow wave / spiral) with speed, direction and
  colours. A light belongs to one effect at a time.
- **Apply** writes the profile into the controller. The whole profile must
  fit one 960-byte report (shown under the editor): about 36 distinct colours
  when every light is used. Writes land in the controller's non-volatile
  store — apply when done, not in a loop.
- **Brightness (0–9) and the lid logo** apply immediately; the tray has
  *Keyboard Lighting* (profile, brightness, lights off) and scenes can set a
  profile and brightness.
- **Not available:** Audio bounce/ripple and Aurora (screen) sync need host
  processing that exists only in Windows; profiles containing them are shown
  and kept, not edited. Host-driven per-frame colour (LLT's Aurora protocol)
  is ignored by the Gen10 firmware.
- **Access.** `lighting-helper` runs as the user: the udev rule
  `70-centurion-lighting.rules` tags the controller's hidraw node
  `uaccess` (systemd-logind or elogind). It applies to nodes created after the
  rule is installed — re-login or replug once; until then the tab offers
  *Use administrator rights* (pkexec, polkit action
  `com.centurion.lighting.write`). Nothing at login ever prompts.
- Protocol: `crates/centurion-spectrum` (feature reports, report id 7, from
  LenovoLegionToolkit; key legends after legion-spectrum-control, MIT).

## Boot guard

`centurion-boot-guard` (pulled in by every preset service: `centurion-tune`,
`nvcurve-autoload`, `centurion-intel-uv`, `centurion-intel-uv-daemon`, and the elogind
resume hook) keeps an unstable preset from crash-looping the machine. Each
boot is *armed* until it has run for 3 minutes or shuts down cleanly. A boot
that ends while armed (panic, hang, forced power-off) trips the guard: the
preset services are skipped — not failed — on every following boot until you
press *Resume boot presets* on Home (or run `centurion-boot-guard reset` as root).
The automatic login scene has the same protection at user level.
State: `/var/lib/centurion/boot-guard.json`
(`centurion-boot-guard status`).

## GPU mode (Home → Device)

Switches the MUX through the firmware's own GameZone WMI call
(`\_SB.GZFD.WMAA` 0x29 / 0x2A — the SMI Legion Space uses); the change takes
effect at the next boot. *Hybrid*: the AMD iGPU drives the panel and the
NVIDIA GPU can power off; *dGPU only*: the panel is wired to NVIDIA. Hybrid
is refused when the running kernel has no `amdgpu` (the panel would stay
black) unless you confirm a second warning; the BIOS setup (F2) has the same
switch as a way back. Needs `acpi_call`.

## Boot options (Optimizations → Boot options)

Read-only advisor for kernel command-line and NVIDIA module options: shows
what is already in effect, explains each, and builds a copyable command-line
addition and `/etc/modprobe.d/nvidia-centurion.conf`. It never edits the
bootloader.

## Export / import (Scenes tab)

One JSON file with scenes, automatic switching, Optimizations presets,
game-launch settings, Ryzen / Intel profiles and NVIDIA curve profiles.
Import shows what is inside, warns when the file comes from another model or
BIOS (curve offsets are chip-specific), and asks whether to overwrite or keep
existing items. NVIDIA profiles are written through `nvcurve-root-helper`,
which validates them.

## Backup (Health → Backup)

`backup-helper` (engine: `crates/centurion-helpers/src/backup.rs`) streams: one JSON
request line on stdin, `{"event":"start"|"progress"}` lines while it works, one
final `{"ok":…}` line. Stdin stays open; EOF cancels (tar and the compressor
run in their own process groups and are terminated, the `.partial` archive is
removed). polkit action `com.centurion.backup`: AUTH_ADMIN, never
cached, for everyone (including a local admin).

**System image** (`system_backup`, root):
`tar --acls --xattrs --xattrs-include='*' --ignore-failed-read --exclude=… -cvpf - / | pigz -pN -6`
→ `<folder>/backup-YYYY-MM-DD.tar.gz` (`_HHMMSS` added when the name exists;
`.tar.zst` / `.tar.xz` with the other compressors), 0600 root, written as
`.partial` and renamed, plus `<archive>.info.json` (entry count, kernel,
excludes, tar status, `level`, `chain`, `encrypted`).
**Increments** (`incremental` true): every image gets a root-owned snapshot (GNU tar `--listed-incremental`;
a missing file makes a full dump): `<archive>.snar`, or for an encrypted image
`/var/lib/centurion/backup-snar/<FNV-1a of the folder>-<name>.snar` (it names every archived path; the
folder is excluded from images). An increment copies the newest image's snapshot to a `.partial`, names the image
`backup-DATE_HHMMSS-iN.tar.<ext>` and records `level` N and `chain` (the full image's name).
It continues a chain only with the same encryption (`encryption` none / `recipient` + key / `passphrase` in
the sidecar); otherwise, or after `MAX_LEVEL` (6) increments, the image is a full one and the result's `notes` say
why. A passphrase is checked against the chain's newest image first (`key_opens`: age decrypts and authenticates
the first chunk) — a wrong one stops the backup. Retention (`prune`) works per chain: the newest `keep` chains
stay, and a chain is removed whole with its sidecars and snapshots.
**Encryption**: `recipient` = `age1…` → `age -r <key>`; `passphrase` → `age -p`. age 1.1.1 reads a
passphrase only from a terminal (and ignores `AGE_PASSPHRASE`), so age runs in a session of its own with a
pseudo-terminal as its controlling tty (`openpty`, `setsid`, `TIOCSCTTY`); a thread answers each
"…passphrase…:" prompt on the master side (twice when encrypting, at most four times). Reading with a secret
key: `identity` (the `AGE-SECRET-KEY-1…` lines of what was pasted or loaded) is written into a pipe that age
reads as `-i /dev/fd/3`. Secrets come only from the request on stdin, are never logged or written to a file,
and are overwritten when dropped (`Secret`). A passphrase image is verified at creation; a recipient image only
when an `identity` comes with the request (otherwise later, by `verify`).
 Default excludes use the `/proc/*` form, not `/proc`:
the empty mount points stay in the archive, so a restore onto a fresh file
system boots. The archive itself and older `backup-*.tar.*` files in the
folder are always excluded; `/home/*` unless "Include /home". tar status 1
(files changed while read) is normal on a live system; status 2 keeps the
archive and flags it. Optional: verify (read back through
`<decompressor> | tar -t`, entry count compared), retention (newest N
`backup-YYYY-MM-DD*` images kept), nice 19 + lowest best-effort I/O priority.

**Restore** (`system_restore`, root): compression from the magic bytes (an encrypted image: `age -d`, with `-i /dev/fd/3` or the passphrase prompt,
then the compression named by the `.tar.<ext>.age` file name), then
`<decompressor> | tar --acls --xattrs --xattrs-include='*' --numeric-owner -xvpf - -C <target>`.
An increment is restored after its full image and every increment before it (missing ones are named,
never skipped; each step emits a `start` event with `step`/`steps`). The steps follow each increment's
`parent` (the file name of the image it continued, in its sidecar), so a second increment of the same level —
made after the snapshot of the newest one was lost — is never mixed in; sidecars without `parent` fall back
to one image per `level` of the `chain`. tar's own incremental extraction
(`--listed-incremental=/dev/null`) is **not** used: it deletes every entry of an archived directory that was not
there when the image was made — e.g. `/home/<a newer user>` (the images exclude `/home/*`), or the disk mounted under
`/mnt` that the restore reads from. Instead the stream passes through `tarscan` (reads GNU/ustar/pax headers,
`L` long names, the `D` dumpdirs of GNU format and the `GNU.dumpdir` pax records GNU tar writes once `--xattrs`
selects POSIX format) on its way to a plain `tar -x`, and `ChainRestore`:
renames (`R`/`T`) are applied before the bytes behind them reach tar; after each increment, an entry is deleted
only if the chain itself restored it (hashed member names of the earlier steps) and the increment's dumpdir no
longer lists it — through `openat`/`unlinkat` with `O_NOFOLLOW`, never across a mount point (`st_dev`), a
directory only once it holds nothing unknown. A stream it cannot read (bad checksum, multi-volume) disables
deletions for that step. The result reports `deleted`, `renamed` and `notes`.
The request must repeat the resolved target (`confirm`); the GUI asks to type
RESTORE for `/`. Missing mount points (`/proc /sys /dev /run /tmp /var/tmp
/mnt /media /home`) are created afterwards, so images made with a plain
`--exclude=/proc` restore to a bootable tree too. `--numeric-owner`: correct
when restoring from live media whose passwd differs.

**Compare** (`config_diff`, as the user): `a` and `b` (or `b` = `"current"`: the live configuration staged by
the same code as `config_backup`) are unpacked into a 0700 scratch directory, links kept as links (compared by
target, never followed), and read into memory. Two archives are ordered by `created` (then file time; `swapped`).
JSON files are compared key by key (`a.b[0].c`), `.ini` by section and key, and any text where ≥ 80 % of the
non-comment lines are `KEY=value` (kernel `.config` with `# CONFIG_X is not set`, conf.d, make.conf) by key;
other text by the lines only in one side (multiset, order ignored; `reordered` when only the order changed).
With `profile` and a system profile in both, `packages.txt` gives `packages` added / removed / changed (same
`category/name`, version split at the first `-digit`). Logs and per-boot facts are skipped (`ignored`); in
"current" mode, paths that could not be read now are `unreadable`, not removed. Lists are capped at 200.

**Centurion configuration** (`config_backup`, as the user): readable files are staged
and packed as `centurion-config-YYYY-MM-DD_HHMMSS.tar.gz` (0600):

    Centurion-BACKUP.json                 manifest: created, host, model, BIOS, kernel, counts, unreadable files
    user/config/…                   ~/.config/{centurion,ryzen-curve-optimizer}
    user/state/…                    logs (optional)
    system/etc/centurion, system/etc/nvcurve, system/var/lib/centurion,
    system/etc/modprobe.d/zz-centurion-nvidia.conf
    system-profile/                 reference only, never restored: root/etc/portage, world, kernel .config,
                                    fstab, grub/dracut/kernel, modprobe.d, modules-load.d, sysctl, conf.d,
                                    runlevels, udev rules; cmdline, DMI, modules, packages.txt
                                    (/etc/portage/gnupg is left out: Portage's regenerable keyring, root 0700)

Restore: the GUI first saves the current state (`…-pre-restore.tar.gz`), then
`config_restore_root` (pkexec) and `config_restore_user`. The root part goes
through a whitelist — `/etc/centurion/*`, `presets/*`,
`/etc/nvcurve/{config.json,profiles/*}`, the calibration files of
`/var/lib/centurion` (signature, calibration, outcomes, io-probe,
wmae-verified; not the boot-guard / boot-default state) and Centurion's NVIDIA
modprobe file — and every file is re-created root:root 0644 with an atomic
write; owner, mode and symlinks in the archive are ignored. Files that are not
in the backup stay.

## Components

Cargo workspace:

| Crate | Content | Status |
|---|---|---|
| `crates/centurion-helpers` | pkexec root helpers: `legion-profile-helper`, `fwattr-helper`, `ryzen-co-helper` | step 1 ✔ |
| `crates/centurion-helpers` `intel_uv` | Intel undervolt core, `intel-uv-helper`, `centurion-intel-uv` CLI | ✔ |
| `crates/centurion-spectrum` | Spectrum keyboard protocol (hidraw): key map, effects, profiles; `lighting-helper` in centurion-helpers | ✔ |
| `crates/nvcurve` | nvcurve core library: NvAPI + NVML (dlopen), HAL, safety, snapshots, profiles, autoload | step 2a ✔ |
| `crates/nvcurve` bins | `nvcurve` CLI, `nvcurve-root-helper` | step 2b ✔ |
| nvcurve REST/WS server | not ported — nothing in the GUI uses it | — |

    cargo test --release

## nvcurve core — mapping from the Python package

| Python | Rust |
|---|---|
| nvapi/bootstrap, constants, errors, types | `nvapi.rs`, `types.rs` |
| pynvml | `nvml.rs` (only the entry points used, resolved lazily by name) |
| hal/gpu, vfcurve, ranges, monitoring, limits, snapshot | `hal/*.rs` |
| safety, atomicio, config | `safety.rs`, `atomicio.rs`, `config.rs` |
| profiles/native, apply | `profiles/*.rs` |

No runtime dependency on Python, pynvml or nvidia-ml-py. Linking needs no
NVIDIA libraries; a missing driver is an `NvError::Unavailable`.

### Behavioural differences (all deliberate)
- `snapshot::restore(None)` now picks the newest *regular* `.bin` and applies the
  same snapshot-dir containment check as an explicit path. The Python version
  would follow a planted symlink that sorted last.
- `validate_limits` reports the offending field structurally; `apply_profile`
  no longer substring-matches error text to decide what to drop.
- `apply_profile` reads the ClockBoostTable once and reuses it for both the
  auto-snapshot and the write baseline (was two reads).
- nvidia-smi fallback only runs a root-owned, non-writable binary from a fixed
  list (`/usr/bin`, `/opt/bin`, `/usr/sbin`) — never `$PATH`.
- Profile loading rejects non-integer numeric fields at load time (was: loaded,
  then failed later). One bad file is skipped with a warning, as before.
- Profile-name filter uses Rust's Unicode `is_alphanumeric`; this can differ from
  Python's `str.isalnum` only for rare combining marks.

## nvcurve root helper

Same ops and JSON protocol as `root_helper.py`; results still land in
`/run/nvcurve-gui/*.json` in the same shape. Differences:
- No `python3 -m nvcurve` subprocesses at all (memlock, default profile and
  autoload were still shelled out) — one process, one NvAPI session.
- `write_nvcurve_profile` now applies the same schema check as
  `apply_gpu_offsets` (was: "any valid JSON"). Root applies this file later.
- Deleting a profile clears *every* auto-load entry that references it; the
  Python helper ran `profile default --clear`, which only cleared GPU 0's key.
- `profile_dir` is pinned to `/etc/nvcurve/profiles` (where the helper writes)
  instead of following config.json, so write and apply can't diverge.
- autoload's log output is captured and returned as the op message.

## nvcurve CLI

Ported: `read [--full|--json|--raw]`, `inspect`, `write`, `snapshot`, `gpus`,
`profile`, `memlock`, `autoload`; global `--gpu N`. Log level via
`NVCURVE_LOG=debug`. Not yet: `serve`, `daemon` (2c), `verify`, `setup`,
`read --diag`. Dropped: `service` (systemd; OpenRC init script comes in step 4).
Snapshot/profile dirs now follow `/etc/nvcurve/config.json` in every command
(the Python CLI used built-in defaults for `snapshot`).

### First hardware test (read-only first)
    nvcurve gpus
    sudo nvcurve read
    sudo nvcurve read --json | head -40
    sudo nvcurve inspect --point 0
    sudo nvcurve write --global --delta 0 --dry-run

## Qt6 GUI — `gui/`

    cmake -S gui -B gui/build -DCMAKE_BUILD_TYPE=Release
    cmake --build gui/build -j
    gui/build/centurion

Requires Qt ≥ 6.4 (Widgets) and a C++20 compiler. Helper directory is
`-DCENTURION_HELPER_DIR=/usr/libexec/centurion` (default; must match polkit).

Done: theme (same tokens/QSS), async pkexec runner, platform-profile reader,
Home tab (profile switcher, hardware inventory, live sensors).
All four tabs are ported (3a–3d). Tray + install layout: step 4.

Differences from the PySide6 Home tab:
- pkexec runs asynchronously; the window no longer freezes while the polkit
  dialog is open, and profile buttons are disabled during the switch.
- nvidia-smi runs asynchronously with a 3 s kill timeout, never stacked; the
  Python tab blocked the GUI thread up to 3 s every 2 s poll.
- hwmon sensors are ordered numerically (temp2 before temp10).
- `CENTURION_SCREENSHOT=/tmp/x.png` renders the window once and exits (dev aid).

### Look and theme contract

All colour, spacing and control styling lives in `gui/src/theme.{h,cpp}`; tabs
only use the tokens and selectors below, so a palette change reaches the
custom-painted views (V/F curve, fan curve, keyboard, tray icon) too.

- Surfaces: `BG0` window → `BG1` card → `BG2` inner panel → `BG3` control →
  `BG4` hover. Text inputs sit in a recessed `WELL` (darker than the card);
  buttons are one step above it. Amber `ACCENT` marks only what is active.
- Selectors: `btnAccent`, `btnDanger`, `btnMini`, `terminal`, `miniSlider`,
  `box_<colour>` (group title colour), `role=muted|title|value` (`value`: a
  live readout, brighter and heavier than its label), `mainTabs`
  (the header band). A `QGroupBox` inside another renders as a divided
  sub-section, not a second card.
- Header: the brand mark (icon + "Centurion" wordmark, `appMark` /
  `appWordmark`) sits at `theme::BRAND_X`; the page tabs are a recessed rail
  that starts at `theme::brandInset()` (measured with the wordmark's font).
  `MainWindow::fitTabs()` caps the rail to its keys and to the window, so it
  hugs the tabs and scrolls when the window is narrow. Rail keys keep one font
  weight in every state, so the rail never changes width with the selection.
- Surfaces derived per palette in `stylesheet()` (`mix()` of two palette
  colours, never a fixed grey): bezel, page floor, rail, lit top edges.
- `theme::banner(color, selector)` is the one callout style (profile lock,
  tuning state, crash guard, max fans, lighting access, AMD trial).
- `theme::profileAccent()` follows the Legion power-button LED: Power Saver
  blue, Balanced white, Performance red, Custom purple (Extreme magenta).
- The app style is Fusion behind a small proxy that gives nested layouts a
  6 px spacing (Fusion's fallback of 0 drew button pairs touching).
- Glyphs in `gui/data/theme/` (chevrons, check) are drawn for this palette at
  1× and 2×.
- Themes: 8 premade dark palettes in `PALETTES` (theme.cpp) — Graphite
  (default), Gruvbox, Dracula, Tokyo Night, Nord, Catppuccin Mocha, Crimson,
  Glacier. Picked on Home (top right), stored as `theme=` in
  `~/.config/centurion/gui.ini`, loaded before the first widget.
  A change restarts the app in place (`execv /proc/self/exe --window` after
  the event loop ends and the instance lock is released). Adding a theme is
  one row of 19 colours.

## Firmware Attributes tab

Port of `fwattr_tab.py`, same fwattr-helper protocol (single + batch).
Differences:
- Values are snapped to the driver's `min + k·step` grid before writing
  (the Python tab could send off-grid values; the helper only checks range).
- Writes are async; the tab is disabled while pkexec is pending, so a second
  Apply can't race the first.
- Home tab's profile switch unlocks/locks the tab immediately (signal).
- "Default" button no longer clipped (was fixed 60 px).
- `CENTURION_FWATTR_BASE=/path` renders against a fake tree (dev only; the helper
  still rejects anything outside `/sys/class/firmware-attributes`).

## Ryzen Curve Optimizer tab

Port of `ryzen_curve_optimizer.py`: CCD/L3 topology + CPPC detection, fixed
8-slot-per-CCD SMU grid, disable checkboxes, per-CCD fill, all-core, reset,
profiles in `~/.config/ryzen-curve-optimizer/profiles` (same files as before).
Differences:
- **Bug fix:** applying a saved profile from the tray fired the per-core batch
  while the all-core pkexec call was still running; it was rejected as "busy",
  so profiles with both parts only ever got the all-core half. Now chained:
  per-core runs after all-core succeeds.
- Profiles are written with QSaveFile (atomic, fsync) and reads are capped at 256 KiB.
- Layout redesign: **Reset Curve Optimizer** is a global action (it clears
  all-core *and* per-core offsets, now with a confirmation) next to Profile;
  Apply All-Core and Apply Per-Core share one width and right edge. Each CCD is
  a card with a single aligned grid (Slot · Offset · Disable · CPPC, header in
  the same grid), columns spread evenly, ★ marks the two best CPPC cores per CCD.
  The grid scrolls instead of being squeezed on short windows.
- The tab's private Gruvbox sheet is gone; it uses the app theme.

## NVIDIA Curve Optimizer tab

Port of `nvcurve_gui.py` (embedded mode), same nvcurve-root-helper ops and
`/run/nvcurve-gui/*.json` result files. The V/F chart is a QPainter widget
(no QtCharts). Differences, most of them fixes:
- **Group drag clamp bug:** the Python widget clamped every *frequency* to
  800–3000 MHz while dragging, so shifting the whole curve lifted all
  sub-800 MHz idle points to 800 — large unintended positive offsets that
  then got applied. Now *offsets* are clamped to the driver range (+1000 / -2000 MHz) and
  frequencies only to ≥ 0.
- **Arrow-key edits** in the Python widget changed only the plotted points and
  were lost on the next recompute (e.g. touching the core spin box). All edits
  now go through one offset model.
- **Stale result files:** a result JSON is only accepted if it was written
  during the current operation (the Python tab re-read an older file if the
  helper's best-effort write failed).
- **NVML** is opened (dlopen) only while the tab is visible and closed when
  hidden, so the dGPU can reach runtime D3cold; pynvml is no longer needed.
  Mem/hotspot temperature labels were dropped (pynvml has no such sensor
  constants, so they always read N/A).
- The first curve read (which may raise a polkit prompt) happens on first
  show, not at startup.
- "Apply Profile" button applies a saved profile directly (`apply_named_profile`);
  selecting a profile in the combo only loads it into the editor, as before.
- Ctrl+click toggles a point, Shift+click selects the range from the last clicked point
  (Ctrl+Shift adds it), Ctrl/Shift+drag on empty space box-selects (added to the selection), Ctrl+A selects all, Shift+↑/↓ steps 15 MHz.
- `CENTURION_NVCURVE_FAKE=/path/read.json` renders a canned curve (dev only).

## Optimizations tab — lutris-game-tune, integrated

Everything `lutris-game-tune.sh` does at PRE/POST, plus the tuning knobs that
came up during the sysfs review, as one tab backed by a Rust root helper.

    crates/centurion-helpers/src/tune.rs          allowlist table + sysfs logic (shared)
    crates/centurion-helpers/src/bin/tune-helper  pkexec target: apply / restore / boost / boot
    crates/centurion-helpers/src/bin/tune-helper.rs is also built as tune-profile-helper (Cargo [[bin]]): only
    apply_preset (values from the root-owned store /etc/centurion/presets, by name) / restore / release /
    boost / boot / tools. Raw values, preset_save/delete, set_boot and driver options stay in tune-helper.
    crates/centurion-helpers/src/bin/centurion-gamemode Lutris/Steam front end (user process)
    gui/src/optimizetab.{h,cpp}             the tab
    packaging/openrc/centurion-tune               boot preset service

**Rows** (59, grouped CPU · Memory · Scheduler · Storage · Devices · Stability):
all lutris-game-tune parameters (governor/EPP, epp_boost, X3D mode, ASPM, deep
C-states, VM set, MGLRU, THP ×3, split-lock, watchdog, autogroup, CFS slice,
debugfs scheduler knobs, HDA power save, PCI latency timers) and new ones:
amd-pstate mode, boost, min freq = lowest_nonlinear, **SMT**, **CCD parking**
(hot-plug), khugepaged defrag, MGLRU min_ttl_ms, KSM, `vm.max_map_count`,
`numa_balancing`, `timer_migration`, preemption model, **unbound workqueue
cpumask** and **IRQ affinity** by CCD role, I/O scheduler / WBT / read-ahead
per disk, USB autosuspend, amdgpu iGPU DPM level, MCE poll interval.
Rows the machine does not have are shown greyed out ("n/a").

**Per-CCD governor / EPP / boost / max frequency.** amd-pstate has one
cpufreq policy per CPU, so `Governor · CCDn`, `EPP · CCDn`, `Boost · CCDn`
(per-policy `boost`, 6.11+) and `Max frequency · CCDn` write only the
policies whose CPUs sit on that L3 domain; the GUI names each die's role,
e.g. "(V-Cache)". The global `Scaling governor` and `Energy-performance
preference` rows are **hidden on any 2+ CCD chip** — the CCD rows cover the
same files and always apply after where the global row would, so showing
both invited setting one and wondering why the other value stuck. Set both
CCD rows the same for one governor/EPP across the chip, or split them (that's
what Gaming X3D and Competitive do). A single-CCD chip has no CCD rows, so
the global ones are the only way to set this there and stay visible.
`epp_boost` stays global regardless: the patch series has no per-policy
file. EPP's read-only `custom` state is not offered.

**CCD roles instead of CPU lists.** Values like `cache`, `frequency`, `ccd1`
are resolved against the live L3 topology (largest L3 = V-Cache die; highest
`cpuinfo_max_freq`, or the smaller-L3 die on a tie = frequency die), so one
preset works on any X3D part. Symmetric parts only offer `ccdN`; single-CCD
parts hide these rows. cpu0's CCD is never offered for parking.

**Reversibility.** The first write to each concrete file records its
original value (before the write, so a crash mid-batch is still restorable).
"Restore originals", the per-row ↺, the last POST and `rc-service centurion-tune
stop` write them back. Order matters and is fixed by the table: amd-pstate
mode first (it resets per-policy files); SMT and CCD parking last on apply
(an offline CPU's cpufreq policy returns EBUSY) and **first** on restore.

**Presets.** Six built-ins — Gaming X3D, Competitive, Low latency desktop,
Compile throughput, CO validation, Quiet battery — plus your own (Save as…).
A preset stores the checked rows and a `run` block (nice, autogroup,
CCD affinity). Loading skips values the machine does not offer and says which,
and checks every other row at its boot-guard default (`/var/lib/centurion/defaults.json`;
a row the snapshot lacks takes its live value while Centurion has not changed it), so a
loaded preset - and a preset saved from it - describes the whole state. The Wi-Fi / Bluetooth /
WWAN radio rows are the exception: they are never filled in (left as they are) and change only
when you check them yourself. Presets saved by earlier versions, whose fill took the radios from a
snapshot taken before the Wi-Fi was unblocked (Wi-Fi off at every scene change), are repaired
once at start-up, and the app offers to re-approve their root-owned copies and the boot preset. The tray and the
drift re-apply send the same full set.
★ *Use for games* makes it centurion-gamemode's default; ⏻ *Apply at boot* stores the
checked rows (validated by root) in `/etc/centurion/tune-boot.json`.

**Lutris** (the Game launch sub-tab has copy buttons):

| Field | Value |
|---|---|
| Pre-game script | `/usr/bin/centurion-gamemode PRE` |
| Post-game script | `/usr/bin/centurion-gamemode POST` |
| Command prefix | `/usr/bin/centurion-gamemode RUN` |
| Steam launch options | `centurion-gamemode WRAP -- %command%` |

Append a preset name to pin one per game (`PRE "Competitive"`). Game mode is
reference counted like lutris-game-tune; WRAP forwards SIGINT/TERM/HUP to the
game so POST still runs when Steam stops it.

Differences from lutris-game-tune, all deliberate:
- **No setuid wrapper.** centurion-gamemode runs as the user; root work goes through
  `pkexec tune-helper`. The RUN boost renices only the process that called
  pkexec, and only if its real uid is the authenticated user (`PKEXEC_UID`).
- **CCD isolation uses affinity + kernel steering, not cgroup moves.** The v4
  cgroup constrain/move logic (and its session watcher) is replaced by
  `sched_setaffinity` inherited by the game tree, unbound workqueue and IRQ
  steering to the other CCD, and optionally parking that CCD. No process is
  ever moved between cgroups, so the elogind/D-Bus incident class cannot recur.
- The X3D `_DSM` write keeps lutris-game-tune's 3 s timeout (thread + abandon).
- PCI latency timers are written natively (one byte at config offset 0x0D via
  pwrite; never a full config write) instead of via `setpci`; per-device
  refusals are skipped like `setpci … || true`.
- THP / shmem / defrag are ordinary rows again (v3 forced `always`).
- State lives on tmpfs: a reboot is a full restore, as before.

Security: the tune-helper request carries only keys and values. Every path is
derived from the table and re-checked before writing (realpath inside /sys,
fixed `/proc/sys` files from the table, `/proc/irq/<n>/smp_affinity_list`);
values are revalidated against live option lists and ranges. The polkit rule
grants it without a prompt like the other everyday helpers — any process running
as the active user can therefore retune the kernel (all of it reverts on reboot);
tighten `49-centurion.rules` as described in that file if that
matters. Firmware-persistent writes are never password-less (see Install).

Dev aids: `CENTURION_TUNE_FAKE=/path/describe.json` renders canned data,
`CENTURION_OPT_SUBTAB=N` and `CENTURION_OPT_LOAD=<preset>` pick the sub-tab and preset
for `CENTURION_SCREENSHOT`.

### Added knobs (sysmap review)

- **CPU:** cpuidle governor (menu / teo …, from `available_governors`).
- **Memory:** multi-size THP per size (16 KB – 1 MB) and khugepaged
  (`max_ptes_none`, `pages_to_scan`, scan / alloc sleep).
- **Scheduler:** sched_ext scheduler (`scx_lavd`, `scx_bpfland` …) — started
  detached by tune-helper, stopped on restore / game-mode release. Only
  allowlisted `scx_*` binaries, root-owned in system directories, are run.
- **Network** (new group): TCP congestion control (bbr offered when the module
  exists), default qdisc (only qdiscs the running kernel has), Wi-Fi power save
  (through `iw`; NetworkManager may re-enable it on reconnect).
- **Devices:** PCIe ASPM per link (`l1` / `l1ss`), for links a driver or the
  firmware left without ASPM; links that refuse are skipped.

Like every other knob these record the original value on first write and go
back on *Restore originals*, game-mode release or a scene switch.

### Deep-optimization guide

Every row's tooltip already explains what it does, when to touch it, and what
to enter — this section is for combining rows toward a goal, the reasoning a
single tooltip can't carry. All of it applies through **checking rows and
Apply checked**, or by loading/editing one of the six built-in presets
(Preset dropdown → *Load*) and saving your own variant (*Save as…*).

**Chasing 1% lows / frame-time spikes in a specific game.** Start from
*Gaming X3D* and change one thing at a time — this is empirical, not a
formula:
1. `cpu.x3d_mode` = cache, launch affinity (Game launch tab) = the V-Cache
   CCD. This alone is usually the single biggest win on an X3D chip for
   cache-hungry games (open-world, simulation-heavy, emulators).
2. `wq.cpumask` and `irq.affinity` = the *other* CCD, so filesystem/network
   work and interrupts physically cannot land on the game's cores.
3. Try `cpu.smt` = off. Some titles' 1% lows improve with SMT off (no sibling
   cache contention), most don't care — this is genuinely per-game, test it.
4. If still spiky, `cpu.cstate_max` to a shallow state (e.g. `1`) trims
   wake-from-idle jitter at the cost of idle power/heat while gaming.
5. Last resort, not first: `cpu.ccd_park` the non-gaming CCD (*Competitive*
   preset). Total isolation, but you lose those cores entirely until restored
   — only worth it if steps 1–4 didn't get you there.

**A game crashes or hangs specifically under Wine/Proton, not on native
titles.** Check `kernel.split_lock_mitigate` = 0 first (some titles trigger
the split-lock mitigation and stall ~1000× on affected instructions) and
`vm.max_map_count` = 2147483642 (some Proton titles map more regions than
old distro defaults allow and crash on it). Both are already in every gaming
preset; if you built a preset from scratch, these two are the "don't forget"
rows for Proton compatibility specifically.

**Validating a Ryzen Curve Optimizer offset (Ryzen tab) before trusting it.**
Load the *CO validation* preset here first: it keeps boost on and every
C-state enabled (idle→boost transitions are where a marginal core first
misbehaves — you want that path exercised, not avoided), re-enables the
kernel watchdog, and drops the MCE poll interval to 10s. Apply it, then apply
your CO offsets in the Ryzen tab, then stress the affected cores while
watching `dmesg -w` (or rasdaemon if installed) for correctable-error or
watchdog messages. Restore this preset once you're done — it's deliberately
not a tab to leave applied permanently.

**Compiling / long parallel builds (kernel, emerge -j).** *Compile
throughput* trades every latency-favouring row for throughput: EPP
`balance_performance` rather than `performance` (better sustained clocks
under sustained load), `cpu.x3d_mode` = frequency (compilers are usually more
clock-sensitive than cache-hungry), SMT on, no CCD parked, wider scheduler
slices (`sched.preempt` = voluntary, `sched.base_slice_ns` larger), and I/O
scheduler back to `mq-deadline` for fair multi-process disk access instead of
`none`'s raw single-queue latency.

**Battery life on the go.** *Quiet battery*: EPP `power`, boost off, min
frequency at hardware floor rather than the efficient-but-higher
`lowest_nonlinear`, `cpu.cstate_max` = all (never restrict idle depth on
battery), aggressive ASPM (`powersupersave`), HDA and USB power-saving
re-enabled. This preset is the one place a lot of the "always safe" rows from
other presets get deliberately reversed — that's intentional, they trade
latency for power savings which is exactly backwards for gaming but right
here.

**Building your own preset from scratch.** A reasonable order: (1) start
from the closest built-in preset and *Load* it, (2) uncheck what you don't
want, (3) use *Select → Changed* to see at a glance which rows currently
differ from the live system, (4) *Apply checked* and actually use the system
for a while before deciding it's right, (5) *Save as…*. Rows marked ⚠ are the
ones most likely to cost you something (stability, heat, idle power) if
misapplied — read those tooltips before checking them, and prefer testing
them individually rather than as part of a first-time bulk apply.

**Per-CCD splits in general (X3D 2-CCD chips only).** The pattern behind
`Governor · CCDn` / `EPP · CCDn` / `Boost · CCDn` / `Max frequency · CCDn`:
treat the two CCDs as two different machines with two different jobs. The
game's CCD gets the aggressive settings (performance EPP, boost on, no
frequency cap); the other CCD gets efficiency settings (balance_power EPP,
maybe boost off, maybe a frequency cap) since it's mostly idle plus whatever
background work you steered onto it with `wq.cpumask`/`irq.affinity`. None of
these four rows do anything by themselves — they only matter paired with the
launch affinity (Game launch tab) that actually puts the game process on the
CCD you're optimizing for.

### NVIDIA curve: zoom
Zoom slider (1–12×, voltage axis) and pan slider under the graph; the
frequency axis auto-fits to the visible points. Over the graph: wheel zooms at
the cursor, Shift+wheel pans, `+`/`−` zoom, `0` fits. A strip at the top of the
plot shows where the window sits on the whole curve. Dev aid:
`CENTURION_NVCURVE_VIEW=zoom,pan` with `CENTURION_NVCURVE_FAKE`.

### systemd
Both init flavours are installed; the running init picks its own files.

    systemctl enable --now nvcurve-autoload.service   # ★ Default GPU profile
    systemctl enable --now centurion-tune.service           # ⏻ Apply at boot preset
    journalctl -u centurion-tune                            # helper's JSON result

`centurion-tune.service` runs after `systemd-sysctl` (so its values win over
`/etc/sysctl.d`) and module loading, before the display manager; `systemctl
stop centurion-tune` restores the originals. `nvcurve-autoload.service` retries for
a while if the NVIDIA device is not up yet. Both are skipped by a
`ConditionPathExists` when no default profile / boot preset is set.
Service files hold `@BINDIR@`/`@LIBEXEC@`; install.sh and the ebuild fill them
in (`PREFIX`, `UNITDIR` are honoured).

## Intel CPU support

The CPU vendor is read from `/proc/cpuinfo` (`vendor_id`) by both the GUI and
the helpers (`tune::cpu_vendor()`).

* **Ryzen Curve Optimizer tab** and the tray's *CPU Curve (Ryzen)* menu are only
  created on AMD. On Intel, *Undervolt CPU* in Game launch is disabled and
  `centurion-gamemode` skips the CPU step (an Intel undervolt tool is planned).
* Every tunable carries a vendor (`amd()` / `intel()` in `tune.rs`). Rows of the
  other vendor are left out of `describe` and reported as *not available* on
  apply, so preset files stay portable.
* Intel rows (Optimizations → CPU / Devices): `intel_pstate` mode, HWP dynamic
  boost, max/min perf %, EPB, EPP · P-cores / E-cores, max frequency · P/E,
  uncore min/max (`intel_uncore_frequency`), RAPL PL1/PL2 (MSR + MMIO, watts),
  TCC offset (`intel_tcc_cooling`), iGPU min/max MHz and SLPC profile (i915/xe).
  *Core performance boost* maps to `intel_pstate/no_turbo` (inverted).
* Hybrid topology (`/sys/devices/cpu_core|cpu_atom/cpus`) adds the `pcore` /
  `ecore` roles to launch affinity, unbound workqueue CPUs, IRQ affinity and
  parking (E-cores only; cpu0 is a P-core).
* Built-in presets are vendor-specific: *Intel gaming hybrid*, *Intel
  competitive*, *Intel low latency desktop*, *Intel compile throughput*,
  *Intel quiet battery*.
* Dev aid: `CENTURION_CPU_VENDOR=amd|intel` forces the GUI's vendor.


## October 2026 additions

**Scenes — display and keyboard extras.** New scene keys: `panel_od` (bool; legion-gpu-helper `set_panel_od`), `kbd_backlight` (0..max; legion-profile-helper `kbd_backlight`), `zone4` (the 4-zone Lighting tab request: effect, brightness, speed, right_to_left, colors; lighting-helper `z4_set`; *Capture* reads `~/.config/centurion/zone4.ini`), `display: {refresh: Hz}`. Applied after keyboard lighting, before the run command, the same way by the GUI engine and `centurion-gamemode`. Rows appear only where the hardware exists: Over Drive support is cached per user in `~/.cache/centurion/panel.json` when Home probes it (the probe is root-only), the white-backlight row needs the ideapad LED on a non-RGB keyboard, the refresh row needs `centurion-display list` to report more than one rate.

**centurion-display** (`crates/centurion-helpers/src/display.rs`, runs as the user, never via pkexec): KDE `kscreen-doctor -j` (Plasma, Wayland and X11), `wlr-randr --json` (wlroots, not GNOME) or `xrandr --query` (X11). Only the enabled internal panel (eDP / LVDS / DSI) and only its refresh rate: the closest offered rate at the current resolution. Every tool call has a hard time limit.

**Heroic / Bottles detection** (`gui/src/launcherwatch.cpp`, tune.json `detect_launchers`). Off = no timer. On: a process-name sweep of the user's processes every 15 s for `heroic` / `bottles`; only while one runs (or a detected game is alive) environments are read every 5 s. Heroic games carry `HEROIC_APP_NAME` (Heroic's `setupWrapperEnvVars`); Bottles games are `*.exe` processes whose `WINEPREFIX` is under a bottles directory, minus Wine's service programs and installers. Two sweeps to start, two to end. Start / end run `centurion-gamemode PRE` / `POST` detached with `CENTURION_GAME` and `CENTURION_OWNER_PID` (= the GUI; gamemode accepts it only for a live process of the same user and tune-helper re-checks the uid, so a session left by a crashed GUI is pruned). `centurion-gamemode RUN` / `WRAP` export `CENTURION_WRAPPED=1`, which the watcher skips.

**Battery use** (`gui/src/batterypage.cpp`, `~/.local/state/centurion/battery.json`): one status read per minute; on battery, energy_now differences are charged to the active scene; intervals over 10 min or where wall time ran ahead of monotonic time (a suspend) are dropped. One capacity sample per day (energy_full, design, cycle_count). Home's battery row shows draw and time left / to full (`sysinfo::batteryState`).

**Health event kinds** (`crates/centurion-helpers/src/health.rs::classify`, `gui/src/healthtab.cpp`): besides `xid`, `gsp`, `mce`, `aer`, `lockup`, `d3cold`, `amdgpu` the classifier emits `nvme` (I/O timeout / controller reset = error, controller down = critical), `fs` (EXT4-fs error, BTRFS error / csum failed, XFS corruption; critical), `igpu` (i915 GPU HANG, xe Timedout job / trying reset; error), `wifi` (iwlwifi microcode error, mt79xx message timeout, ath firmware crash; warn), `thermal` (critical temperature reached = critical, package temperature above threshold = warn), `oom` (warn) and `oops` (`Oops:` / `BUG:` / `WARNING: CPU:` at the start of the message; error; the title names the trailing `[module]` when present). `HealthTab::drainKmsg` pre-filters raw `/dev/kmsg` records with a `KEYS` list that must be extended with every new pattern, otherwise the line never starts a scan; keys anchored with `;` match the start of the message. `stability.rs::event_weight` gives all of the new kinds weight 0 and `curvepage.cpp` ignores kinds it does not know, so none of them is charged to a curve.

**Boot history and pstore** (`crates/centurion-helpers/src/boothist.rs`, `pstore.rs`, `gui/src/bootpage.cpp`): `health-reader {"op":"boots"}` (read-only, same silent polkit action as the scan) returns `boothist::build` over `exposure.json`, `oc-journal.jsonl` and `pstore::list`. A boot is `clean` (closed by centurion-boot-guard), `unclean` (`on_boot` found it open), `running` (current) or `unknown` (left open, the guard did not run). `curves` is the ordered, de-duplicated state list per journal kind; an empty object means stock or older than the journal's rotation. A pstore record is attributed to the unclean boot whose lifetime (start to the next boot's start) contains its time, else the nearest within 24 h, else left unattributed. `previous_unclean` (the boot before the current one, when it is unclean) feeds the band in `HealthTab`; the GUI reads that once ~4 s after start and when the page is shown, never periodically. The last 30 kernel-log lines of an unclean boot are taken by the GUI from its own `kernel.log` (`boot=<first 8 of boot_id> seq=` tag); the helper does not know the user's home. pstore sources: `/sys/fs/pstore`, `/var/lib/systemd/pstore` (up to 3 directory levels), and the archive; files are opened `O_NOFOLLOW`, must be regular, are read up to 1 MiB, clipped to the last 12 KiB and passed through `health::scrub`. `tune-helper {"op":"pstore_clear"}` takes nothing from the request: for every file in `/sys/fs/pstore` it writes the scrubbed text to `/var/lib/centurion/pstore/<crash time>-<name>.txt` (`write_root_file`, 0644) and only then `unlink`s the pstore file (the kernel frees the EFI variable; EFI variables are never touched directly), one `wlog` line per removal; archive files beyond the newest 30 are pruned. The GUI confirms first; the op is not in `PROFILE_OPS`.

**Ryzen limit reasons** (`crates/centurion-helpers/src/pmtable.rs`): `ryzen-co-helper monitor_stream` with `"limits": true` adds the PM table head from `/sys/kernel/ryzen_smu_drv/pm_table`. Desktop-derived tables (version family 0x24 / 0x38 / 0x54 / 0x62): PPT, TDC, THM, EDC at f32 indices 0/1, 2/3, 4/5, 8/9. Mobile APUs (0x37 / 0x40 / 0x45 / 0x4C / 0x5D / 0x65): STAPM, PPT fast / slow, TDC, EDC, THM at 0/1, 2/3, 4/5, 8/9, 12/13, 16/17. Every limit and value must be physically plausible, otherwise nothing is shown ("layout not recognised" + version). ≥ 95 % of a limit counts as binding; the tab sums seconds per limit since the monitor started. Read-only.

**Curve journal and stability** (`ocjournal.rs` in both crates, `gui/src/curvepage.cpp`): ryzen-co-helper, intel-uv-helper, nvcurve-root-helper and `nvcurve autoload` append `{"t","boot","kind","state"}` to `/var/lib/centurion/oc-journal.jsonl` (0644, rotated at 256 KiB) after every successful curve write. Health → Curve stability builds per-boot timelines (boot start / end from `exposure.json`, stock from each boot's start) and attributes the saved Xid / GSP / D3cold (GPU) and MCE / lockup (CPU) lines of `health.log` to the state in effect; Xid 45 / 154 are follow-ups and not counted. Guard: a new Xid 13/31/43/61/62/69/109/119/120 with a non-stock GPU curve raises a notification and — opt-in — runs `reset_gpu_curve` (never after Xid 79, never while the dGPU is unusable); an MCE / lockup with a non-stock CPU curve does the same with the CPU helper's `reset`. At most one reset per minute.

**Trial apply** (`gui/src/trialbar.cpp`; gui.ini `nvidia/trial`, `ryzen/trial`, `intel/trial`): 30 s after a successful apply the tab's own reset runs unless *Keep* is pressed; a reset or a new apply ends the trial.

**Monitor repeats and *Mark all seen*** (`gui/src/healthtab.cpp`; gui.ini `health/seen_ms`): an event with the same boot, kind, code and raw title as the newest row, within 300 s of kernel time, updates that row (`×N`) instead of adding one; `groups_` keeps the newest row per key and forgets it when the 500-row cap drops it. The counters only count events whose wall-clock time (kernel µs + boot time) is after `seen_ms`. The Overview gets the same boundary as `kernel_seen` (seconds) and drops the older `health.log` lines before counting (`overview::unseen_log`), so the kernel card and the Home badge agree with the Monitor.

**Lid watch** (`gui/src/lidwatch.cpp`; gui.ini `sleep/bag_warn_min`, default 10, 0 = off): UPower's `LidIsClosed` on `org.freedesktop.UPower` (system bus, `PropertiesChanged` plus one `Get` at start-up). Closing starts one single-shot timer; opening stops it. When it fires, a `/sys/class/drm` connector other than eDP / LVDS / DSI reading `connected` cancels the warning; otherwise the CPU temperature (k10temp / coretemp `temp1_input`) and the fastest `fan*_input` go into one tray notification. Nothing is polled and no helper runs.

**Cooling fan checks** (`crates/centurion-helpers/src/cooling.rs`, `centurion-cooltest`): each run stores `fans` (mean rpm per fan over the steady window) and `fan_spread` = (max − min) / max in percent; `assess` turns a rise of 15 points or more over the baseline's spread into a `fan_split` watch reason, and runs without `fan_spread` (older records) are not judged for it. The fan max test is driven by the GUI, not the tool: `CoolingPage` saves the Home tab's fan targets and mode through `FanControls` (wired in `mainwindow.cpp` to `HomeTab::fanInfo / setAllFansMax / setAllFansAuto / setFanTarget`), reads `centurion-cooltest fans` every 2 s for 35 s (5 s before, 30 s at maximum), then restores the fans (all Auto, then the fixed targets; waits for pending Home writes) and calls `centurion-cooltest fanmax --peaks …`. The first clean fan test under a key is stored in `fanmax.json` (`{key: {"t", "peaks"}}`); later peaks are compared by `check_fan_peaks` (warn at 8 % below baseline per fan). `forget` removes both the runs and that key.

**Drift detector** (`gui/src/driftwatch.cpp`): reference snapshot ~20 s after a scene finished or a game scene changed, and whenever tune-helper's state (source, preset, changed keys) differs from the one the reference was taken for; checks 30 s after a power-source change and every 10 min (`tune-helper describe` light, as the user, + the active scene's firmware attributes + the platform profile). Compared in the same read-back form, so representation differences never count. Skipped while a game session runs or a calibration holds the machine. Culprits listed: running power-profiles-daemon, tuned, auto-cpufreq, thermald, system76-power, ananicy-cpp, laptop_mode, powertop, cpupower, gamemoded, and TLP when its service is enabled.

**Sleep** (`packaging/sleep/centurion-sleep-log`, `gui/src/sleeppage.cpp`, `wakeup.rs`): the system-sleep hook (elogind `40-centurion-sleep-log`, systemd `system-sleep/centurion-sleep-log`) keeps a pre-sleep snapshot in `/run/centurion/sleep-pre` and appends one JSON line per sleep to `/var/log/centurion/sleep.log` (start, end, mode, energy before / after, capacity, status, dGPU runtime_status before / after, `last_hw_sleep`, wake IRQ, wakeup sources whose count rose). Sysfs reads only; never blocks a suspend. Wake sources: tune-helper `wakeup_set` {device, enabled} toggles `/proc/acpi/wakeup` (read before, read back after) and records the choice in `/etc/centurion/wakeup.json`; `tune-helper boot` (centurion-tune) restores it at every boot.

**Automatic backups** (`gui/src/backuppage.cpp`): checked 3 min after start and every 6 h. When the newest configuration archive in the folder is older than *auto_days* (default 7), `backup-helper config_backup` runs as the user with tag `auto`; only `…-auto.tar.gz` beyond *auto_keep* are pruned. System images need root, so only a reminder (at most once per 20 h) when the newest `backup-*` image is older than *remind_days* (default 30, 0 = off).

**Fan curve assistant** (`gui/src/fanassist.cpp`): CPU limit (the preset's `cpu_min`–`cpu_max`, within the firmware's `cpu_temp` range) and GPU limit (`gpu_temp` range), written through fwattr-helper in one batch, snapped onto the firmware's step grid, only when changed and only in the Custom profile; without `cpu_temp` the curve assumes 100 °C. Presets built into the program, or from `~/.config/centurion/fan-presets.json` when the user creates one (`{"presets": [{"name", "level", "cpu_min", "cpu_max", "ramp_from"}]}`, or `"curve": [10 levels]` in place of level / ramp_from, used as it is; built-in: Quiet / Balanced / Performance / Max, each with its own `curve`, CPU 85–89 / 92 / 95 / 100 °C; a file that does not parse or validate — level 1–10, 0 < cpu_min ≤ cpu_max, 0 < ramp_from < cpu_min, or a curve of 10 levels 1–10 that never goes down with its last three ≥ 3 / 5 / 7 — is reported and the built-in presets used): shaped on the EC temperature steps of the fan whose thresholds reach the hottest (the CPU's; sensor ids differ per model), the step in use at the limit (last threshold ≤ limit) gets the holding level, steps below ramp to it from level 1 at `ramp_from`, steps above hold it up to limit + 8 °C, then 10 (three hottest steps ≥ 3 / 5 / 7, as the helper requires). The curve only goes into the editor.

**Kernel log view** (`gui/src/klogview.cpp`, `klogpage.cpp`): the saved lines (`persist`'s format) are parsed by `klogview::parse`; `normalize` replaces every digit run with `#` (the repeat key), `group` collapses rows per key, and `firstBoots` maps each key to the boot tag of its first occurrence in the whole saved log, so *Only new* is judged over all boots whatever the scope. Muted keys are `normalize(msg)` in `gui.ini` `klog/muted`. The right-click menu finds the key of a row by its rendered line text (`rowKey_`). Test: `gui/tests/klogview_test.cpp`.

**Notifications** (`gui/src/notifyprefs.cpp`, `notifydialog.cpp`, `tray.cpp::gatedNotify`): `HealthTab::alert(category, severity, title, message)` carries the notifyprefs key (`kernel`, `network`, `backup`, `curve`, `smart`, `heat`) and, for kernel alerts, the worst severity (`error` / `critical`). `notifyprefs::decide` is the whole gate (Show / Hide / Defer); the Tray holds deferred lines while `scenes::gameSessions() > 0` (`/run/centurion/tune/state.json`, written by the game hooks) and sends one summary from `flushDeferred()` when a game ends (`LauncherWatch::gameEnded`, or `SceneEngine::activeSceneChanged` with a non-game scene). Settings in `gui.ini` `notify/off`, `notify/critical_only`, `notify/defer_game`. Test: `gui/tests/notifyprefs_test.cpp`.

**Charger history** (`gui/src/batterypage.cpp`, `batteryadvice.cpp`): on every tick the page adds the seconds on the charger to `ac_days[today]` (and those at capacity ≥ 98 % to `full`), using the same monotonic-gap rule as the scene statistics, so a suspend ends the stretch. A *drain stretch* (`ac_drain`) is open while the charger is plugged in and `status` is `Discharging`; it is saved when it ends, if it lasted a minute or more, with its energy taken from `energy_now` differences. `batteryadvice::evaluate` decides the Long_Life advice from the last 30 days; `parseChargeTypes` reads the bracketed mode from the battery's `charge_types`. The button calls `legion-profile-helper` `charge_type` through `privileged::run`, as Home does. Test: `gui/tests/batteryadvice_test.cpp`.

## Network: traffic ledger, Wi-Fi, blocklist subscriptions

- **Traffic** (`crates/centurion-helpers/src/traffic.rs`, `netguard.rs` `acct_*`): with `traffic: true`
  in `/etc/centurion/netguard.json` the daemon sets `net.netfilter.nf_conntrack_acct = 1` (logged in
  `writes.log`), opens a NETLINK_NETFILTER socket on groups NEW (1) and DESTROY (3), and adds an `ct state
  invalid counter` rule (no verdict) so conntrack tracks local flows. At NEW the owner is looked up while the
  socket exists (sock_diag, then `/proc/<pid>/fd` in batches); at DESTROY the counters are added to that
  program's day in the ledger. Forwarded flows (no local socket) are ignored. Totals are written at most every
  10 s (`TRAFFIC_FLUSH`) and on stop. File: 0640 root:wheel, 90 days, 256 programs per day (rest under
  `(other)`). Connections the daemon did not see open are not counted.
- **Wi-Fi** (`wifi.rs`): `iw dev <if> link` and `iw dev <if> station dump`, unprivileged, through the
  user op `netguard-helper {"op":"wifi"}`. The first `/sys/class/net/*` with `phy80211` or `wireless`, one whose `operstate` is `up` first.
  The tab also counts this boot's `WIFI` lines in `health.log` (tail only, reread when the file changes). Warnings:
  signal below −75 dBm; retries above 10 % or failures above 2 % of transmitted packets (since association).
- **Blocklist subscriptions** (`gui/src/blocklistsubs.*`, `netpage.cpp` `updateSubscription`): stored in
  `gui.ini` as `netguard_subs/N/*`. The GUI downloads over https (30 s timeout, 8 MiB cap), parses
  (`parseBlocklist`: at most 100 000 tokens), and sends one `blacklist` request: `add` = new list, `label` = the subscription name,
  `replace: true` (the helper drops the group's old entries before adding, so an entry stored in another form —
  a range split into CIDR blocks, `1.2.3.5/24` stored as `1.2.3.0/24` — is not removed again). The helper validates every entry
  and saves once, so a refused request changes nothing. The blacklist cap is `MAX_BLACKLIST = 100 000`; the
  helper request limit is 4 MiB and the config read limit 8 MiB.

## Health analysis ops (read-only)

The Health pages use five read-only ops. They run as the user through `tune-profile-helper` (no pkexec) from `gui/src/userhelper.cpp` (QProcess, 15 s timeout, the last stdout line is the reply). They write nothing to the system and never query the dGPU through NVML or `nvidia-smi`.

- `{"op":"overview","battery":bool,"dgpu":bool,"backup_dirs":[abs dirs]}` → `{"ok":true,"cards":[{"id","level",…}],"worst":n}`. Analysis in `crates/centurion-helpers/src/overview.rs`. Levels: −1 no data, 0 ok, 1 watch, 2 attention. Card ids: `kernel`, `unclean`, `battery` (with a battery), `cooling`, `backup`, `dgpu` (NVIDIA only), `sleep`. There is no disk card yet (SMART is not part of this op).
- `{"op":"dgpu_usage"}` → `{"ok":true,"today":{…},"wakes":[…],"power":{…}}`: the analysis of `$XDG_STATE_HOME/centurion/dgpu.json` (default `~/.local/state/centurion/`). The GUI writes that file (`gui/src/dgpuusage.cpp`), not the helper: one sample a minute from the Battery use tick, plus one row per wake-up with the holders found at that moment. Format, version 1: `days[YYYY-MM-DD][bat|ac]{awake_ms,asleep_ms}`, `power{awake|asleep}{sec,wh}`, `wakes[{t,holders}]`. Battery intervals count only when the energy difference is plausible (0 to 50 Wh) and no suspend lies in between.
- `{"op":"battery_trend"}` → `{"ok":true,"status","samples","health","months_to_80"}`: the wear forecast for Battery use. Least squares of health (full ÷ design, %) over the daily `capacity` samples of `battery.json` from the last 180 days. `status`: `insufficient` (under 30 samples; no forecast), `no_decline` (flat or rising), `at_or_below` (already 80 % or less), `ok` (with `months_to_80`). Analysis in `crates/centurion-helpers/src/trends.rs`.
- `{"op":"event_days"}` → `{"ok":true,"days":[{"d":"YYYY-MM-DD","kinds":{kind:n}}]}`: kernel events per local day and kind for the last 30 days, oldest first (Monitor's chart). Counted from the last 2 MiB of `health.log.1` and `health.log`, so older lines of a long log are not counted. Analysis in `trends.rs`.
- `{"op":"diag_report","version":…,"dgpu":bool}` → a preview `{"ok","files":[{name,status,bytes,masked,text}],"masked"}` (64 KiB of text per file). Add `"out":"/abs/x.tar.gz"` to write the archive instead: mode 0600, written to `.partial` and renamed, reply `{"ok","file","bytes","included"}`. Collection and scrubbing (serial/UUID → `<hidden>`, MAC → `<mac>`, IPv4 → `<ip>`, kernel addresses → `<addr>`) live in `crates/centurion-helpers/src/diagreport.rs`; the list of files is in HANDBOOK §9.

The GUI pieces are `overviewpage.cpp` (cards, refresh, report button), `reportpreview.cpp` (the preview dialog), `dgpupage.cpp` (usage and wake-up rows) and `batterypage.cpp` (the sample call).
