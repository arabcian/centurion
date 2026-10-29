# Centurion — Handbook

A short, task-by-task guide. For what each feature is, see the
[README](../README.md); for internals, [TECHNICAL.md](TECHNICAL.md) and
[AUTOTUNE.md](AUTOTUNE.md). Hover any setting in the GUI — every row has a tooltip.

**Golden rules**

1. Change one thing at a time, test, then save it as a profile.
2. Everything has an undo: *Restore originals*, *Reset*, *Factory reset*, *↺ Return to boot-guard defaults*.
3. Firmware-level changes (BIOS memory timings, BIOS CPU OC, GPU MUX) ask for the admin password — read the warning first.

---

## 1. First ten minutes

1. Install ([README → Install](../README.md#install)), log out and in once.
2. Start the app from the menu. It sits in the **tray**; closing the window hides it, *Quit* in the tray menu exits.
3. The first time the window opens, the **setup wizard** walks through the rest; every step can be skipped, and Home → *Setup wizard…* opens it again:
   - *Your machine* — model, support status, the interfaces found.
   - *Boot services* — tick the ones you want (`centurion-boot-guard` first — it protects the rest) and run the command it shows (root, so the wizard does not run it itself):
     ```sh
     rc-update add centurion-boot-guard boot        # OpenRC
     systemctl enable centurion-boot-guard          # systemd
     ```
   - *First backup* — the Centurion configuration (same as Health → Backup → *Back up now*).
   - *First scene* — *Capture current* saves the power profile and firmware limits as "Default", a restore point for the whole machine; optionally pick scenes for AC and battery. (The fan curve is captured from Scenes → *Capture current*.)
   - *Extras* — a shortcut for that scene, the MangoHud line.
4. Open **Home**, pick a power profile, look at the *Live* box to see sensors work.

## 2. The window and the tray

| Where | What |
|---|---|
| Tabs | Home · Scenes · Firmware · Ryzen / Intel · NVIDIA · AMD GPU · Optimizations · Lighting · Health (only those your machine supports) |
| Tray icon | Click: show / hide the window. Right-click menu: Scene, Power Profile, CPU Curve, GPU Curve, Optimizations, Keyboard Lighting, Fans, Show/Hide, Quit |
| Second launch | Just shows the running window |

Use the tray for daily switching; use the tabs when you tune.

## 3. Home

- **Power profile** — click a card. Applies instantly. *Custom* unlocks firmware limits and the fan curve.
- **Live box** — temps, fans, power, battery (with draw and time left / to full). Updates only while the Home tab is visible (saves power). The dGPU row shows *asleep* or its temperature without waking it.
- **Device box** — battery charge mode, GPU mode (Hybrid / dGPU only, takes effect next boot), iGPU-only mode, Panel Overdrive, Fn lock, camera, USB charging when off / on battery, keyboard backlight level (white keyboards), Instant Boot, Custom in Fn+Q, Flip to start, Super-key lock, touchpad lock. A switch your firmware does not report is simply not shown.
  - **Device info…** (Hardware box) lists what the firmware reports: series, BIOS, interfaces, capability list. Use *Copy report* when filing a bug for another model.
  - **iGPU-only mode** unloads the NVIDIA driver. If the dGPU does not return, press **Bring NVIDIA back**.
- **Fans** — per fan: *Auto*, *Max*, or set a value and press *Set*. **Turbo fan** = EC full speed. **Fan curve…** edits the table used in Custom mode (AC power). **Assistant…** sets the firmware's CPU limit (85–100 °C, `cpu_temp`) and GPU limit (`gpu_temp`, the firmware's own range) — the chip throttles there instead of running hotter — and proposes a table for a noise preset, shaped on the firmware's temperature steps. Presets are built into the program; to use your own, copy `fan-presets.json.example` from the program's root folder to `~/.config/centurion/fan-presets.json`, edit it and reopen the assistant: each has a `name`, the fan `level` at the CPU limit (1–10), the CPU limits it offers (`cpu_min`–`cpu_max`) and `ramp_from` (a temperature step, 0–9 as in the editor; steps up to it stay at level 1) — or, instead of `level` / `ramp_from`, its own `curve`: 10 levels, one per temperature step as in the editor (never going down, last three at least 3 / 5 / 7). Stock presets: Quiet / Balanced / Performance / Max, each with its own `curve`, CPU 85–89 / 92 / 95 / 100 °C. An invalid file (including an old `ramp_from` in °C) is reported and the stock presets are used. The limits are set when the proposal goes into the editor (Custom profile) — press *Apply* there to keep the curve.
- **Memory timings** — *Reload* reads live timings; *Edit timings…* → *Write to BIOS* (admin password; applies next boot; *Restore previous…* undoes it). Wrong timings can stop the laptop from booting — change small steps only.
- **Boot guard box** — appears when a crashed boot paused presets. Fix the cause (usually an undervolt that is too deep), then *Resume boot presets* / *Resume login scene*.

## 4. Scenes

A scene is a **name for a whole setup**. It stores power profile, firmware limits, CPU curve, NVIDIA curve, Optimizations preset, lighting (Spectrum profile / brightness, or the 4-zone keyboard's effect and colours via *Capture*), white keyboard light level, panel Over Drive, the internal panel's **refresh rate**, fan boost, Custom-mode fan curve and an optional command. Each part can stay *Unchanged*; rows only appear where the machine has the thing behind them. The refresh rate is set as you through the desktop's own tool (KDE `kscreen-doctor`, `wlr-randr`, or `xrandr` on X11) at the current resolution — e.g. 60 Hz in the battery scene.

**Make one:** Scenes → *New…* → *Capture current* (copies firmware limits and the fan table) → pick profiles per component → *Save*. *Apply* tests it.

**Automatic switching:** tick *Switch scenes with the power source*, choose a scene for AC and for battery. The tray has *Pause scenes* — while paused nothing is written automatically, even on plug / unplug.

**Game start:** Optimizations → Game launch → *System at game start*: the first game switches to the chosen scene, the last exit switches back.

**Shortcut:** click the *Shortcut* field of a scene, press a key combination (e.g. Ctrl+Alt+1) and *Save*. The key is registered with the desktop's global-shortcuts portal (KDE Plasma 5.27+, GNOME 48+, Hyprland); the desktop may ask you to confirm it and can change it later in its own shortcut settings — the line under the field shows the key it actually uses. Where the portal offers it (interface version 2), *Change on desktop…* next to the field opens that dialog directly. Without the portal, bind `centurion-gamemode SCENE "Name"` in the desktop's shortcut settings instead. A copied scene starts without a shortcut.

**In games (MangoHud):** tick *Show the active scene and power profile in games* under Automatic switching. It adds a marked block to `~/.config/MangoHud/MangoHud.conf` (`custom_text=Centurion` + `exec=cat $XDG_RUNTIME_DIR/centurion-overlay`, a file Centurion keeps current while it runs — empty when it is not running), shown from the next game start. Blocks added by older versions run `centurion-gamemode OVERLAY` instead and keep working; untick and tick again for the new form. MangoHud runs `exec` only with `legacy_layout=0`, which draws only the listed elements, so when your file does not use that layout the block also lists MangoHud's defaults (GPU, CPU, FPS, frame time). Unticking removes the block and leaves the file as it was.

**Scripts:** `centurion-gamemode SCENE "Name"`, or the D-Bus API (section 10). Export / import scenes from the Scenes tab.

## 5. Firmware tab

Needs **Custom** profile for most limits.

- **CPU PL1 / PL2 / PL3** — sustained / short boost / peak power. Raise in small steps; watch temperature in Home.
- **Temperature target** — where the CPU starts to throttle.
- **GPU cTGP / Dynamic Boost** — GPU power and the boost ceiling / floor. Written over WMI (needs `acpi_call`).
- **CPU overclocking (firmware, next boot)** — PBO scalar, boost override, all-core CO stored in the BIOS. Asks for the password.

## 6. Undervolting

### NVIDIA GPU (NVIDIA tab)
1. *Read Curve*.
2. Quick way: set the **Core offset** (negative = undervolt) → *Apply Offsets*. Detailed way: drag or select points on the graph (*Fit* shows all, *Reset Graph* drops unapplied edits).
3. Stress test a game. Stable → *Save As…*; add ★ **Default** to apply at boot (needs `nvcurve-autoload`).
4. Other controls: **Lock / Unlock** the VRAM clock, **Cap / Uncap** the GPU core clock, **Driver options…** (NVIDIA module parameters). *Reset Curve* / *Reset All* go back to stock.

### Ryzen CPU (Ryzen tab)
- **All-core:** enter an offset (negative = undervolt) → *Apply All-Core*.
- **Per-core:** enter offsets per core (CPPC-ranked, one column per CCD) → *Apply Per-Core*. Each CCD's *Fill* sets every enabled core of that CCD to one value; *Clear* empties the fields.
- *Save* a profile; the tray *CPU Curve* menu applies it. *Reset Curve Optimizer* clears all offsets.
- Crash or reboot after applying → lower the offsets (e.g. −5 steps).

### Intel CPU (Intel Undervolt tab)
*Read current* → tick the values to write → *Apply*. If the readback differs, the BIOS locks undervolting (*Test UV lock*). *⏻ Apply at boot && resume* stores it; separate AC / battery profiles; *Import ThrottleStop.ini…*.

### AMD GPU (AMD GPU tab)
Change values → *Apply*. It is a **trial**: unless you press **Keep** within 20 s, the old settings return. *Undervolt step −10 mV* repeats until a game crashes; keep the last step that held. Nothing is applied at boot, so a hang is fixed by rebooting. Needs `amdgpu.ppfeaturemask` bit `0x4000` (the tab shows the exact value).

## 7. Optimizations

### 7.1 Presets
Top bar: choose a preset → **Load** → review the checked rows → **Apply checked**.
Loading a preset (or an autotune result) checks **every** row: the preset's own values, and every row it leaves out at the
value `centurion-boot-guard` captured at boot. Applying a preset therefore also puts back whatever an earlier preset or a
manual change left behind, and a preset you save holds the whole state. (Rows the snapshot does not cover take their live
value while Centurion has not changed them; the status line says how many rows came from where.)
**Save as…** keeps your own preset. **Restore originals** undoes everything applied this session.
**↺ Return to boot-guard defaults** pauses Scenes and rewrites all knobs to the values captured at boot — use it as a clean baseline.

Rows are grouped: **CPU · Memory · Scheduler · Storage · Network · Devices · Power · Stability**. The number on each sub-tab is how many rows your machine supports.

### 7.2 Autotune (let the program choose)
1. Optional but recommended: enable `centurion-boot-guard` and reboot once (it records the boot defaults).
2. Press **⚙ Autotune** → pick a goal: *power saving*, *gaming*, *throughput*, *desktop*.
3. Review the filled rows (every row is checked: autotune's values, the rest at their boot-guard defaults; changed rows are bold). **Weights…** shifts priorities (latency, throughput, power, footprint, stability).
4. **Apply checked**. A 120 s guard rolls memory settings back if the system stalls.

### 7.3 Calibration (teach the program your machine)
Optional, improves Autotune. 15 minutes, do not use the PC meanwhile.
```sh
# Pause Scenes, press "↺ Return to boot-guard defaults" (or reboot), then:
sudo centurion-calibrate --budget 15
centurion-calibrate --show        # what was learned
```
Results accumulate in `/var/lib/centurion/signature.json`. After a BIOS / hardware change the old signature is set aside automatically.

### 7.4 Games
**Game launch** sub-tab:
1. Choose a preset and press **★ Use for games**.
2. Steam → game → Launch options:
   `centurion-gamemode WRAP -- %command%`
3. Lutris → Configure → System options:
   - Pre-launch script: `/usr/bin/centurion-gamemode PRE`
   - Post-exit script: `/usr/bin/centurion-gamemode POST`
   - Command prefix (optional launch boost): `/usr/bin/centurion-gamemode RUN`
4. Pin a preset per game by appending its name, e.g. `PRE "Competitive"`.

Game mode is reference-counted: the first game applies, the last exit restores. If the preset is not approved yet: `centurion-gamemode APPROVE "Name"`.

**Heroic and Bottles** have no hook like Lutris: tick *Detect games started from Heroic and Bottles* (System at game start). Centurion then notices their games itself (Heroic: `HEROIC_APP_NAME` in the game's environment; Bottles: a Windows program running in a bottle) and runs the same PRE / POST — game scene, ★ game preset, session record. It only looks at processes while Heroic or Bottles runs; games you already wrap with `centurion-gamemode WRAP` are left alone.

**Drift banner:** when TLP, power-profiles-daemon, tuned, auto-cpufreq or a script changes a setting Centurion applied, a banner at the top of Optimizations lists it (what it is now, what it was) and the tools found running; *Re-apply* puts the active scene (or preset) back. A tray notification says the same, with a *Re-apply* button.

### 7.5 Boot options
Shows kernel parameters that help (ASPM, THP, amdgpu mask…) as copy-paste text. Nothing edits your bootloader.

### 7.6 Apply at boot
**⏻ Apply at boot** makes the selected preset the boot preset (needs `centurion-tune`). **Clear boot** removes it.

## 8. Lighting (Spectrum and 4-zone keyboards)

1. Pick one of the 6 hardware **profiles**.
2. Select keys: click, drag, Ctrl / Shift to add.
3. **Paint selection** (static colour) or add an **effect** (colour change, pulse, wave, rain, ripple, type lighting, rainbow…) with speed, direction, colours.
4. **Apply to profile** writes it to the keyboard (limit: 960 bytes, shown under the editor). *Revert* discards edits; *Factory reset profile* restores defaults.
5. **Brightness (0–9)** and **Lid logo** apply immediately. Tray: *Keyboard Lighting*. Scenes can set profile + brightness.

Apply when finished — do not apply in a loop (writes go to non-volatile memory).

**4-zone RGB keyboards** (older Legion 5 / LOQ / IdeaPad Gaming) get a smaller Lighting tab: effect (static, breath, wave, smooth), brightness, speed and four zone colours, then **Apply**. The keyboard cannot be read back, so the tab shows what it applied last.

## 9. Health

The sub-tabs are grouped: **Overview**, **Diagnostics** (Monitor, Boot history, Curve stability, Kernel log), **Power** (Sleep, Battery use, dGPU power, Cooling), **Network**, **System info** (SMART and the tool pages) and **Backup**. Health opens on the sub-tab you had open last time.

### Overview
The first sub-tab: one card per area, coloured by its level — **ok** (green), **watch** (yellow), **attention** (red), or **no data** (grey) until there is something to judge:
- *Kernel errors* — critical and error lines of the last 24 h and 7 days.
- *Unclean shutdowns* — boots that ended in a crash, hang or power loss: watch at 1, attention at 3 or more. Once you have looked into them, **Mark reviewed** on the card stops them from counting (the card says how many are marked); the next unclean boot counts again. Boot history still lists them all.
- *Battery health* (only with a battery) — full capacity against design: ok from 80 %, watch from 70 %, attention below.
- *Cooling test* — the verdict of the last cooling test (stable, close to the limit, reached the limit, or inconclusive).
- *Last backup* — age of the newest archive in the backup folders: ok up to 7 days, watch up to 30, attention after that. The system image folder (`/`) is not read.
- *dGPU awake time today* (only with an NVIDIA dGPU) — the share of today's time on battery the GPU was awake; watch above 20 %.
- *Sleep drain* — battery per hour asleep (watch above 2 %/h) and any dGPU error on wake (attention).

**Show** opens the sub-tab with the details. The worst level also appears as a small badge on Home and as a line at the top of the tray menu, both hidden until the first reading. The thresholds are fixed rules of thumb, not a diagnosis.

Nothing runs in the background: the page reads its data when it is shown, once at start-up, a few seconds after a kernel fault, and on **Refresh**.

### Diagnostic report
**Create diagnostic report…** at the bottom of Overview builds one `.tar.gz` for a bug report. A preview comes first: every file with its status (included, not present, too large or unreadable — the last two are left out), and the text as it will be saved. **Save…** writes the archive (readable by you only) where you choose. Nothing is uploaded.

The report holds the system identity from DMI (vendor, model, BIOS, kernel, release, NVIDIA driver version), the Health records (health and kernel logs, sleep and cooling records, battery and dGPU usage, the boot record, the last 200 writes Centurion made to sysfs or firmware) and a README. Long logs contribute only their recent part.

Before saving, serial numbers, UUIDs and asset tags are replaced with `<hidden>`, MAC addresses with `<mac>`, IPv4 addresses with `<ip>`, and kernel addresses with `<addr>`. The replacement is best effort: look through the preview before you attach the archive anywhere public.

### Monitor
*Scan now* lists NVIDIA Xid / GSP, MCE, AER, lockups and guard rollbacks with uptime. Events are saved across boots — **Open saved log** / **Clear saved log**. Use it first when games crash after tuning.

Above the list, a chart of the last 30 days: one column per day, stacked by kind (Xid, NVMe, file system, and so on), read from the saved log. Click a day to show only its rows from this session; click it again to show all. The list holds this session only, so an older day points you to the saved log.

The same event repeating within five minutes is one row, marked `×N`, with its first and last uptime; the saved log still has every line. **Mark all seen** stops the events shown so far from counting as new: the summary then reads *Not seen yet* and counts only what came after, and the Overview's kernel card and the Home badge follow the same count. Nothing is deleted by it.

Double-click an event (or right-click it) to see it in the Kernel log: the 20 saved lines before and after it, the event itself in bold. The menu also offers the page the event points to, when there is something to do there now: dGPU power for Xid 79 and D3cold; **Reset GPU curve to stock** for a GPU Xid while a GPU curve is in effect; the CPU curve tab for a machine check or lockup while a CPU curve is in effect; SMART for NVMe and file system errors.

Also recognised: NVMe timeouts and controller resets, file system errors (ext4, Btrfs, XFS), Intel iGPU hangs (i915 / xe), Wi-Fi firmware crashes (iwlwifi, mt79xx, ath1xk), critical / throttling temperatures, out-of-memory kills and kernel oops / BUG / WARNING lines (with the module name when the kernel prints one). Their counters appear in the summary line once seen. They are shown and saved like the rest but do not count against a CPU / GPU curve in Curve stability.

### Boot history
One row per boot Centurion has seen: when it started, how long it lasted, how it ended (**clean**, **unclean** = crash, hang or power loss, **running**, or *not tracked* when the boot guard did not run in it), its kernel events and the CPU / GPU curves that were in effect. Select a row for the details. For an unclean boot you also get the last 30 saved kernel-log lines of that boot and the pstore records (panic text the kernel saved across the crash, with kernel addresses removed). Repeated unclean boots with the same curve in effect point at that curve.

When the previous session ended unexpectedly, a band at the top of Monitor says so (*Show* opens this page, *Dismiss* hides it for that boot).

pstore records pile up in firmware (EFI) storage on systems without systemd-pstore. **Archive and clear pstore…** (only shown while there are records) copies them to `/var/lib/centurion/pstore` — with kernel addresses removed, the newest 30 are kept — and then deletes them from `/sys/fs/pstore`; a record that could not be archived is not deleted. The deletion is listed in `/var/log/centurion/writes.log`.

### Curve stability
Every CPU / GPU curve applied (tabs, scenes, game hooks, boot autoload) is journaled by the helpers; this page lays the saved Xid / GSP / D3cold and machine-check / lockup errors over it: hours in effect, errors, errors per 10 h per profile, highlighted when clearly worse than stock. Optional guard: *Take the GPU curve back to stock after an Xid* and the same for the CPU curve after a machine check — otherwise you only get a notification naming the curve. After such a reset the notification has *Undo*, which applies the saved profile that was in effect again (not for unsaved edits). The curve tabs also have **Trial apply**: after Apply you have 30 s to press *Keep*, else the curve goes back to stock.

**Memory test** checks BIOS memory timings or speed: it writes and reads back a share of the free memory (10–90 %, default 70 %) for 5, 15, 30 or 60 minutes with `stressapptest` (preferred) or `memtester`, as you, no password, while machine checks reported by the Monitor count as errors. The machine is slow meanwhile. Each result is kept (newest 50, `~/.local/state/centurion/memtest.json`) with the BIOS memory timing settings it ran under — speed and the overridden timings, "changed this boot" when a new setting is not in effect yet; read without a password on the Legion Pro 7 16AFR10H, "not readable" elsewhere. memtester must lock the memory it tests: it is held to the session's memlock limit (`ulimit -l`), and a limit under 64 MiB is reported with how to raise it; stressapptest needs no lock. A failure also raises a *Curve guard* notification. Without either tool the section shows the install command.

**GPU test** checks a graphics-card undervolt, overclock or VRAM offset: it runs work whose correct result is known on the chosen GPU (never an unnamed "first" one, so laptops pick the discrete card) for 5, 15, 30 or 60 minutes (default 10) and counts every wrong result, as you, no password. Run **VRAM** after changing a memory offset (it fills 10–95 %, default 80 %, of the video memory) and **Core** after changing a core curve. The screen may stutter meanwhile. Needs the Vulkan driver of your GPU. A lost GPU, any wrong result, or an Xid / amdgpu error reported by the Monitor during the run is a failure. A pass is evidence, not proof. Each result is kept (newest 50, `~/.local/state/centurion/gpustress.json`) with the curve state in effect.

### Sleep
Suspend mode, suspend statistics (failures, last failing device), hardware-sleep time, and one row per sleep from the sleep hook: duration, battery used and %/h, hardware-sleep share, NVIDIA dGPU state before → after, what woke the machine. **Wake sources**: untick an ACPI device to stop it waking the laptop (remembered; restored at boot by `centurion-tune`). **Suggestions**: an enabled wake source that woke the machine at least twice with the lid closed (neither the lid nor the power button among the wake sources) is listed with the battery the machine then spent awake before sleeping again, and a *Disable* button that does the same as unticking it. The battery cost is measured only on battery and when the next sleep came within 2 h.

**Warn when the lid has been closed this long** (default 10 minutes, 0 = never): a laptop shut in a bag but still awake gets one tray notification after that many minutes, with the CPU temperature and the fastest fan. It is not sent when a monitor is connected besides the built-in panel, and nothing is checked while the lid is open. It works while the window is closed, from UPower's lid signal; no polling.

### Cooling
Does the laptop still cool as well as it did? **Run cooling test** loads every CPU thread for 2, 4 (recommended) or 8 minutes after a short cool-down, and records die temperature, the fastest fan and the clock. Nothing runs in the background; the fans will be loud. The first clean run under a set of firmware settings (PL1 / PL2 / PL3, CPU temperature target, power profile, power source) is the **baseline**; later runs are compared with it. A hotter die at the same load, louder fans or, when the firmware holds the die at its temperature target, a lower clock suggest dust in the heat sink or dried-out paste: clean the vents and fans first. A changed power limit or temperature target starts a new baseline, because it changes how much heat the load makes. Runs disturbed by other programs or started warm are marked and not compared. Runs made under other settings stay stored and are listed in grey with what differs (for example *temperature target 90 → 89*); they are never compared with the current settings. For a fair test: charger in, fans on Auto, other programs closed, same room, same length. After cleaning, **Use the latest run as baseline**. **Forget results for these settings** deletes the stored runs; they live in `~/.local/state/centurion/cooling.json`.

Every run also records how unevenly the fans spin (the gap between the fastest and the slowest fan, in points). A gap that grows by 15 points or more against the baseline is a watch item — one fan is slower than the others: dust in that fan, a blocked duct or a loose cable. A baseline from before this was recorded cannot be compared.

**Fan max test…** puts every fan at maximum (the same as *Max all fans* on Home) for 30 seconds, after 5 seconds of reading, and records each fan's highest speed. Afterwards the fans go back to what they were set to on Home. It needs the Home tab's fan controls, and it asks first because the fans will be loud. The first test under a set of firmware settings is the baseline for the peaks; a fan peaking 8 % or more under its baseline is flagged (dust in that fan, or a blocked air path). Cancelling restores the fans at once. **Forget results for these settings** also forgets the fan baseline (`fanmax.json`).

**Trend** (above the table, shown from two clean runs on) plots the steady die temperature of each clean run under the same firmware settings, by day.

**Overdue reminder**: the *Remind me when the newest clean test is older than* setting (default 120 days, 0 = off) sends one tray notification when the newest clean run is that old, at most once a week. Centurion checks it once, a few minutes after start-up, and does not check again until the next start. Nothing is shown when no clean run exists yet. The notification is in the *Cooling test reminders* category under Notifications.

### Battery use
Time left at the current draw, battery drain per scene (energy actually taken from the battery while each scene was active, sleep excluded) with "full charge lasts ~", and a daily capacity / health history.

The charts: average draw per scene as bars, and the health line (full charge against design capacity, one point per day). **Wear forecast** under the history fits a straight line to the last 180 days of daily samples. With at least 30 samples it says when health reaches 80 % of design at the current rate, in months. A flat or rising trend gets no forecast, and fewer than 30 samples get none either. It is a rule of thumb from the samples, not a warranty.

**Charger use** — time on the charger and how much of it the battery spent at full charge (98 % or more), kept for 60 days. When more than 70 % of the charger time over the last 30 days was at full charge (and at least 20 hours of charger time), and the battery has a *Long_Life* charge mode that is not selected, the page offers **Set charge mode to Long_Life…** (asks for the password, same helper as Home). Below that, **the stretches on the charger while the battery still drained** — start, length, energy lost and scene. Such a stretch means the adapter could not carry the load; a longer adapter or a lighter scene fixes it. Stretches under a minute are not listed. The record lives in `~/.local/state/centurion/battery.json` (`ac_days`, `ac_drain`); nothing is polled beyond the existing one-minute read.

### dGPU power
Why the NVIDIA GPU is awake: runtime-PM state, sibling functions, upstream port, open handles. **Copy report** for bug reports.

**Usage on battery** — awake time per day (the share of time on battery), and how many watts more the battery spends while the dGPU is awake than while it sleeps. The comparison appears only after at least 30 minutes on battery in each state. **Wake-ups (last 20)** — each time the GPU left runtime suspend, with the programs that had its device open at that moment (read from `/proc`). The record lives in `~/.local/state/centurion/dgpu.json`. It is sampled once a minute while Centurion runs, from the runtime-PM counters in sysfs. Nothing asks the driver or NVML for data, so looking at it never wakes the GPU.

**Long-awake warning** — on battery, a dGPU that stays awake for 10 minutes without a break raises one desktop notification naming the program that held it. *Show* opens this page. Each awake stretch is reported once; the warning is not stored.

### Network
Needs the `centurion-netguard` service.
- **Connections** — live sockets with the program behind them. Right-click: *Details…*, *Kill connection*, *Whitelist program*, *IP info*, blacklist options.
- **Block non-whitelisted Wine / .exe programs** — a Windows program not on the whitelist is cut off at its first packet and listed in **Blocked**. Whitelist it from the right-click menu or **Rules**. **LAN exempt** leaves private addresses alone.
- **Rules** — *Whitelist* (path, folder or name) and *Blacklist* (IP, CIDR `198.51.100.0/24`, range `192.0.2.10 - 192.0.2.80`). **Add many…** bans a pasted list as one group you can remove with one click. Right-click an address: blacklist the IP, its subnet, or the owner's whole network (from whois).
- **Subscriptions…** (Rules) — a published list, e.g. `https://iplists.firehol.org/files/firehol_level1.netset`, banned as one group named after it. Add it with a name, an https address and an interval (daily, weekly, monthly). Nothing is downloaded on its own: when an update is due, the page says so, and **Update now** downloads the list here and applies it with one administrator prompt. A failed download, an invalid entry or more than 100 000 entries keeps the group as it was. **Remove** also un-bans its addresses. The daemon never goes online itself.
- **Log connections** — on: every new connection to and from the machine goes to **Log** (and `/var/log/centurion/connections.log`). Off: nothing is logged.
- **Traffic accounting** — on: the daemon counts the bytes each program sends and receives, per day. A connection is counted when it closes. **Connections** gets a *Today* column, and the **Traffic** tab lists the last 30 days (totals are kept for 90 days in `/var/lib/centurion/traffic.json`, readable by the wheel group). Off: nothing is counted; the saved totals stay.
- **Wi-Fi** — shown only on a machine with a wireless interface: link name, signal, link rate, retries and failed transmissions since connecting, with a plain warning when the signal is weak or the link loses packets, and how often the Wi-Fi firmware crashed since boot (Monitor's Wi-Fi events). Read every 3 s while the tab is open (`iw` is needed).
- **IP info** — reverse DNS + whois.

### Tool pages
Sensors · CPU · Memory · DMI · PCI · PCIe detail · USB · Storage · SMART · NVIDIA · Graphics · Kernel · Battery · Thermal · Security · Firmware updates. Each runs the matching system command and shows the output (DMI, PCIe detail, SMART need the admin password; NVIDIA and Graphics may wake the dGPU). A tool that is not installed shows the command for your distribution (`emerge`, `pacman -S`, `dnf install` or `apt install`, from `/etc/os-release`), or only its name when the distribution is not recognised.
- **Thermal** — every ACPI thermal zone's type, temperature (millidegrees Celsius) and trip points. Shown only when the kernel has a thermal zone; many AMD laptops have none, their temperatures are on the Sensors page.
- **Security** — CPU vulnerability mitigations, kernel lockdown, Secure Boot (`mokutil`), the kernel taint flags decoded into one line per flag, and the microcode revision.
- **Firmware updates** — `fwupdmgr get-devices` and `get-updates`. Looking for updates contacts the LVFS server on the internet, so this page runs only on **Run again**, never when it is opened.

PCI lists the live link of each GPU and NVMe drive. A link running with fewer lanes than the device supports is flagged with a line naming the lane count; check again under load, because a lower speed at idle is normal and only noted.

**Why doesn't the hardware sleep?** appears when the last three sleeps of at least 10 minutes each spent under 2 % of their time in the hardware (SoC) low-power state. One click reads the settings that usually keep it from being reached: the suspend mode, PCIe ASPM policy, NVMe power states and runtime power management, USB devices kept awake, whether the NVIDIA GPU is powered now, and the AMD / Intel power counters from debugfs (root; the password is not asked for an active session). It changes nothing. Findings are listed in plain words, and the raw counters are shown below them.

**Can this machine hibernate?** (Sleep) checks, without a password and without changing anything, what a hibernate needs: the kernel offers it (`disk` in `/sys/power/state`), no kernel lockdown (Secure Boot usually turns it on, and the kernel then refuses), swap on disk at least as large as the memory (zram does not count: it lives in memory), `resume=` on the kernel command line or systemd's `HibernateLocation` EFI variable, and whether the swap is encrypted (plain dm-crypt with a random key cannot hold the image; LUKS must be unlocked by the initramfs).

### Kernel log
Warnings or worse, kept across boots.

- **Level** — *WARN and worse* (everything saved), *ERR and worse*, *CRIT and worse*, *ALERT and worse* or *EMERG only*.
- **Group repeats** (on by default) — messages that differ only in their numbers (counters, addresses, process ids) become one line with `×N` and the time of the last one. The saved log keeps every line.
- **Only new** — shows the messages that no earlier boot in the saved log had. After a kernel or driver update this is the list of what changed.
- **Mute this message** — right-click a line. Muted messages are hidden (the count is shown under the list); **Show muted** brings them back, and **Unmute this message** undoes it. The list is kept in `gui.ini` (`klog/muted`).

The page reads the saved log when it is shown or its options change; nothing runs in the background for it.

### Notifications…
The **Notifications…** button on the Monitor's toolbar (Health) picks which alerts reach the desktop:
- *Hardware and driver errors* (Health scan), *Blocked connections* (Network guard), *Backup reminders and finished backups*, *Curve guard* (suspect or reset curves), *Disk health* (SMART), *Lid closed while the machine stays awake* and *Cooling test reminders*. A switched-off category is hidden on the desktop; the event itself stays where it is listed (Monitor, Network, Curve stability, Backup, SMART, Cooling).
- *Hardware and driver alerts: critical only* — hides plain errors and keeps critical ones.
- *While a game runs, hold these alerts* — the alerts that arrive during a game session wait, and when the session ends one summary notification lists them (the first five, then the count).

Other notifications (a game detected, a scene switched, fans put back to Auto) are not in these categories and always show. Settings are kept in `gui.ini` (`notify/…`).

### Backup
- **Centurion configuration** — scenes, presets, curves, lighting, boot profiles, network rules, calibration. Optional *System profile* (Portage config, kernel config, fstab, package list — reference only) and *Logs*. Saving needs no password; restoring the root-owned part does. A safety copy is made before every restore.
- **System image** — choose folder and compressor, options *Low priority*, *Verify*, *Include /home*, retention. Writes `backup-DATE.tar.gz` of `/`. **Restore** to any target path (type `RESTORE` for `/`). Admin password every time.
  - *Incremental* — writes only what changed since the newest image in the folder (`backup-DATE_HHMMSS-iN.tar.gz`, up to 6 increments after a full image). An increment keeps its chain's encryption: switching encryption on or off, or to another public key, starts a new chain with a full image (the log says why); with a passphrase, the chain's passphrase is checked first. Retention removes a whole chain (a full image with its increments), never a single increment. In the list an increment sits under its full image, with the size of the whole set.
  - **Restoring an increment** unpacks the full image and each increment in order. What was deleted or renamed on the system between the images is deleted or renamed again — but only among the files these images themselves restored. Files the target has of its own, everything the images excluded (`/home/*`, `/mnt/*`, the disk you restore from…) and anything behind a mount point are never touched. (GNU tar's own incremental restore deletes all of those; Centurion does not use it.)
  - *Encryption* — **Passphrase**: asked (twice) when the image is made and when it is read back, never stored; the image is verified right away like an unencrypted one. **Public key (age)**: paste an `age1…` key or make one with **New key pair…** (`age-keygen`; the secret key goes into a file you choose, readable only by you — move it off this computer). The image is written as `….tar.gz.age`. Reading it back asks for the `AGE-SECRET-KEY-…` line or the key file (**Load key file…**); an image made to a key is verified later, from the list, with that key. Secrets are used for that run only: they reach the helper on its standard input, a key goes to age through a pipe and a passphrase through age's own prompt — never into a file. Without the passphrase or the secret key the image cannot be read. The snapshot of an encrypted image (the list of every archived path) stays in `/var/lib/centurion/backup-snar`, not next to the image.
  - *Verify selected* works on several archives at once (one line per archive). Each result is remembered (`~/.local/state/centurion/backup-verify.json`) and shown in the list: *readable DATE*, or in red *FAILED verification* with the reason, until the file changes. An increment whose earlier images are missing is reported as readable but incomplete.
- **Automatic** — the configuration is backed up on its own when the newest one in the folder is older than the set interval (default 7 days; no password; only `…-auto` archives are pruned, keep-newest count). A system image needs the password, so Centurion only **reminds** you (tray, at most once a day) when the newest image is older than the interval you set (default 30 days, 0 = off).
- **Compare two…** — select two *Centurion configuration* archives (Ctrl+click); they are put in time order. **Compare with now…** — one archive against the configuration as it is now ("what did I change since this backup?"). A window lists, under *Changed / Added / Removed*, each file and inside it the keys that changed (JSON, `.ini`, and KEY=value files such as a kernel `.config` — `# CONFIG_X is not set` counts — `conf.d` or `make.conf`), or the lines removed and added for other text. When both have a system profile, *Packages* shows installed packages added, removed and at another version. Logs and per-boot facts (mounts, loaded modules) are not compared; files that cannot be read now are listed apart rather than as removed. A filter box and **Copy as text**. Reads only, restores nothing.
- **Before firmware writes** — the BIOS memory timings, BIOS CPU OC, GPU MUX mode and a forced iGPU-only switch persist in firmware and always ask for the password. Right before each one, the configuration is copied to the same folder as `…-pre-memtiming`, `…-pre-biosoc`, `…-pre-gpumux` or `…-pre-igpu` (no password; a system profile is included when the Backup page has it ticked). Only the newest 5 of these copies are kept. If the copy fails, the write still goes ahead and a notification says why.

## 10. Command-line cheat sheet

```sh
centurion --window            # open the window
centurion-gamemode STATUS                      # game-mode state
centurion-gamemode SCENE "Gaming"              # apply a scene from a script
centurion-gamemode RESTORE                     # undo game tuning
centurion-autotune gaming                      # preview a preset
centurion-autotune gaming --save               # save as "Auto Gaming"
centurion-autotune audit --fix                 # repair unsafe saved values
centurion-autotune report 30                   # memory / PSI over 30 s
sudo centurion-calibrate --budget 15           # measure the machine
centurion-boot-guard status                    # boot-guard state (sudo … reset to clear)
centurion-netguard status | list | whois 1.2.3.4
sudo centurion-intel-uv read                   # Intel undervolt state
nvcurve sensors                          # hotspot / VRAM temps, throttle reasons
sudo nvcurve reset-all                   # everything NVIDIA back to stock
centurion-gamemode OVERLAY                     # "Scene · Profile" line (MangoHud exec=)
```

**D-Bus API** (session bus, while Centurion runs — for KDE Connect, Home Assistant, Stream Deck, scripts):

```sh
D="gdbus call --session -d com.centurion.Centurion -o /com/centurion/Centurion -m com.centurion.Centurion"
$D.ListScenes                    # ['Gaming', 'Quiet']
$D.ApplyScene Gaming             # queued like a tray click; signal SceneApplied(name, ok)
$D.ActiveScene
$D.PowerProfiles                 # profiles the firmware offers
$D.SetPowerProfile performance   # signal PowerProfileChanged(profile)
$D.PowerProfile
$D.Sensors                       # Home's Live rows: cpu, fans, storage, power, igpu, battery
$D.SensorValues                  # the same as numbers: cpu_temp_c, fan1_rpm, battery_percent, …
$D.HealthSummary                 # Health → Overview: worst level (-1 no data, 0 ok, 1 watch, 2 attention) and the cards
$D.HealthEvents 0                # this session's kernel events with seq > 0, oldest first; signal HealthEvent(kind, level, title)
```

Any process of your user can call it, like `centurion-gamemode`; root work still asks polkit as usual.

## 11. When something goes wrong

Go down the list until it is fixed:

1. **Odd behaviour after tuning** → Optimizations → *Restore originals*.
2. **Still odd** → *↺ Return to boot-guard defaults*, then `centurion-autotune audit --fix`.
3. **Crash on boot** → boot-guard pauses presets automatically; fix the offending undervolt / curve, then *Resume boot presets* on Home (or `sudo centurion-boot-guard reset`).
4. **Bad GPU curve** → NVIDIA tab → *Reset All*; or `sudo nvcurve reset-all` from a console.
5. **Bad CPU curve** → Ryzen → *Reset Curve Optimizer* (tray: *CPU Curve → Reset curve*).
6. **dGPU gone after iGPU-only** → Home → *Bring NVIDIA back*, or reboot.
7. **BIOS setting changes by itself** → read `/var/log/centurion/writes.log` to see what Centurion wrote.
8. **Laptop will not boot after BIOS memory timings** → BIOS recovery / reseat RAM; this is why that editor asks for confirmation — change small steps.
9. **Restore a known-good setup** → Health → Backup → *Centurion configuration* → restore.

## 12. Glossary

| Term | Meaning |
|---|---|
| **Scene** | Named setup of the whole machine |
| **Preset** | Named set of Optimizations rows |
| **★ / ⏻** | ★ = default (games / GPU boot curve); ⏻ = applied at boot |
| **Boot guard** | Pauses boot presets after a crashed boot |
| **CCD** | CPU chiplet; on X3D chips one CCD has the V-Cache |
| **CO** | Curve Optimizer (Ryzen undervolt) |
| **cTGP / Dynamic Boost** | GPU base power limit / extra shared with the CPU |
| **EPP** | Energy-performance preference of the CPU governor |
| **PSI** | Linux pressure-stall information (stall time for CPU / memory / I/O) |
| **D3cold** | Deepest PCIe power-off state — what lets the dGPU sleep |
| **MUX** | Hardware switch that wires the panel to the iGPU or the dGPU |
