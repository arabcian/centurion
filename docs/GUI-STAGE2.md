# GUI overhaul — stage 2 of 4, hardware tuning increment

Baseline: `centurion-GUI-stage1.zip` (all earlier fixes retained).

## Implemented in this increment
- CO Search: completed-core progress (per-core search only), less crowded action rows, searchable trial-history snapshot with timestamps, physical core identity when known, candidate, outcome and available reboot evidence. Recovery prompt shows the interrupted candidate explicitly (not a prediction of the next candidate). Recovered isolated attempts carry core/value metadata; discarding a journal reloads its history rather than displaying another session. Persistent crash guards, journal version, search strategy and stress decisions are unchanged.
- NVIDIA V/F curve: independent dashed base reference, dotted historical driver readback and solid draft; each reference can be toggled. Axis fitting accounts for visible reference curves; font-aware plot margins. Point comparison shows voltage, calculated reference/draft, cached driver frequency and draft offset, with snapshot timestamp. The base can be reconstructed; it is not guaranteed factory stock. Historical readback is not live telemetry and may predate an operation that failed to return a fresh curve. Driver log no longer has a fixed 52-pixel height. Existing Apply, profile, flatten and safety/helper paths remain unchanged.
- Fan Curve: historical EC table overlaid on the editable draft, in addition to the existing temperature/RPM table. No new fan control algorithm or automatic writes.
- Firmware: searchable read-only parameter inspector with last-known value, draft, device, supported range, default where available, and access status. WMI readback not verified is shown as unavailable; snapshot is not live. Status text wraps.
- AMD GPU: read-only capability matrix derived from the controls that the actual selected card exposes in its editor. This is not proof that a write would succeed.
- Intel: font/size-hint-aware voltage, TCC, ratio and related input widths.
- Autotune: report separated into Summary & Evidence, Recommendations, and Predictions & Trade-offs; searchable recommendations, changed-or-unverified filter, visible count, resizable rationale/candidate-score inspector, separate scrollable long sections. Model estimates labeled separately from observed results. The weight editor displays each configured weight as a fraction of its own legal maximum without modifying its legal range or default behavior.
- Explore correctness: the report now reads proposed values from the Explore result instead of the stale optimization editor. No preset-save or game-default action in Explore; user may rerun Autotune with alternative weights. Unknown or stale live values are not presented as matches. No ABBA, tuning-helper or calibration model changes.
- Turkish localization: 85 new messages, including placeholders. Six source-contract regression tests.

## Verification and limitations
- 19 of 22 non-native Python test scripts passed (including Stage 1 and Stage 2 source contracts). Three pre-existing failing suites remain: advanced_17_static_test.py, test_evidence_lab_static.py, test_session_audit_static.py. Stage 2 source checks: 6/6; translation tests: 5/5; title-case test passed.
- CMake configuration attempted with `CENTURION_BUILD_TESTS=ON`: Qt 6.4+ development configuration is absent in this environment. Native Qt compilation, CTest, CO native tests, visual/HiDPI validation and physical hardware validation have NOT been completed. Static tests are not substitutes for native execution.
- No Rust files, privilege policy, hardware-writing helpers, CO strategy, fan safety floor, GPU voltage/frequency arithmetic or autotune solver were modified. There is no automatic hardware application from the new inspectors.

## Remaining within stage 2
This is an incremental implementation, not completion of every idea GUI-43–93 and GUI-112–127. Full CCD heatmap, session archive browser, profile comparisons, ETA refinements, NVIDIA undo/redo and post-apply transaction report, Intel domain/topology dashboard, advanced AMD frequency chart, firmware power hierarchy, calibration data manager and measured A/B/frametime workspace remain. Stage 1 outstanding items also remain open (see GUI-STAGE1.md). Stages 3 and 4 remain pending.

## Local verification
```
cmake -S gui -B build-gui -G Ninja -DCENTURION_BUILD_TESTS=ON
cmake --build build-gui -j$(nproc)
QT_QPA_PLATFORM=offscreen ctest --test-dir build-gui --output-on-failure
```
First open each inspector without applying anything. Check historical vs draft labels, unavailable WMI readback, keyboard navigation, Turkish and small-window behavior. In Autotune, run Explore and verify the main table stays unchanged and preset save is absent. Check that changing weights retains existing numeric limits. In CO Search, inspect recorded attempts without starting a stress run solely for UI verification. Do not induce reboot or use unsafe values to exercise recovery.
