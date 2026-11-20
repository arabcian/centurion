# CO Search, per-core: search rules (20 November 2026)

Supersedes the per-core algorithm of `CO_SEARCH_MANUAL_TUNING_2026-11-20.md` (coarse / refine steps) and the
reboot lockout and safety margin of `CO_SEARCH_REBOOT_RECOVERY_FIX_2026-11-20.md`. Journal, signature, stock-reset
and fault-detection behaviour from those documents is unchanged.

## Rules

1. One core is probed at a time; every other core is at stock (0). Cores take turns, alternating CCDs, so a
   stable core keeps going down while another one is already finished.
2. A core's first probe is the **Start** value (default -15, clamped to the limit and any crash guard). After each
   pass it goes **Step** points more negative (default 1).
3. **The first failure after a pass ends the core.** Its result is the last value that passed. Nothing as negative
   as the failed value, and nothing between the last pass and the failure, is tried on that core again.
4. If the very first probe fails before anything passed, the core descends from stock (-Step, -2·Step, …), never
   reaching the failed value. A failure there also ends the core (result: last pass, possibly 0).
5. **Crash = failure, recorded for good.** If the machine reboots or freezes, or the stress worker dies, while a
   core is probed, that core and value go to `$XDG_STATE_HOME/centurion/co-search-crash-guards.json` before the
   resume / discard question. That value and everything below it are never applied to that physical core
   (CCD / slot) again, in this or any later search. A computational error (wrong result, machine-check) ends the
   core in this search only, so a later search with another scene is not limited by it.
6. **Unticked cores** (Core column checkboxes, per-core mode) stay at stock in every probe, in the confirmation and
   in the saved profile. The selection is remembered in the CO Search settings.
7. Unchanged: full-length confirmation per core and with all cores; a confirmation failure backs the named core(s)
   off by 1 (2 after a crash), or every undervolted core when no core can be named. A crash during the confirmation
   has no single culprit and is not stored as a per-core guard.

Example, Start -15, Step 1, a core stable down to -22: -15, -16, … -22 pass, -23 fails → result -22, the core is
done. With Step 5: -15, -20 pass, -25 fails → result -20 (up to Step − 1 points more conservative).

## Recovery changes

- The two-reboot lockout is removed. In a per-core search a crash at each core's edge is an expected outcome, and
  the lockout stopped the search after two cores. A reboot loop cannot form: each crashed value is guarded before
  anything else happens, so a resume cannot apply it again. Each resume still asks the user, and shows how many
  suspected reboots the search has had.
- No extra +1 margin after a crash: the crashed value and everything below it are forbidden, the core keeps its
  last pass (which had passed its test).
- A journal that cannot be resumed (damaged, older format, candidate / state mismatch, missing pending marker) now
  offers **Discard**. Before, these cases returned without one, and every later *Start search* reopened the same
  message, so the search could never be started again.
- Strategy snapshots are version 4 (`tuning: {initial, step}`, `excluded` per core). Version 3 snapshots from the
  previous build still resume: a core that had failed after a pass keeps that pass; the step becomes 1. Version 2
  is refused (with the discard offer).

## Tests

`gui/tests/costrategy_test.cpp` (ctest `costrategy`) now runs the real `Search` code over every combination of
5 start values × 5 steps × 4 limits × every threshold (2,850 walks), and 3,960 crash-guard walks. It checks that a
core only ever goes more negative, is never probed after a failure that followed a pass, never at or below a
failure or a guard, ends at its last pass, and with step 1 lands exactly on the threshold. The previous
`co_walk_native_test.py` / `co_reboot_native_test.py` re-implemented `report()` by hand and tested that copy of
the old refinement; they are removed. The Qt test from the previous build failed (`costrategy_test.cpp:361`) and
had never been run; it passes now.

Not tested here: SMU writes on Ryzen hardware. Check on the machine with stock CO and saved work first.
