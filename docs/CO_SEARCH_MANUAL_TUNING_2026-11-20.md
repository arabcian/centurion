# CO Search: configurable, adaptive per-core probing

> **Superseded in part** by `CO_SEARCH_PERCORE_RULES_2026-11-20.md` (per-core search rules, crash guard, reboot lockout).

**Based on:** `Centurion_CO_Search_RebootLoop_Fix.zip`, 2026-11-20. Includes the prior reboot-loop protections, crash-guard persistence and recovery journal logic.

## Operator controls

The **Per-core controls** row in Ryzen CO Search now contains three editable spin boxes:

| Setting | Default | Range | Meaning |
|---|---:|---:|---|
| Start | -15 | -50..-1 | First CO candidate of each not-yet-tested core, clamped to the configured search limit and recorded crash guards |
| Coarse step | 5 | 1..20 | Descend by this many CO points after a passing coarse candidate |
| After failure | 3 | 1..10 | Refinement quantum after a failed probe |

This row is disabled in **all-core** mode (whose original search method is unchanged). Changes are persisted in the user's existing CO Search settings file. No firmware/SMU temperature or power settings are changed by the new controls.

To adjust an **ongoing per-core** search, modify the three fields and click **Apply to next trials** while a trial is active. The running candidate remains unchanged. Only future probes use the new parameters, and values already accepted/rejected and crash guards remain intact. The button is disabled during confirmation and when a trial is not pending, so it cannot overwrite a pending on-disk recovery record during a write/reset gap.

## Adaptive strategy

- For every core, try the chosen starting value (subject to the search limit and crash exclusions), with all other cores at stock.
- After a passing coarse probe, try `max(limit, last_pass - coarse_step)` provided it does not cross a known failure or guard.
- After a **failed** probe, keep `last_pass > first_failed` as a bracket. Its first refinement is at most the chosen refinement quantum (default **3 CO points**) when the bracket is narrow; for a wide bracket it chooses a multiple of that quantum near its midpoint to avoid unnecessary trials.
- For a default quantum of 3, as the interval narrows, use 3, then 2, then 1 CO point. No candidate at or below an observed failed boundary is reused, and the loop ends when no untried integer between passing/failing bounds remains.
- If the **initial** candidate fails, stock CO 0 remains the conservative passing-side boundary. The algorithm searches toward zero; it never assumes -15 is universally safe.

**Illustration:** With Start=-15, Coarse=5, After failure=3 and Limit=-30, a core might test -15 (pass), -20 (pass), -25 (fail), -23 (fail), -22 (fail), -21 (pass). The result of isolated probing is then tested at full duration, both per-core and as a combined set. These are hypothetical outcomes, not stability promises.

**Wide bracket illustration:** With a jump of 20 and a 3-point quantum, an observed 20-point pass/fail interval is first refined with a **9-point** (3×3) jump, then with smaller multiples and 2/1-point probes as needed.

## Reboot and persistence safety

- The strategy snapshot includes `tuning` (`initial`, `coarse_step`, `refine_step`) and is now **strategy version 3**. The recovery journal remains **journal version 3** and retains the active candidate's signature as before.
- Old strategy version 2 snapshots load with safe default tuning. Invalid version 3 tuning is refused rather than guessed.
- A tune change during an active trial uses an atomic journal write; a write failure restores the old values. `Search::inFlight()` still reconstructs the **same** candidate; changing step sizes does not make an in-flight test replay differently.
- Persistent guards remain independent of the operator controls. For example, an interruption at **-25** protects -25 and -24; no user start value, descent stride or resumed search can cause either to be re-tested on the guarded physical core.
- The previous maximum-two-suspected-reboots lockout remains in place. There is **no custom temperature cutoff**; a critical platform/firmware thermal emergency is still a stop condition.

## Validation

In the build environment:

- `python3 gui/tests/co_walk_native_test.py` — **11,799** stable-boundary cases using the **real C++** candidate function (9 tuning sets, all supported limits, all integer thresholds); 84,385 probes with no duplicates or over-limit values.
- `python3 gui/tests/co_reboot_native_test.py` — **49,719 simulated crash cases** and **44,673 clean cases** with runtime tuning changes; no protected offset was replayed.
- Other static GUI/localization checks and 5 Turkish catalogue tests passed.
- The Qt6-linked `centurion-costrategy-test` includes manual tuning, journal version transition, in-flight snapshots and 3->2->1 regressions, but **Qt6 development headers and libraries are not available here**, so this test and full GUI build could not be run in this environment. No Ryzen SMU writes/hardware stability tests were performed.

On a Gentoo installation with Qt6 and the full Centurion build dependencies:

```bash
cmake -S gui -B build/gui -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/gui -j"$(nproc)"
ctest --test-dir build/gui --output-on-failure
```

After a system reboot incident, start from explicitly reset stock CO and verify platform stability; take a backup of the existing CO Search state/journal before testing a new build. An abrupt reset is not diagnostic proof of a particular CPU core's silicon stability, and short stress tests are not exhaustive certification.
