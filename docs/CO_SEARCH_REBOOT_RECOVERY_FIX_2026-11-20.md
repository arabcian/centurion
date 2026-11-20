# CO Search — reboot-loop recovery fix (20 November 2026)

## Incident

A user reported an isolated CO search repeatedly rebooting at **Core 9, CO -25**, even after choosing Resume four times. The previous implementation wrote the strategy **before** `Search::next()`, replayed `next()` after loading, and then tried to report the regenerated candidate as failed. It had **no durable cross-session blacklist**. Exact reproduction of the original repeated -25 incident would require the original user journal/boot logs; the source alone does not establish why all four attempts repeated the offset.

## Fix

- Journal **v3** contains the state **after** `Search::next()`, a `pending` flag and exact trial signature (`what`, all/per-core offsets and seconds). Resume uses `Search::inFlight()`, validates the signature, and reports the **actual in-flight candidate** as interrupted rather than executing `next()` to guess it.
- A pending trial found on recovery is deemed unsafe, even if the machine merely lost its GUI process. Do not misattribute this as proof of a CPU defect: it is a precautionary classification.
- A per-core interrupt at **-25** records a durable **crash guard** for that physical CCD/core. At -25, -25 and -24 are excluded; the deepest permitted subsequent candidate is **-23**. This is not automatically accepted as stable: final confirmation still must pass. Interruption at -15 also backs off toward zero; it cannot rely on -15 being safe.
- The guard is stored separately in `$XDG_STATE_HOME/centurion/co-search-crash-guards.json` (`~/.local/state/centurion/...` when XDG_STATE_HOME is not set) and persists when the old search journal is discarded. It is associated with a physical CCD/core, so logical CPU renumbering does not silently remove its protection.
- **Important:** Guard persistence happens *before* the Resume / Discard question; choosing Discard cannot silently permit the same crashing value in a new search. A damaged or unreadable guard file blocks new searches rather than being interpreted as no restrictions.
- The journal stays in `pending` state **after the test returns** until a new trial has its own atomically committed journal or the CPU stock-reset completes. This covers a late reboot while the previous candidate is still physically applied.
- A successful recovery atomically stores its **already resolved** state before proceeding to privileged CPU writes. A second pending-trial reboot in the same search locks automatic resume. The recorded guard remains available for manual fresh searches after stock-reset and safety checks.
- In combined confirmation with an unattributable reboot, the in-progress search backs off all negative cores by **two** CO points before attempting confirmation again; this is not proof of the root cause. It does not permanently quarantine a specific core without isolated evidence.
- Legacy v2 journals are accepted only when the replayed pre-next() `what` exactly matches the recorded interrupted candidate. Mismatch blocks recovery without writing CO offsets.
- An abnormally terminated stress worker is also treated as an unsafe candidate with the two-point recovery margin and a persisted per-core guard when isolated.
- Application close with a confirmed stock reset marks the interrupted trial as failed before retaining a resolved recovery journal. A failed stock reset never deletes the recovery journal. If resetting or journaling fails, no new offset is applied.

## How to use this update

1. **Do not press Resume in the old Centurion build.** Make sure the Ryzen CO is set to stock; ensure any boot scene/profile does not reapply an unsafe curve. Do not begin another hardware search while the machine is still rebooting.
2. Install the updated full source/GUI. If you kept the legacy `co-search-session.json`, open the new GUI and inspect the trial that it identifies. The patch quarantines an identifiable pending CO value before giving you the discard/resume choice.
3. After multiple incidents, prefer **Discard** rather than immediately running another search; the crash guard survives the discarded journal. A new search does not automatically claim that any value is stable.
4. **Forensic state paths** (read-only inspection):

   ```bash
   STATE="${XDG_STATE_HOME:-$HOME/.local/state}/centurion"
   [ -f "$STATE/co-search-session.json" ] && cat "$STATE/co-search-session.json"
   [ -f "$STATE/co-search-crash-guards.json" ] && cat "$STATE/co-search-crash-guards.json"
   [ -f "$STATE/co-search-runs.json" ] && cat "$STATE/co-search-runs.json"
   ```

5. The patch does not change CPU firmware/SMU thermal protection. No arbitrary CPU temperature stop limit was reintroduced. An abrupt reboot can also stem from RAM, firmware, power management, other OC settings or power-delivery issues; the guard conservatively forbids a candidate *associated* with the crash, it cannot prove causality.

## Validation in this environment

- `python3 gui/tests/co_walk_native_test.py`: 1,311 candidate/limit combinations, actual `Walk::coreCandidate` compiled with native g++.
- `python3 gui/tests/co_reboot_native_test.py`: 7,165 synthetic reboot scenarios + 8,567 no-crash scenarios, actual candidate function compiled with native g++; no prohibited post-crash candidate is repeated.
- `gui/tests/costrategy_test.cpp`: adds Qt6-linked simulations for pending snapshots, Core 9 / -25 reboot recovery, future crash guard and combined-confirmation backoff. **These Qt tests are added but not executable here because Qt6 headers and development libraries are unavailable.**
- Existing GUI static/i18n Python checks pass. **No Ryzen hardware undervolting tests, Qt6 full build or Rust build were performed in this environment.**

This is a safety/recovery patch, not a guarantee that an undervolted Ryzen will be stable. After a reboot incident, save unsaved work, check firmware and power-scene configuration and run stability testing only if the system is healthy.
