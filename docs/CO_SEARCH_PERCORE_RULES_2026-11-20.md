# CO Search, per-core: search rules (20 November 2026)

Supersedes the per-core algorithm of `CO_SEARCH_MANUAL_TUNING_2026-11-20.md` (coarse / refine steps) and the
reboot lockout and safety margin of `CO_SEARCH_REBOOT_RECOVERY_FIX_2026-11-20.md`. Journal, signature, stock-reset
and fault-detection behaviour from those documents is unchanged.

## Rules

1. One core is probed at a time; every other core is at stock (0). Cores take turns, alternating CCDs, so a
   stable core keeps going down while another one is already finished.
2. Every core has its own **Start** (the table's Start column: typed in, default -15, or filled from a saved
   **Preset**, see below), clamped to the limit and any crash guard; 0 means one step below stock. After each
   pass the core goes **Step** points more negative (default 1).
3. **The first failure after a pass ends the core.** Its result is the last value that passed. Nothing as negative
   as the failed value, and nothing between the last pass and the failure, is tried on that core again.
4. If the Start fails (nothing passed yet), the core **climbs back towards stock** by Step from the failure
   (Start + Step, Start + 2·Step, …). The first value that passes is its result; reaching 0 ends it at stock.
   Climbing only ever tests values less negative than every failure.
5. **Crash = failure, recorded for good.** If the machine reboots or freezes, or the stress worker dies, while a
   core is probed, that core and value go to `$XDG_STATE_HOME/centurion/co-search-crash-guards.json` before the
   resume / discard question. That value and everything below it are never applied to that physical core
   (CCD / slot) again, in this or any later search. A computational error (wrong result, machine-check) ends the
   core in this search only, so a later search with another scene is not limited by it. So does the application
   dying while the machine keeps running (same boot): that is not a crash of the machine. Quitting the application
   during a trial (after its confirmed stock reset) is no failure at all: a resume runs that trial again.
6. **Unticked cores** (Core column checkboxes, per-core mode) stay at stock in every probe, in the confirmation and
   in the saved profile. The selection is remembered in the CO Search settings.
7. When every core is done, the whole set gets the **20-minute verification** (below). A failure backs the named
   core(s) off by 1 (2 after a crash), or every undervolted core when no core can be named, and the verification
   runs again. A crash during it has no single culprit and is not stored as a per-core guard.

## Presets

*Preset* lists the saved Ryzen Curve profiles (`~/.config/ryzen-curve-optimizer/profiles`, including earlier CO
Search results). Choosing one fills the Start column: a core's per-core value; the profile's all-core value for a
core without one; 0 when there is neither. Slots disabled in the profile are unticked; positive values start at 0;
values past the limit start at the limit (logged at the start). Editing a Start afterwards switches Preset back to
*manual*. So a saved profile — even with different values on every core — is tested core by core and pushed
further with the given step.

## All-core start

The global start moved to all-core (*All-core start*, default -5 = the previous fixed behaviour). All-core still
descends by 5 and by 1 after a failure; a failed start keeps the steps of 5 from stock down to it.

## 20-minute verification (per-core)

The whole found curve is applied for every part; each part stops at the first wrong result, and kernel / MCA
events are checked as for every trial. Light and heavy parts alternate, so single cores are also tested on a hot
package and right after large current steps.

| Part | Command | 16 cores | Why |
|---|---|---|---|
| 1 | `percore --load co --seconds 24 --kernel 0`, each undervolted core in turn | 384 s | One busy core: the shared voltage rail follows that core's request alone, at its highest boost, with the lightest kernel (FFT-SSE) — where CO offsets fail first — plus all ten boost/idle patterns. First, as it fails fastest. |
| 2 | `allcore --load power --seconds 180` | 180 s | Every thread at the package power limit: highest current, voltage droop, heat. |
| 3 | `allcore --load co --cycles 2 --kernel 1` | 120 s | Right after it, on a hot CPU: FFT-AVX then NTT, current steps, a changing number of busy cores, a lone core boosting in turn. |
| 4 | `percore --load co --seconds 24 --cycles N --kernel 2` | 384 s | More single-core cycles with the other kernels (NTT's integer multiplier, then FFT-AVX, …). |
| 5 | `percore --load sse --seconds S --transitions` | – | Only if at least 5 s per core are left: different code with random 1–200 ms idle pauses. |
| 6 | `allcore --load power --seconds R` | 132 s | Power limit again, for the rest of the 20 minutes (at least 60 s). |

Only undervolted cores are loaded alone. The parts add up to 1,200 s; with more than 35 undervolted cores parts
1, 2, 3 and 6 alone exceed it and the verification is that long. With 8 cores part 4 runs 3 cycles and part 5
fits (9 s per core).

### Why the earlier test stayed near 70 W

Most of it is single-core by design (tens of watts), and its all-core kernels wait on dependency chains and on
memory (the FFT's hash / carry steps, buffers up to 1 MiB; the classic AVX loads' integer chain and memory walk),
so the FMA pipes are mostly idle and the package never reaches its power limit. CO offsets do fail most at light
loads, but the high-current, hot region of the curve was not tested.

### The `power` load (`centurion-cpustress --load power`)

The logistic map on 10 independent 256-bit vectors (AVX2 + FMA) or 16 512-bit vectors (AVX-512), with no memory
work: each step is an fnmadd and a multiply, and there are enough independent vectors to keep both FMA pipes busy
every cycle. Measured in the build environment (a Xeon, one thread): 33.4 GFLOP/s against 16.8 for the existing
`avx512` load; it scales with the threads. Watts could not be measured there.

Error detection: the map is chaotic, but in floating point it is many-to-one near x = 0.5, so a one-ulp error
can round away and two trajectories merge again (seen in testing: merged two steps after the error). Each
vector's raw bits are therefore added to integer accumulators before every step, and the accumulators are stirred
once per step by an invertible xorshift-multiply, so that a +1 / −1 pair of differences cannot cancel either. A
test flips one low bit at 49 positions × 8 seeds × both widths, including the very last step; every flip changes
the result. With the `fault-inject` build an all-core `power` run reports `fail` and names the CPU.

The GUI asks `centurion-cpustress info` before a search and refuses to start with a binary that has no `power`
load (reinstall both together).

## Crash watcher: which core the logs blame (`gui/src/coevidence.{h,cpp}`)

Evidence is collected from three places:

1. **Live, during every trial** (every 3 s): the MCA threshold counters in sysfs, which move the moment a corrected
   error happens (the kernel itself logs corrected errors only at its next poll, up to 5 minutes later — too late
   before a crash), and the classified kernel events (health-reader). A new record is written to the recovery
   journal at once (`evidence`), so it survives a reset a moment later. The journal also keeps `active_cpu`, the
   CPU the single-core parts last loaded.
2. **Early in the next boot** (first 120 s): machine checks the kernel finds left in the MCA banks by the reset.
   Used only if the boot history confirms that the previous boot is the crashed one (the boot guard's history);
   otherwise skipped, and the summary says so.
3. **pstore** records the boot history attributes to the crashed boot (the last lines of a kernel panic), if the
   kernel keeps them.

Recognised: the kernel's `CPU N: Machine Check…: … Bank B` and the AMD decoder's `CPU:N (…) MCB_STATUS[…]`
records, the threshold counter paths `machinecheckN/<unit>/…/error_count`, and soft / hard lockups.

**False-positive protection** (`judge`):

- Only machine checks of a core's own units count: banks 0–6 on Zen (load/store, instruction fetch, L2, decode,
  execution, floating point) or those counter unit names. A record from a shared unit (L3, memory controller,
  fabric) points at RAM / fabric as much as at a core: **nobody** is blamed.
- Records naming more than one core: ambiguous, nobody. A record naming a core that was at stock contradicts the
  Curve Optimizer as the cause: nobody. Unknown CPUs are ignored.
- Lockups / oopses name the CPU that noticed a problem, not the one that caused it: shown, never used.
- A blame is used only in the final verification (every core undervolted): that core alone backs off by 2
  instead of every undervolted core. It is **believed once per search**: if the set crashes again, the logs missed
  the real culprit and every undervolted core backs off (`blamed` in the snapshot).
- Log evidence never goes into the persistent crash guard: that stays reserved for single-core probes, where the
  core under test is known without any log. In a single-core probe the verdict is only reported.

The resume question and the log show the verdict ("System logs: …").

## Saving the result

*Save result as profile…* saves the Result column under a chosen name as a Ryzen Curve profile (listed in Ryzen
Curve and in this dialog's Preset). *Use result as Start* copies it into the Start column for the next search.
Both work after a finished search, after Stop, and after discarding an interrupted search on recovery (its values
stay in the table); a search that did not finish its verification is named and logged as *not verified*. The
automatic "CO search <date>" profile of a finished search is unchanged.

## Recovery changes

- Discarding a journal of the current boot (the application died, the machine did not) resets the curve to stock
  first, since the trial's offset may still be applied; the journal is removed only after a confirmed reset. A
  journal of an earlier boot is removed without a write (a restart already brought the SMU back to stock).
- CO Search runs on AC power only: it is not started on battery, and unplugging the charger stops it with a stock
  reset. On battery the firmware's lower power limits would give results that need not hold on AC, and a battery
  running out would end a trial like a crash.
- The two-reboot lockout is removed. In a per-core search a crash at each core's edge is an expected outcome, and
  the lockout stopped the search after two cores. A reboot loop cannot form: each crashed value is guarded before
  anything else happens, so a resume cannot apply it again. Each resume still asks the user, and shows how many
  suspected reboots the search has had.
- No extra +1 margin after a crash: the crashed value and everything below it are forbidden, the core keeps its
  last pass (which had passed its test).
- A journal that cannot be resumed (damaged, older format, candidate / state mismatch, missing pending marker) now
  offers **Discard**. Before, these cases returned without one, and every later *Start search* reopened the same
  message, so the search could never be started again.
- Strategy snapshots are version 5 (`tuning: {all_core_start, step}`; per core `start`, `up`, `excluded`).
  Versions 3 and 4 still resume: their single per-core start becomes every core's start; a core that had failed
  after a pass keeps that pass; a failed start climbs from now on. Version 2 is refused (with the discard offer).
  A search interrupted during the old per-core confirmation cannot be matched to the new verification and offers
  discard.

## Tests

`gui/tests/costrategy_test.cpp` (ctest `costrategy`) now runs the real `Search` code over every combination of
6 start values × 5 steps × 4 limits × every threshold (3,420 walks), and 3,960 crash-guard walks, plus climbing,
all-core start, preset-style per-core starts and the verification's composition for 2–50 cores. It checks that a
core only ever goes more negative, is never probed after a failure that followed a pass, never at or below a
failure or a guard, ends at its last pass, and with step 1 lands exactly on the threshold. The previous
`co_walk_native_test.py` / `co_reboot_native_test.py` re-implemented `report()` by hand and tested that copy of
the old refinement; they are removed. The Qt test from the previous build failed (`costrategy_test.cpp:361`) and
had never been run; it passes now.

Not tested here: SMU writes on Ryzen hardware. Check on the machine with stock CO and saved work first.
