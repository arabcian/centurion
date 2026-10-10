# Autotune

Autotune profiles the machine and proposes an Optimizations preset for one of
four goals: **Power saving**, **Gaming**, **Bare throughput**, **Optimal desktop**.
Nothing is written until you press *Apply checked* (or save it into a preset/scene).

## How a value is chosen

There are two kinds of rules.

**Structural rules** have one right answer for the hardware: amd-pstate active,
V-Cache CCD roles, game affinity / IRQ steering on X3D, amdgpu DPM `auto`,
bringing a parked CCD back, BBR + fq, swap cost model (kernel doc: swappiness
> 100 for in-memory swap), and amd-pstate's per-core **EPP boost** (`cpu.epp_boost`,
patched kernels): on for **Gaming** and **Throughput** as part of the goal itself -
neither the signature nor the joint model can take it away (it stays in the model
as fixed context, so the other knobs are decided next to it). Desktop turns it on
by rule, power saving off; there the measurements may overrule the rule (below).

**Scored knobs** are every setting with a real trade-off. Each candidate value
has an effect vector over five objectives relative to the reference
("leave it", or the kernel default):

| objective  | meaning |
|------------|---------|
| latency    | frame-time / input / wake-up smoothness |
| throughput | work per second |
| power      | idle and load power, heat |
| footprint  | RAM held by the setting (reserves, THP bloat, dirty cache) |
| stability  | risk of hangs, resume failures, OOM kills (always a cost) |

`U = Σ weight × effect − 0.10 × deviation`. A candidate is written only if it
beats the reference by 0.03 — otherwise the setting is left alone. Effects are
ordinal estimates taken from kernel documentation plus this machine's evidence;
a knob that fixes a problem the machine does not show earns nothing.

## Rules and measurements

A rule's choice is prior knowledge (kernel documentation plus this machine's
evidence); a calibration run is a measurement with a field of view. Two things
keep a benchmark that cannot see a knob from voting on it:

- **Seen or not.** A knob counts as *seen* when at least one of its values moves
  this goal's utility by 2 posterior standard deviations in the joint model. Only
  then do measured effects replace the rule's estimates (in proportion to the
  evidence, as before). For a knob the benchmarks do not see, "measured: no
  effect" is the benchmark's blindness: the estimate stays.
- **A rule's choice needs credible evidence against it.** In the joint pass a
  value chosen by a rule pays neither margin nor modesty and goes back to the
  reference only when the model puts it below the reference with confidence
  (mean + 1 sd < 0, in the context of the other choices). Values picked from the
  measurements alone (keys no rule knows) and every *additional* change still
  have to clear the margin in context.

Before this, every calibrated knob had to prove itself again in the benchmarks:
whatever they could not measure went back to its boot default, and the more a
machine was calibrated the less autotune changed. The notes of a run say how
many rule-based choices were kept and how many of them sit on unseen knobs.

Default weights (latency, throughput, power, footprint, stability, storage; heat and
GPU budget are relevances like load, 0–1):

| goal       | lat | thr | pwr  | mem | stab | storage | heat | budget |
|------------|-----|-----|------|-----|------|---------|------|--------|
| Gaming     | 1.0 | 0.6 | 0.15 | 0.4 | 1.0 | 0.5 | 0.5  | 1.0 |
| Desktop    | 0.7 | 0.3 | 0.7  | 0.6 | 1.0 | 0.6 | 0.15 | 0 |
| Throughput | 0.2 | 1.0 | 0.2  | 0.5 | 1.0 | 0.7 | 0.6  | 0 |
| Power save | 0.2 | 0.1 | 1.0  | 0.5 | 1.0 | 0.4 | 0    | 0 |

**Heat** is the share of the time the goal runs heat-soaked (see *Heat as a design
factor*); **budget** the relevance of the CPU-GPU power budget (see *CPU-GPU budget*).

**Storage** is a relevance, not a fifth objective: the calibrated I/O knobs
(Storage rows and the dirty window) are measured by their own benchmark suite
(the IO phase below), and its latency / throughput / power / footprint count
with the four weights above *times* the storage weight. 0 = the disk does not
matter for this goal (those knobs keep their rule-based values), 1 = an I/O
effect counts as much as the same CPU/memory effect. A game streams assets but
is mostly CPU/GPU-bound, a desktop waits on saves and launches, bulk work moves
data — hence the defaults.

**Load share** is the second relevance (0–1): how much the calibration's loaded
phase (memory held down and fragmented, half the CPUs busy) counts against the
quiet one for a knob measured in both. Defaults: throughput 0.7, gaming 0.6,
desktop 0.4, power saving 0.2. It was a constant per goal; it is now a weight
(`load`).

Change them per goal with **Weights…** next to the Autotune button, or
`centurion-autotune <goal> --weights latency=1.2,footprint=0.8,storage=1,load=0.5`.
Range 0–3 (load 0–1); stability cannot go below 0.5. The weights used are stored
in the preset's `autotune` block. The GUI stores and sends only the weights you
changed.

**Weights follow the machine's history.** Three weights left at their defaults
lean towards what this machine has shown over its recorded uptime (see *Evidence*
below), each by at most half, upwards only, and only with five hours of history:

| weight | leans on | full effect at |
|---|---|---|
| footprint | share of uptime some task stalled on memory | 1 % |
| storage | share of uptime some task waited on I/O | 4 % |
| load | share of uptime a runnable task waited for a CPU | 20 % |

Every change is a line in the report's notes. A weight you set is used as given.

## Measurement conditions

A knob's effect is only as good as the conditions it was measured in. Every calibration
session records them, and the model keeps them apart.

**Power source per run.** Every row carries what its power objective was read from:
RAPL (the CPU package, on AC) or the battery (the whole machine). They are different
quantities - a 10 % package saving is a few percent of the machine - so the idle/load
power models never mix them: a phase's power model uses the goal's source when it has at
least 12 such rows (battery for power saving, RAPL for the others), else the other source
alone, and the report says so. Rows of older signatures carry the file's last-session
flag as their source.

**Measurement context.** Each session records its context: power profile, AC or battery,
the CPU firmware limits (PL1/PL2/PL3, temperature limit, CPU+GPU offset), the GPU limits
(cTGP, Dynamic Boost; only read while the dGPU is usable), the fan mode and the Curve
Optimizer state (ryzen-co-helper records what it applied in
`/run/centurion/co-state.json`). Where a phase's runs were measured in more than
one context, the context enters the model as pseudo-knobs (`ctx.profile`, `ctx.power`,
`ctx.limits` = limits + fan + curve) whose interactions with every knob are learned like
any other pair: EPP and boost under a 45 W Quiet limit are not EPP and boost under 130 W
Performance. Autotune reads every effect *in the context the preset will run in*:
the profile of the scene it is for (GUI: *Autotune for … for scene X*, CLI: `--profile`),
on AC (on battery for power saving), with the firmware limits of the newest session
measured there. A context value never measured is ignored (an unmeasured value says
nothing), and a signature measured in one context behaves exactly as before.

`sudo centurion-calibrate --scene NAME` measures in a scene's power context: its power profile,
firmware limits and fan mode are written before the session (through the same root
helpers) and put back afterwards; its Optimizations preset is not applied (those knobs are
what is measured), curves are recorded, never changed.

**Calibration hold.** While a session runs nothing else may change the power profile,
firmware limits, fan or curves: centurion-calibrate writes `/run/centurion/calibrating.json`
and legion-profile-helper, fwattr-helper, legion-gpu-helper, ryzen-co-helper and
intel-uv-helper refuse writes from anyone else; the GUI's scene engine defers automatic
switches (a charger plug/unplug) until the hold is gone, then applies the scene for the
power source. Before, a scene switched by a charger event in the middle of a session was
measured as if it were the knob under test.

**Temperature.** Leakage power and boost headroom move with the die temperature, and runs
used to start as warm as the previous run left the die. Now:

- before every cool run the die gets time to come back near the phase's cool starting
  temperature (the phase's first cool run sets it; at most 8 s, 15 s in the DEV phase,
  and never longer than it keeps cooling);
- the start temperature (k10temp Tctl / coretemp package) is recorded per run, relative to
  the session's typical start in the same heat state, and enters the model as a linear
  nuisance term (prior 0.01 objective units per °C, shared by all sessions). A knob whose
  runs happened to start warmer no longer reads as costing power;
- on AC the EC fan is held at full speed for the session (`--no-fan-lock` to leave it on
  auto): the fan curve's hysteresis made identical runs meet different cooling. The fan
  mode is part of the context and is put back afterwards (`--restore` after a crash).

**Battery cadence.** The EC refreshes the discharge reading far more slowly than it was
polled: a 2 s window held one or two real readings, one of them often from the run before.
On battery the session first measures the reading's update period, and every idle-power
window then lasts at least four updates, drops the first one (it may straddle the knob
change) and averages fresh updates only.

**Display.** Backlight level and panel DPMS state are recorded per run; a run measured
under another display state than its session's first run (dimmed, blanked) counts 20 %
and the session says so once - turn screen dimming off for a calibration.

## Heat as a design factor

Every benchmark window is shorter than a second: what the calibration saw was a cool,
bursting machine. EPP, boost and C-state choices that win a burst can lose an hour-long
game once boost headroom is spent and leakage is up. From the "crowd" stage of the
progressive sessions on (and in deep/max sessions; `--heat` / `--no-heat` force it), heat
is a design factor: a run with the pseudo-knob `ctx.heat = hot` is preceded by a 15 s heat
soak (every CPU busy; consecutive hot runs only top it up), balanced against every knob
like a knob, so the model learns which knob effects change with heat (knob × heat pairs).
Hot runs of a batch go last; a cool run after them waits for the die. Decisions mix the
cool and hot context by the goal's **heat** weight; the optima the calibration confirms are
the cool ones.

## Devices (DEV phase, battery only)

Device power states are whole-machine power that RAPL never sees, so on AC they could
only be scored by rules. A session on battery adds a DEV phase: runtime PM of PCI and
connected USB devices, HDA codec / controller power-down, USB autosuspend of connected
devices, NVMe APST tolerance, SATA ALPM and AHCI runtime PM, disk APM, panel ABM, Ethernet
EEE. A run lets the devices settle for 3 s and averages the battery reading over at least
10 s and four fresh updates; nothing else runs. Left out on purpose: PCIe ASPM policy and
per-link ASPM (they reach the NVIDIA dGPU's links; a D3cold wake failure is a hard hang),
radios (a choice of what to use), the iGPU DPM level, suspend mode and Wake-on-LAN. The
DEV model adds to the joint model with the power weight alone (it measures nothing else);
rule-chosen device values stay unless the measurements credibly contradict them.

## CPU-GPU budget

On a laptop whose GPU takes what the CPU leaves (NVIDIA Dynamic Boost), a watt the CPU
saves in a GPU-bound game becomes GPU clock. For the gaming goal on AC with an NVIDIA dGPU,
autotune credits the power objective as throughput: the power weight grows by
`budget x throughput weight x k`, `k = c x eps x P_cpu / P_gpu`:

- `c` = the share of a CPU watt the GPU gets, `eps` = the GPU clock gained per relative
  GPU watt, both measured by `sudo centurion-calibrate --gpu-coupling` while a GPU-bound game or
  benchmark runs (the CPU is switched between two power levels - EPP performance / power,
  else boost on / off - in 15 s blocks; GPU power and clock are read with nvidia-smi; the
  result is marked rough when the GPU was not fully busy in both states);
- until then `c` comes from the firmware's Dynamic Boost ceiling (at most what the boost
  can move for a 30 % CPU saving) and `eps` = 0.35 is an assumption, which the report says;
- `P_cpu` = the game loop's package power from the last calibration, `P_gpu` = cTGP +
  Dynamic Boost of the target context. No Dynamic Boost recorded (or the dGPU off while
  calibrating): no credit.

## Verifying a preset (`--verify`)

`sudo centurion-calibrate --verify NAME [--rounds N] [--io] [--goal G]` measures a whole preset
from the approved store against the boot state, ABBA-interleaved (drift cancels), on the
idle benchmark suite (and the storage suite with `--io`). It reports the gain per objective
with its standard error, the weighted gain for the goal (from `--goal` or the preset's
name) and what the joint model predicted for the preset's calibrated part - `as predicted`,
or `SURPRISE` when the uncalibrated part or interactions the model has not seen matter. CPU
offlining, driver switches, firmware limits and radios are never written by a verification.
The result is kept in the signature (`verify`, last 20); when every change is a calibrated
knob the runs also join the design log.

## Field evidence: real games (A/B)

centurion-gamemode records every game session (start, end, the game, the preset and its values)
in `~/.local/state/centurion/game-sessions.jsonl`. With *A/B field comparison*
(Optimizations → Game launch; tune.json `"field_ab": true` or a list of game names) every
other launch of a game runs at the boot defaults (A: no game preset, no launch boost; the
game scene - the power context - stays the same), the others with the preset (B).
An A launch opens no tune-helper session, so its launcher (pid + start time) is kept in
`$XDG_RUNTIME_DIR/centurion/scene.json` (`ab_a`, `ab_owner`): the GUI counts it as a running
game (no orphan clean-up ~15 s in, no AC / battery switch under it), and once the launcher is
gone without POST the flag no longer blocks the next game's start.

`sudo centurion-calibrate --field LOG... [--game NAME]` imports frame-time logs - FLM's CSV
(`interval_ns`), MangoHud's CSV (`frametime`) or one number per line in ms - matched to the
recorded session by the log's time. Per session: median frame time, tail (worst 1 %) and
pacing (mean frame-to-frame change), the first 10 % dropped (loading). A game whose log
holds two A sessions and one B session gives rows of the load model: throughput =
-ln(median / A median), latency = -(ln(tail / A tail) + ln(pacing / A pacing)) / 2. A game is
its own session in the model (its own offset), a field row counts half a benchmark run, and
only knobs the calibration designed are kept - a row loses weight for every changed setting
the model does not know.

## Field stability

A benchmark sees what a setting gains, never what it risks. Centurion keeps the record that can
show it, at no cost while the machine runs (`/var/lib/centurion/exposure.json`):

- exposure: which knobs were changed, to what, from when to when - tune-helper adds a
  segment after every apply/restore, centurion-boot-guard opens each boot, calibration time is
  marked as its own (`@calibrate`);
- events: at a clean shutdown the boot's classified kernel events (Xid, GSP timeouts,
  uncorrectable AER and machine checks, lockups, D3cold wake failures, amdgpu resets); a
  boot that never reached its shutdown counts as an unclean end (weights: unclean 3,
  lockup / GSP / D3cold / other critical 2, amdgpu reset 1, corrected errors 0; at most 6
  per boot);
- per knob, the weighted event rate while it was changed is compared with the rate while
  it was at its boot value (Gamma-Poisson posteriors). With at least 3 h on each side, two
  weighted events while changed and P(rate >= 1.5x) >= 0.8, the knob pays a stability
  cost of up to -0.5 (shown in the report). Knobs that are always changed (a boot preset)
  cannot be judged and are never charged; field evidence never retires a key on its own
  (the events are shared by everything changed at the same time).

A calibration run in flight when the machine died is the prime suspect: its
configuration is written to `/var/lib/centurion/calibrate-inflight.json`
(fsynced) before each run, and centurion-boot-guard turns it into an unsafe set of the signature
at the next boot when that boot did not end cleanly.

## Calibrating at the next boot

*Calibrate at next boot…* (Optimizations) or `sudo centurion-calibrate --schedule-boot
[--sessions N] [--budget MIN] [--scene NAME]` (`--cancel-boot` cancels) automates the
"pause Scenes, reboot, calibrate" routine. At the next boot the boot preset is held
(centurion-tune applies nothing while a calibration is scheduled) and the centurion-calibrate-boot
service takes the calibration hold at once, so the login scene waits too. It then waits
for AC power, five minutes without keyboard/mouse/touchpad input (it watches
`/dev/input/event*`, reading only), a quiet CPU and no game, and runs the sessions one by
one. A key press or mouse move stops the running session cleanly (what it measured is
kept); it resumes after the next quiet stretch (at most six stops, four hours in all).
Afterwards it applies the boot preset and releases the hold; the GUI applies the scene for
the power source. The flag is consumed at the start, so a crash does not repeat it at
every boot. Status: `centurion-calibrate --status` (and the Optimizations tab); log:
`/var/log/centurion/calibrate-boot.log`. Enable the service once:
`rc-update add centurion-calibrate-boot default` / `systemctl enable centurion-calibrate-boot`.

## Alternatives and sensitivity

The fits are per objective, so other trade-offs cost no new measurement. Every autotune
run (GUI, or `centurion-autotune <goal>` without `--no-explore`) also reports:

- **predicted**: what the calibrated part of the preset does per objective (mean ± sd)
  against the boot state;
- **alternatives**: the same data under three other trade-offs - cooler and quieter (power
  x2.5), more performance (latency and throughput x1.3, power x0.4), leaner memory
  (footprint x2) - with the settings that change and their predicted effect; *Use these
  weights* saves that trade-off for the goal and runs autotune again;
- **close calls**: the decisions that flip when a single weight moves 30 % down or up.

## Evidence

Evidence-driven rules (`watermark_scale_factor`, boosted reclaim, working-set
protection, THP) read reclaim / stall / swap counters and pressure-stall time.

- **Across boots.** `centurion-boot-guard` stores each boot's counters at a clean
  shutdown (`/var/lib/centurion/evidence.json`, last 40 boots, 30-day
  half-life, boots under ten minutes ignored). Autotune adds them to the running
  boot's. Before, only the running boot counted - and a freshly booted machine,
  which is what a calibration asks for, has no history: those rules never fired.
  Nothing runs while the machine is up; the file is written once, at shutdown.
- **Without the calibration.** The load phase's ballast is hours' worth of
  allocation stalls and direct reclaim. `centurion-calibrate` records what the counters
  gained during its sessions (`/run/centurion/calib-evidence.json`,
  per boot); autotune and the shutdown record subtract it, and the 5-minute
  pressure averages are not read for ten minutes after a session. Before, an
  autotune run after a calibration in the same boot read its own test load as the
  machine's habit.

The report header shows the stall shares, the hours and boots behind them, and
how many minutes of calibration were left out.

## Anchored at boot defaults

The machine's own boot state is the reference, not a constant: `centurion-boot-guard`
snapshots every tunable after the kernel, the distro and your `sysctl.conf`
have run, but before TLP and before any preset
(`/var/lib/centurion/defaults.json`; `clean` = no TLP/Centurion yet;
a clean snapshot of a kernel is never replaced by a dirty one). Enable it once:
`rc-update add centurion-boot-guard boot` / `systemctl enable centurion-boot-guard`.
Without it the centurion-tune boot op takes the snapshot, else the kernel defaults are the reference.

For every scored knob:

- the boot value is the reference; a live value that drifted goes back to it
  when no candidate wins;
- distance from it costs modesty (log2 for numbers), and a number may move at
  most **2×** away (up to 8× for evidence-driven knobs such as
  `watermark_scale_factor`, in proportion to the reclaim evidence);
- **0 is a mode, not a dose**: where 0 switches a feature off (writeback
  throttling, boosted reclaim, background compaction, NVMe APST, codec
  power-down, …) going to or from 0 costs like a choice. As a dose it sat ten or
  more doublings from any boot value, so earlier versions silently dropped every
  "off" candidate whenever a boot snapshot existed. No dose is interpolated
  between 0 and a setting either;
- the THP group starts from the boot THP configuration when the kernel honours it;
- **learning:** every apply of a guarded knob and every guard rollback is
  counted per key (`outcomes.json`). A rollback adds a stability cost of
  0.6 × rollbacks / (applies + 1); two rollbacks retire the key's alternatives
  — autotune only offers its boot value from then on.

`vm.dirty_*` is the exception: the boot default is a share of RAM that knows
nothing about the disk, so those two stay derived from the write rate.

## Machine signature (calibration)

`sudo centurion-calibrate` builds a **signature** of this machine: what every CPU,
scheduler, memory and storage knob does here, alone and next to the others, idle,
under load and on the disk. Records accumulate
across runs in `/var/lib/centurion/signature.json`, tied to a
hardware fingerprint (DMI product, CPU model, RAM ±2 %; a signature of other
hardware is set aside, not merged).

**Plan.** Generated from the tunable table (CPU, Scheduler, Memory, Storage groups),
minus keys that are structural or unsafe to flip — CCD/core-type roles,
CPU offlining, driver switches, firmware power/thermal limits, the watchdog,
khugepaged pacing (needs minutes), the periodic writeback interval and data age
(5–60 s: longer than a run, so a run cannot see them) and the like
(`centurion-calibrate --list` shows the plan, every excluded key and why). Candidates:
all options for choice rows, the flip for switches, live ÷2 / ×2 for numbers, and
per-key ladders where that is wrong (swappiness 60…180, watermark headroom
128/256/512 MiB, boost ≤ 15000, per-CPU page lists 8/32/128, read-ahead
¼…4× the live value, writeback throttling off / ½ / 2× / 4×, shorter queue depths
only, …). Ladders stay inside autotune's trust region: a dose it could never pick
is a wasted run.

**No frequency ladders.** `cpu.max_freq_ccd0/1` (and Intel's `max_perf_pct` /
`min_perf_pct`) are out of the plan: a cap is a role the goal sets (the idle die
while gaming, a quiet desktop), not a dose to search, and three levels per die
were more than a third of the CPU plan on a two-CCD machine. `cpu.dynamic_epp`
is out too: switching it lets the kernel rewrite every policy's EPP behind the
journal, so the run measured an EPP change and later runs started from another
EPP than the reference. Rows an older version logged for these keys stay in the
model as context; they never decide the key (the goal's rule does).

**Design, not a queue.** The old flow measured one knob at a time (`ref, c1 … cn,
ref`), so every effect rested on two or three runs, interactions were never
seen, and a change had to prove itself alone. The default mode is a
**sequential experimental design** whose depth grows with the budget:

| depth | budget | runs/phase | space-filling | runs change | batch | confirm | chased interactions | dose refinements |
|---|---|---|---|---|---|---|---|---|
| lean | ≤ 20 min | ≤ 240 | 55 % | 3–7 (45 %: 2–4 in one cluster) | 5 | 2× | — | — |
| deep | ≤ 50 min | ≤ 520 | 50 % | 4–9, 20 % crowded with 8–14 | 6 | 3× | 8 | 4 |
| max | > 50 min, `--all` | ≤ 1000 | 45 % | 4–10, 35 % crowded with 10…half of all knobs | 8 | 4× | 16 | 8 |

(`--depth lean|deep|max` fixes the shape.) The budget is kept by wall clock — the
measured cost per run, not the estimate — so fitting time and slow runs are
counted.

**Progressive lean sessions.** A short session without `--depth` does not
repeat the same lean shape: it builds on the log and spends its time on the
next thing the data lacks, per phase, so 15-minute sessions started whenever
the machine is free (or `--sessions N` back to back) add up to a deep
calibration:

| stage | until | what the session does |
|---|---|---|
| base | every value of every knob was in ≥ 6 runs | lean space-filling (knobs that are new — new kernel, new tunable — first) |
| pairs | 90 % of knob pairs changed together in ≥ 2 runs | space-filling aimed at the pairs never seen together, interaction doubt |
| crowd | max(24, knobs) runs with ≥ 8 changes | half of the runs change 8–14 knobs at once |
| refine | one refinement session ran | doubt-driven runs, dose midpoints around the optima, 3× confirmation |
| polish | — | rotating, the least-run first: interaction/triple doubt · crowd · dose refinement |

The first three stages are read from the data itself (runs of deep/max
sessions count too); every session's strategy is noted in the signature
(`strategies`). When a progressive session's decisions are settled before its
time is up it keeps filling coverage gaps instead of stopping. `--list` shows
each phase's stage and what the log holds.

1. *Space-filling start.* Every run changes a random set of knobs together,
   balanced so every knob, every value and every **pair** of knobs appears
   about equally often — counting the runs already in the log, so a new
   session fills the gaps of earlier ones. Part of the runs change several
   knobs of one coupled cluster (memory/reclaim/THP/writeback, or CPU
   frequency + scheduler); crowded runs (deep/max) change many knobs at once —
   saturation and higher-order effects live there, and so does the optimum.
   A reference run every 6th run. Idle and load phases are designed separately;
   the time is split so each knob gets a similar number of runs.
2. *Runs chosen by doubt.* After each batch the model asks, for every goal,
   which decisions ("change this knob / leave it") are still uncertain and —
   deep/max — which **interactions** among the knobs that matter are still in
   doubt (the 2×2 contrast of two knobs in the context of the goal's optimum),
   and picks the next runs by how much they would reduce that doubt (posterior
   covariance with the doubtful contrasts, squared, over the run's own
   variance, per objective). The phase stops early when the expected number of
   wrong decisions drops below 0.35 and no interaction is left in doubt.
3. *Dose refinement* (deep/max). For numeric knobs of the predicted optima the
   geometric midpoints between the chosen dose and its measured neighbours
   (rounded to the knob's grid: 100 MHz, thousands, hundredths of the dirty
   window) become new levels and are measured inside that optimum.
4. *Confirmation.* The best configuration the models predict per goal is
   measured (2–4×) and compared with the prediction: `as predicted` or
   `SURPRISE`; either way it joins the data.

Every run measures **all four objectives** (latency, throughput, power, memory;
union of the benches the phase's knobs need), so any goal or custom weights are
decided from the same data — experiments are goal-agnostic, fits are per
objective and combined per goal without refitting.

**Phases.** Idle: quiet machine (idle power, single-thread work, wake-up, frame-loop
and thread ping-pong latency, the game loop, the contended window, short jobs,
heap probes). Load: a ballast child holds memory down to
max(1 GiB, 5 % RAM) free, re-faults 64 MiB blocks and keeps half the CPUs busy
(the same CPU suite, allocation stalls/s, heap probe, package power).
Every 4th ballast block is small pages with every other page freed again and is
re-made now and then (compaction heals it): free memory without a free 2 MiB
block, as on a machine that has been up for days — what THP defrag and the
compaction knobs really meet.
IO: the storage suite (below) on the disk holding `--dir`, nothing else running
and no ballast — the I/O knobs live here only, so storage metrics no longer
dilute the CPU/memory objectives (nor their noise the CPU/memory knobs; before,
every idle run carried the I/O benchmark and a 20 % fsync gain shrank to a
seventh of the latency objective).
Single-thread work, the frame loop, the wake-up sleeper and the ping-pong pair run as fresh threads in
several short sub-runs, so one scheduler placement (V-Cache vs frequency CCD, same core
vs another) does not decide a whole run. Package power reads only the RAPL package
domain (not psys or the MMIO duplicate) and survives counter wrap-around; the power
source (battery or RAPL) is fixed for the whole session. CPU time used by other
programs during a run is metered (kernel threads such as kswapd/kcompactd are
not counted — they are part of what the knobs change); a run where others kept
more than 0.6 CPU busy counts proportionally less.
Brakes: the ballast dies at once if MemAvailable < 256 MiB or PSI memory
full > 40 %; it is the OOM killer's first target. A run that caused an OOM kill
is bisected (halves, at most 6 extra runs) to the smallest set of changes that
still does; a single value is stored as **unsafe**, a combination as an
**unsafe set** — neither is ever picked (subsets of an unsafe set are fine).

**THP.** Tested as its own family: the `thp` group (enabled never / madvise /
always, each with or without 16–64 KiB mTHP), `thp.defrag`, `thp.shmem_enabled`
and the 128 KiB – 1 MiB mTHP sizes as separate knobs, idle and loaded. The heap
probe (a disposable child, 2 MiB-aligned heaps) measures what THP changes:
sparse-heap footprint (bloat), fault latency and huge-page coverage of a plain
heap, dependent random loads over a plain heap (TLB reach programs get without
asking), over a `MADV_HUGEPAGE` heap (programs that opt in, with its per-2 MiB
fault p99 — defrag/compaction stalls) and over shared memory (memfd,
`shmem_enabled`). `centurion-calibrate --only @thp` runs just this family (`@mem`,
`@cpu`, `@sched` likewise).

**Measuring tails.** Every latency figure is the expected shortfall at p99 —
the mean of the worst 1 % of the window's samples, at least 5. A single order
statistic of a short window jitters more than the effects being measured (p99
of 40 fsyncs is simply the maximum).

**Frame loop.** A thread wakes every 4 ms (240 Hz) on an absolute deadline,
runs a fixed piece of work over a 256 KiB working set and records deadline →
done. That is what a game's frame loop or a compositor meets: timer wake-up,
C-state exit and how fast the clock comes up for a short burst after idling
(EPP, boost, idle governor, wake-up QoS). The sleeper (wake p99) and the
sustained spin (1-thread work) each see only part of it.

**Workloads the knobs act on** (benchmark set 4). The earlier CPU suite had two
kinds of load: a core that is 7 % busy (frame loop) and cores that are 100 % busy
(1-thread and all-thread spin) on a machine with free CPUs. Most CPU and scheduler
knobs do their work in between, or only when CPUs are short:

| probe | metric (objective) | what moves it |
|---|---|---|
| **game loop**: 125 Hz frame, main thread ~60 % busy (4.8 ms fixed work, 1 MiB set), a 2.4 ms job to each of 3 worker threads per frame; frame ends when all are done | typical frame time (thr = fps), frame tail (lat = 1 % lows) | EPP, per-core EPP boost (acts on cores more than half busy), boost, CCD clock caps, C-states, where job threads wake up |
| frame loop, 240 Hz, 7 % busy | tail and now also the **median** (lat) | timer wake-up, C-state exit, clock given to a mostly idle core; the median carries the same effect at a fraction of the tail's noise |
| **contended window**: 1.5 spinning threads per CPU plus the frame loop | all-thread work, work/joule, frame tail with every CPU taken (lat) | slice, preemption, wake-up placement, migration cost, BORE - with free CPUs these have nothing to decide |
| **short jobs**: half the CPUs keep starting a child that faults a fresh 8 MiB heap, works 2 ms and exits | jobs/s (thr) | placement and clock ramp of new tasks, exec/fault path, THP; under load: reclaim on every fresh heap |

The fixed work is sized once per session, before any knob is touched (iterations
per millisecond at full clock, best of 12 windows), so it never scales with the
clock a setting gives. A CPU run takes about 6.7 s idle and 4.2 s loaded (5.4 / 2.5
before): fewer runs per session, each of which can see the knob it changes.
`kernel.sched_burst_cache_lifetime` (BORE) is no longer excluded - the short jobs
are the fork benchmark it lacked.

**Cache-bound game loop** (benchmark set 5). Every probe above works inside L2
(the largest working set is 1 MiB), so a 96 MiB V-Cache die and a 32 MiB
frequency die differed by their clock only - and a spin loop always prefers the
clock. The settings whose whole point is *which L3 a thread sits on* (cache-aware
balancing and its tolerances, preferred-core ranking, wake-up placement, buddy
and migration-cost settings, workqueue scope) were measured blind, or with the
sign of the clock.

| probe | metric (objective) | what moves it |
|---|---|---|
| **cache-bound game loop**: the game loop's frame (125 Hz, main thread + 3 jobs), the work being dependent loads over one shared set of half the largest L3 (48 MiB on a 96 + 32 MiB part): 2.4 ms for the main thread, 1.2 ms per job where the set is cached | typical frame time (thr), frame tail (lat) | all four threads on the L3 that holds the set, on the smaller one, split over both, or just migrated; THP (the set is a plain heap) |
| game loop, same run | **frame pacing**: mean frame-to-frame change over every frame (lat) | the stutter the tail shows, at a fraction of its noise (the tail is the five worst frames of a window) |
| game loop, same run, quiet phase | **package power** while it delivers its fixed frames (pwr) | what EPP boost, core boost and C-states cost for the same frames |

The loads per frame are sized once per session (`cache unit`: a thread pinned to
each L3 domain in turn, the best one counts), before any knob is touched. The
set is a line-granular single random cycle: no prefetcher follows it, and every
line is visited before any repeats, so its whole size has to stay cached.

The calibrator runs its probes with a timer slack of 1 ns. With the default
50 µs the kernel may deliver every sleep and deadline that much late - a
constant the size of the effects the idle governor, C-state and timer settings
have on a wake-up.

A CPU run now takes about 7.4 s idle and 4.9 s loaded (0.7 s more); the plan has
fewer levels to cover. Rows of benchmark set 4 are kept and count 60 %.

**Storage suite** (IO phase, `size` = 256 MiB, 512 with `--thorough`; unlinked
temp files):

| step | metric (objective) | what moves it |
|---|---|---|
| streaming buffered write + fsync, a 4 KiB fsync prober every 10 ms | write MB/s (thr), fsync tail (lat) | dirty window, scheduler, merging |
| cold sequential read, 128 KiB reads | read MB/s (thr) | read-ahead, merging |
| 4 KiB O_DIRECT random reads, queue depth 1 | random-read tail (lat) | scheduler |
| the same from several threads | IOPS (thr), busy CPU per read incl. irq (pwr) | iostats, add_random, nomerges, rq_affinity, scheduler |
| random reads while a second writer streams and commits | read tail under writes (lat) | writeback throttling, scheduler, queue depth, dirty window |
| sparse first touches of a cold mmap | mmap fault tail (lat), page cache brought in (mem) | read-ahead (fault read-around) |

The disk is the one holding `--dir` (default: the first of `/var/tmp`,
`/var/cache`, `/home`, `/` on a local disk; found through mountinfo, LUKS/LVM
slaves and partitions). tmpfs and network file systems are refused — they would
measure RAM. The Storage rows write every disk the same value, so the measured
disk speaks for all; point `--dir` at a game library to measure that drive.
A Storage row whose disks disagree (a USB stick next to the NVMe drives) is
measured from the suite disk's value; unplug removable disks before
calibrating. One IO run writes about 1.5 × `size`. The idle phase no longer
runs the I/O benchmark, so a full session writes less than before.

**Objective scale.** An objective used to be the mean of its metrics. A knob moves
the one or two metrics it acts on, so every benchmark added made every knob look
smaller: a 10 % better frame tail among eight latency metrics came out as 1.3 %,
under the 0.03 margin whatever the evidence. An objective is now the gain *summed*
over its metrics, counted as if two of them carried it (noise-weighted mean × k/2,
never below the mean): that frame tail is worth 5 %, and a knob lands on the same
scale whether or not the session also ran the memory probes.

**Older signatures.** Idle/load rows and one-at-a-time records of benchmark sets
1-3 are dropped when the file is read (they were blind to most CPU and scheduler
knobs and would vote "no effect" against the new runs); those phases start over.
IO rows are kept and rescaled (the storage suite is unchanged), unsafe values and
unsafe sets stay. `centurion-calibrate` says so once; a few lean sessions rebuild it.

**From runs to a model** (`src/model.rs`). Per run and metric: log-ratio to the
session's reference runs (+ = better), clipped to ±0.5, then combined per
objective (scale above) **weighted by the metric's noise**: from the session's reference runs
(robust sd of the log values) w = 1 / (sd² + mean sd² of the objective) —
halfway between equal weights and inverse variance — kept within ⅓…3× of the
equal share (fewer than 4 reference runs: equal weights). A tail that jumps
30 % between identical runs no longer drowns a bandwidth that moves 1 %, and no
single metric can take an objective over. No noise thresholding, so small real
effects are not thrown away; a metric counts only if 80 % of the runs have it. The model is an additive Gaussian
process per objective (Bayesian regression in kernel form):

- main effects per value; numeric ladders use a random-walk prior over their
  ordered values (neighbouring doses share strength, doses between measured
  points are interpolated), unordered choices get one term per value;
- **every pair** of knobs, and from max(150, 4 × knobs) rows **every triple**,
  as kernel terms (elementary symmetric polynomials — no term list to
  enumerate); pairs inside a coupled cluster, pairs across clusters, triples
  inside and across each have their own prior scale; a knob that responds on
  its own carries more interaction prior (heredity, from a screening fit);
- each session has its own offset and trend, plus a slow drift over wall-clock
  time (thermal, background) that the reference runs pin down; prior scales and
  the noise level come from evidence maximisation and are kept in the signature
  (re-tuned every few batches), outlier runs are down-weighted (Student-t
  style, from leave-one-out residuals);
- diagnostics: leave-one-out R² and the share of 90 % intervals that hold;
  the strongest credible interactions — pairs and triples, per objective, as
  **synergy** (more together), **overlap** (both help, less together) or
  **conflict** (together credibly worse than the better one alone) — are
  printed after a calibration and by `--show`. Rows decay with a 90-day
  half-life, another kernel (major.minor) counts half, rows from an older
  benchmark version 0.6, at most 1600 rows kept (1100 fitted). The old
  one-at-a-time records stay valid: they join as single-knob rows worth half
  their evidence. Idle/load rows of an older benchmark set that changed an
  I/O knob stay out of the idle/load models (their objectives mixed storage
  metrics in); the I/O knobs are learned in the IO phase from scratch.

**Decisions (autotune).** Every quantity is a posterior. For a goal's weights:

- stand-alone effects for the per-knob rules and THP/dirty come from the model
  (`n` = how much the data narrowed the prior), replacing the estimate in
  proportion to the evidence as before;
- then a **joint pass** searches the best combination of all calibrated knobs
  together — coordinate ascent, joint moves of coupled pairs, restarts — on the
  posterior mean minus 0.5 sd, minus modesty and learned rollback risk, and a
  margin (0.03) that every change must pay by itself. The per-knob choices are
  the starting point, so the outcome is never worse under the model; a rule's
  choice among them is held unless the model credibly contradicts it (see
  *Rules and measurements*), and a rule's value that was never measured is left
  out of the search;
- every change is then checked *in context*: gain of the whole combination
  minus the combination without that knob, lower bound `mean − 0.5·sd ≥ margin`,
  else the knob goes back to its reference (this is what drops a redundant
  second knob and keeps a pair that only pays together);
- kept: the boot-default trust region (2×, widened by one doubling for values
  the model has evidence for), unsafe values and unsafe sets, retired keys,
  the phase blend per goal (load share: throughput 70 %, gaming 60 %,
  desktop 40 %, power saving 20 %; a knob only one phase models keeps its full
  weight there; the IO model adds with the storage weight), scoped roles (cpu.epp vs cpu.epp_ccd0 — CCD roles stay
  structural), THP and the dirty window as fixed context;
- the `why` of a changed knob says what it gains alone and in context; the
  notes carry the predicted weighted gain ± sd of the whole combination.
- your weights still decide; stability risk still comes from the model and
  the guard's rollback history, never from a benchmark. The cost a rule gives a
  value (working-set protection: -0.25, -0.6 on a tight machine; writeback
  throttling off: -0.05) is charged in the joint pass as well: before, that pass
  weighed modesty and rollback history only, so a measured gain alone could buy a
  value whose risk no benchmark sees. A value the rule itself chose is not
  charged twice.

**Safety:** refuses while an Optimizations preset is active (Restore
originals first), holds the tune lock, journals every original before
writing, restores after each knob and on Ctrl-C; `sudo centurion-calibrate --restore`
after a crash. Power on AC is the CPU package only (RAPL); device power
states need a run on battery.

```sh
centurion-calibrate --list [--budget N]            # knobs, excluded keys, depth, runs and time per phase, rows logged
sudo centurion-calibrate --scene Gaming            # measure in the Gaming scene's power context
sudo centurion-calibrate --verify "Auto Gaming"    # the whole preset against the boot state (ABBA)
sudo centurion-calibrate --gpu-coupling            # while a GPU-bound game runs: CPU-GPU power coupling
sudo centurion-calibrate --field ~/flm/*.csv       # import game sessions' frame-time logs (A/B)
sudo centurion-calibrate --schedule-boot --sessions 4   # calibrate at the next boot (idle, on AC)
centurion-calibrate --status                       # boot calibration status
sudo centurion-calibrate                           # 15-minute progressive lean session (next stage of what the log lacks)
sudo centurion-calibrate --sessions 4              # four of them back to back: start it and walk away
sudo centurion-calibrate --budget 60               # max depth: crowded runs, interactions chased, doses refined
sudo centurion-calibrate --budget 30 --only @thp   # the THP family only (also @mem, @cpu, @sched, @io)
sudo centurion-calibrate --only @io --dir /mnt/games   # storage only, measured on the games disk
sudo centurion-calibrate --budget 30 --phase load  # load phase only
sudo centurion-calibrate --seed 7 --no-confirm     # other random design; skip the confirmation runs
sudo centurion-calibrate --oat                     # legacy one-knob-at-a-time flow
centurion-calibrate --show                         # effects per value (n = evidence), pair/triple interactions, model quality
```

All options:

| option | effect |
|---|---|
| `--budget MIN` | time for the session (default 15), split between the idle and load phases; kept by wall clock. ≤ 20 min = lean (progressive unless `--depth`), ≤ 50 deep, more max |
| `--all` | no time limit: max depth up to its run cap (1000 per phase) |
| `--depth lean\|deep\|max` | fixed shape regardless of budget; `lean` turns progression off |
| `--sessions N` | N sessions back to back (1–48); progressive ones each take the next stage |
| `--phase idle\|load\|io\|both` | only one phase (default `both` = all three) |
| `--only K1,K2,…` | only these keys; group aliases `@thp` (thp group, defrag, shmem, mTHP sizes), `@mem` (Memory), `@cpu` (CPU), `@sched` (Scheduler), `@io` (Storage rows + dirty window) |
| `--thorough` | every measurement window ×1.6 and a 512 MiB I/O file (less noise, slower runs; `--oat`: two rounds per knob) |
| `--dir PATH` | directory for the storage suite's temporary files, on the disk to measure (default: first of `/var/tmp`, `/var/cache`, `/home`, `/` on a local disk) |
| `--seed N` | another random design (default fixed, so a session is reproducible) |
| `--no-confirm` | skip the confirmation runs of the predicted optima |
| `--oat` | legacy one-knob-at-a-time flow (no interactions, no IO phase) |
| `--list` | the plan: knobs and values, excluded keys and why, depth or progressive stage per phase, runs and time, rows logged (no root needed) |
| `--show` | the signature: effects per value and phase, model quality, pair/triple interactions (no root needed) |
| `--restore` | put back every value an interrupted run left changed (journal in `/run/centurion/calibrate.json`), the scene power context, the fan mode; clears a dead calibration hold |
| `--scene NAME` | measure in a scene's power context (profile, firmware limits, fan); put back afterwards |
| `--no-fan-lock` | leave the fans on auto on AC (default: full speed for the session) |
| `--heat` / `--no-heat` | force heat as a design factor on / off (default: deep/max and progressive stages from "crowd") |
| `--verify NAME [--rounds N] [--io] [--goal G]` | the whole preset NAME against the boot state, ABBA |
| `--gpu-coupling [--secs S]` | CPU-GPU power coupling while a GPU-bound load runs |
| `--field LOG... [--game NAME]` | import game sessions' frame-time logs (see *Field evidence*) |
| `--schedule-boot [--sessions N] [--budget MIN] [--scene NAME]`, `--cancel-boot` | calibration at the next boot |
| `--status` | boot calibration status (JSON) |
| `@dev` (with `--only`) | the device power-state rows (DEV phase, battery only) |

Needs root, a clean state (no active Optimizations preset) and the tune lock;
Ctrl-C at any point restores everything and keeps what was measured. For
device-level power, run once on battery (on AC the power figure is the CPU
package only).

Run it again to add evidence: rows accumulate, and the next design starts where
the doubt is. Offline check of the design against a noisy synthetic machine
(same number of runs, same decision rule, net utility): `cargo test -p
centurion-helpers --bin centurion-calibrate sequential`; the noise/budget sweep is
`cargo test --release -p centurion-helpers --bin centurion-calibrate sweep -- --ignored --nocapture`.

## Settings added with benchmark set 5

| knob | how it is decided |
|------|------|
| `vm.percpu_pagelist_high_fraction` | measured (idle and loaded): 0 = kernel-sized per-CPU free lists, 8 / 32 / 128 = pinned. 1–7 are refused by the kernel and by the audit |
| `vm.extfrag_threshold` | measured in the load phase (fragmented free memory): 250 / 500 / 750 |
| `kernel.nmi_watchdog` | scored: off saves a performance counter per CPU and its NMIs, and costs the hard-lockup trace. The estimate alone never clears the margin; a measured idle-power gain can. `nowatchdog` / `nmi_watchdog=` on the command line wins. The soft-lockup detector (`kernel.watchdog`) is still never turned off |
| `sched.feat_sis_util`, `sched.feat_ttwu_queue`, `sched.feat_cache_hot_buddy`, `sched.feat_wakeup_preemption` | measured only (no rule): wake-up search width, remote wake-ups by IPI or directly, buddies kept on their cache, batch behaviour |
| `blk.max_sectors_kb` | measured in the IO phase: ¼ and ½ of the live request size, jointly with read-ahead and the elevator |
| `clock.source` | structural: switched back to `tsc` when another clock is in use and the kernel still lists the TSC. Never benchmarked |
| launch block `timerslack_ns` | Gaming: 1 ns for the game's process tree (`centurion-gamemode RUN` / `WRAP`); in the GUI: Game launch → *Precise timers* |

## Memory and writeback

| knob | rule |
|------|------|
| `vm.dirty_bytes` / `dirty_background_bytes` | sustained write rate × window (1 s / 0.25 s by default; 0.25–2 s scored, calibrated in the IO phase), capped at 2 % of RAM and 1 GiB, floors 32 / 8 MiB, MiB-aligned. Rate: probe → `/sys/block/*/stat` → device class |
| THP group (`enabled`, `max_ptes_none`, khugepaged pace, mTHP sizes) | searched jointly. `always` costs footprint (a touched 2 MB range takes a whole huge page; the split shrinker only returns it under pressure). With any mTHP size on, `max_ptes_none` is only 0 or 511 (kernel 7.x). Pace never above 2× the default. A `transparent_hugepage=` boot parameter is left alone |
| `vm.watermark_scale_factor` | raised only with evidence (direct-reclaim share, allocstall, `kswapd_low_wmark_hit_quickly`); max 300 and 2 % of RAM / 1 GiB of headroom. That headroom leaves MemAvailable |
| `vm.watermark_boost_factor` | never above 15000. Note: boosting *frees* page cache after fragmentation events; it does not hold memory |
| `vm.min_free_kbytes` | never raised; a live value above 1 % of RAM / 256 MiB is repaired |
| `mm.lru_gen_min_ttl` | scored against OOM risk; with the default weights it stays off |
| `vm.compaction_proactiveness`, `vm.vfs_cache_pressure` | scored; at least one defrag path stays on with THP `always` |

## Storage

Without a signature: flash-only machines get `none`, a spinning disk
`bfq`/`mq-deadline`; read-ahead 256 KiB for gaming, 512 KiB (2 MiB on a
spinning disk) for throughput; writeback throttling is scored. With an IO-phase
signature every Storage row (`scheduler`, `wbt_lat_usec`, `read_ahead_kb`,
`rq_affinity`, `nomerges`, `iostats`, `add_random`, `nr_requests`) and the dirty
window are decided by measurement, jointly with their interactions (elevator ×
queue depth × throttling × dirty window), weighed by the storage weight.

## Devices

Runtime PM, ASPM, USB autosuspend, NVMe APST, HDA power save, Wi-Fi power save
and suspend mode are scored with a stability cost. Boot parameters win:
`usbcore.autosuspend=` → USB autosuspend is never touched; `pcie_aspm=force` →
deeper ASPM states carry a larger risk; `pcie_aspm=off` → no ASPM rows.
`kernel.watchdog` is never turned off (a hang would leave no log).

## Hard limits (audit)

`centurion-autotune audit` and tune-helper check every preset/scene/live value:

- **refused**: `dirty_background_bytes` < 1 MiB, `dirty_bytes` < 4 MiB or ≤ background,
  `watermark_scale_factor` > 1000, `min_free_kbytes` > 3 % of RAM, `vfs_cache_pressure` < 10
- **warned** (with a fix): `max_ptes_none` ≠ 0/511 with mTHP on, khugepaged faster than 2×,
  boost > 15000, reserves above 1 %, `zone_reclaim_mode` on a single node, …

tune-helper never stores or writes a refused value. Old presets/scenes (e.g. the
int32-wrapped `dirty_bytes` 8192 / 290489958 from earlier GUI versions):

    centurion-autotune audit            # list
    centurion-autotune audit --fix      # repair (keeps *.json.bak), then re-save scenes in the GUI

## Pressure guard

After an apply that wrote memory/writeback knobs, a detached helper samples PSI
once a second for 120 s. If `io full` stays ≥ max(2×before + 10, 25) % for 15 s,
`memory full` ≥ max(before + 5, 10) % for 10 s, or allocation stalls exceed
500/s for 10 s, it restores exactly those knobs, writes
`/run/centurion/tune/guard.json` and a kernel-log warning (visible
in the Health tab). It stops early when anything else changes those knobs.

## Tools

    sudo centurion-autotune probe       # ≤ 512 MiB / ≤ 4 s O_DIRECT write into an unlinked temp file
    centurion-autotune report [SEC]     # meminfo, watermarks, THP/writeback settings, PSI, vmstat deltas
    centurion-autotune <goal> --json    # full output incl. per-knob scores and constraint notes

Baseline entries whose original came from TLP are listed as `tlp_originals` in
the helper state: "restore" returns them to TLP's state, not the kernel default.
