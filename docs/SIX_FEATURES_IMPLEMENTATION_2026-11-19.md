# Centurion – six-feature implementation notes (2026-11-19)

Source base: `centurion-main(20261119-120600).zip` (user supplied). New code is in the Qt6 GUI and reuses the existing, unchanged privileged Rust helpers.

## UI locations and source files

| Feature | Location | Implementation |
|---|---|---|
| **Hardware Incident Timeline** | Health → Events & alerts → select event → **Incident Timeline…**, also the right-click menu | `gui/src/insights.cpp`, `gui/src/healthtab.cpp` |
| **SC-02 Transactional Scenes** | Scenes → Apply (the existing scene engine) | `gui/src/scenes.h`, `gui/src/scenes.cpp` |
| **AT-04 Calibration Confidence Explorer** | Optimizations → Autotune → open report → **Calibration Confidence Explorer…** | `gui/src/insights.cpp`, `gui/src/optimizetab.cpp` |
| **PW-02 Fan/Temperature Pareto Lab** | Health → Cooling → **Pareto Lab…** | `gui/src/insights.cpp`, `gui/src/coolingpage.cpp` |
| **UV-01 Ryzen CO Search Assistant** | Ryzen Curve → **CO Search…** | `gui/src/cosearchdialog.cpp`, `.h`, `gui/src/ryzentab.cpp` |
| **HL-03 Capability Doctor** | Health → Problems → **Capability Doctor** | `gui/src/insights.cpp`, `gui/src/healthtab.cpp` |

## Behavior and limits

**Incident Timeline:** Event timestamps are correlated with curve changes in `oc-journal.jsonl`, boot-matched samples in `flight.ring` / `flight-prev.ring`, classified Health errors and new scene transaction history. The timeline explicitly avoids claiming causation from temporal proximity. Old archived Health records retain only an **8-character** boot-ID prefix, and cannot be safely associated with a full boot-specific ring or transaction; they still show the archived event and its wall time. The report is read-only, and NVIDIA state is read without starting NVML.

**SC-02:** At the first unsuccessful application step that may have written something, remaining work stops and captured writes are compensated in **reverse order**. A step that fails before writing anything (no snapshot, power profile not Custom, NVIDIA tab busy) is a problem line only: the remaining steps run and nothing is rolled back. Snapshot-supported components: platform profile; EC full-speed fan flag; EC fan table; directly readable firmware sysfs limits. The old values are captured before writes. Non-reversible actions are **not** silently represented as rolled back: command execution, optimization presets, CPU/GPU curves, WMI-only firmware limits, lighting and display changes require manual verification. Compensating writes can themselves fail. A game taking ownership during an automatic scene switch is not overwritten by rollback. Events are written to the user-owned `$XDG_STATE_HOME/centurion/scene-transactions.jsonl` (default `~/.local/state/centurion/…`, rotated above 512 KiB). The log includes component names and failure notes, but not user command contents.

**AT-04:** Shows measured/estimated information from the existing Autotune `decisions` JSON, sorts unobserved or uncertain decisions first. Reports no artificial confidence for old reports lacking evidence; no tuning writes.

**PW-02:** Uses existing `cooling.json` history. It compares clean records only if they have the same firmware test context `key` and duration `secs`, and warns or refuses comparison when starting temperatures are too different (>3 °C). Objectives: lower RPM, lower stabilized die temperature, higher sustained clock; Pareto dominance is directional. No real dB or gaming frametime measurement is asserted; no fan writes.

**UV-01:** Superseded by the CO Search rewrite: all-core or per-core search with 60 s `centurion-cpustress --load co` cycles, fast / medium / long levels, a CO search scene and temperature limit (default 95 °C), crash-resume journal and the result saved as a profile (see docs/TECHNICAL.md, "CO cycle and CO Search"). Kept from the original: explicit stock precondition, `health-reader` kernel-event scan and live MCA counters after every trial, SIGTERM/SIGKILL watchdog, stock reset on every end (blocking reset when the application quits), no automatic commit.

**HL-03:** Read-only checks for kernel/firmware interfaces and required Centurion tools. A missing indicator means *not visible*, not proof that hardware lacks support. It does not wake the dGPU or write to hardware.

## Build and tests – Gentoo

Requires Rust toolchain with Cargo, CMake, C++20, Qt6 >=6.4 (`Widgets`, `Network`, `Svg`, `DBus`), and the project's ordinary helper installation.

```bash
# From the project root; non-hardware source checks, no special permissions
python3 gui/tests/six_features_static_test.py
python3 gui/tests/health_ui_static_test.py
python3 gui/tests/experimental_layout_static_test.py
python3 gui/tests/experimental_ui_static_test.py
python3 gui/tests/studio_v4_static_test.py
python3 gui/tests/translation_tr_test.py

# Compiles all GUI targets and the additional CTest suite on a fully provisioned Gentoo host
cmake -S gui -B gui/build -DCENTURION_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build gui/build --parallel
ctest --test-dir gui/build --output-on-failure

# Existing privileged helper tests – no hardware calls in tests
cargo test --workspace
```

**Local verification in the analysis container**: the five existing/static GUI checks plus the new 22 static source checks pass; 5 Python translation tests pass. CMake configuration stops at `find_package(Qt6)` because the Qt6 development SDK is absent. Cargo and Rust toolchain are also absent. Thus the C++ tests were **added but not compiled/executed** here. Runtime operation on a real Lenovo/Ryzen/NVIDIA system is **not verified**. Treat this source archive as an integration candidate, not a certified working binary.

## Guided hardware smoke test

1. Back up `$XDG_CONFIG_HOME/centurion`, `$XDG_STATE_HOME/centurion` (with their default home-directory fallbacks), and any custom CO/firmware settings.
2. Open Capability Doctor and Incident Timeline on a known event. Inspect boot-ID and unavailable-data messages. Test a prior-boot event to confirm it does not use current sensor values.
3. Open an Autotune report with `decisions`, and Pareto Lab after at least two comparable cooling runs; verify that tests from different firmware contexts do **not** get grouped.
4. Apply a trivial scene and then a scene with a deliberately invalid *non-destructive* final step. Confirm the remaining steps stop, compensating steps run in reverse and `scene-transactions.jsonl` reports the partial/complete state accurately. **Do not** use an unsafe firmware limit or custom command to simulate failure.
5. For CO Search, use Ryzen Curve → Reset first, end game and calibration sessions, keep AC power and cooling stable, then run a *fast* all-core search with a small limit (-10). Confirm that it returns to stock, records the reset result and saves the profile.
6. For every failed reset or partial scene rollback, review the relevant hardware setting before continuing normal tuning.
