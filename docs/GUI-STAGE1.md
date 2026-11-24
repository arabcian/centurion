# GUI overhaul — stage 1 of 4 (foundation increment)

Baseline: archive commit `6eb741e1715229aa11fb010d531a1ed9aec81087`.

## Implemented
- Optimizations and the Network/Storage/Kernel Tuning pages: independent live value, boot guard snapshot, saved boot value, editable draft and textual state columns. Snapshot is not represented as a factory or restore-original value. Unreadable, stale and cached data cannot count as a verified boot match.
- Per-section filters: all, selected, edited, boot mismatch, saved at boot, unavailable/unverified. Filters do not change selections; hidden selected rows are still applied/saved. Existing setting search clears the destination filter.
- Explicit Apply and Apply at boot previews with detailed old/new values, including removed boot keys. Default is No. Existing caution and ABBA logic retained. Clear boot result no longer reports success after a failed operation.
- Preserve valid unchecked edited values on row rebuild; warn if unavailable rather than silently dropping those drafts.
- Screen-aware initial window size, saved window geometry, font-aware navigation widths/heights, accessible names, font-aware Ryzen/Firmware editors, non-elided Home status, persistent CO Search splitter.
- Font-aware line/bar chart layout, bounded bars and guarded narrow plot hit testing.
- Asynchronous, cancellable fwupd discovery (read-only) with timeout and failed/unknown distinction. Firmware installation remains explicit; the update dialog cannot be closed normally during writing, and the remaining queue stops on a failure. This does not guarantee safety on forced process termination or system shutdown.
- Turkish strings added, source-contract regression tests, QtCore unit test for boot comparison state and 64-bit value presentation.

## Verification
- CMake configuration attempted with `CENTURION_BUILD_TESTS=ON`; blocked because Qt6 development files/Qt6Config.cmake are unavailable in this environment. No native GUI build, visual QA or physical hardware tests performed. No Rust source, privilege policy, hardware helper or write algorithm changed.
- 17 of 20 existing non-native Python test scripts pass (12 GUI + 5 repository scripts); three suites already fail in the original archive with the same failing test names: `advanced_17_static_test.py` (5 checks), `test_evidence_lab_static.py` (7 tests), `test_session_audit_static.py` (11 tests). Do not interpret these as newly introduced failures or as proof of correct runtime behavior. New `gui_stage1_static_test.py`: 5 checks pass. Native CO and Qt tests were not executed.

## Remaining work, four stages
1. GUI Foundation & Optimization Studio: this increment covers the above; full global operation center, generic design/telemetry components, all blocking call sites, full HiDPI/visual regression matrix, result reports, additional optimization detail and persistence remain open.
2. Hardware Tuning & Autotune Studio: Ryzen/CO Search, NVIDIA/AMD GPU, Intel, firmware/fan and Autotune/Calibration workspaces.
3. Health & System Intelligence: Health, System Tools, telemetry/evidence/logs, Network Protection; investigate existing failing test contracts.
4. Unified Experience & Final Polish: Home, navigation, Scenes, Lighting, Backup, themes, translations and remaining integration/visual QA.

## Local GUI verification
```
cmake -S gui -B build-gui -G Ninja -DCENTURION_BUILD_TESTS=ON
cmake --build build-gui -j$(nproc)
QT_QPA_PLATFORM=offscreen ctest --test-dir build-gui --output-on-failure
```
Before using write actions, inspect the four tuning pages in a normal window and narrow window, toggle each filter, verify selections survive filters, compare boot values against the current system, inspect the previews then cancel. Verify Turkish strings and geometry restoration. Firmware discovery may contact fwupd; do not start a firmware update just to test the interface.
