# GUI Stage 4 — Unified Experience & Final Polish

Base: Centurion GUI Stage 3. Earlier changes are retained. This is the fourth implementation package, not a claim that the entire 235-item GUI backlog has been implemented.

## Implemented

- Global Ctrl+K in both shells indexes main pages and the Health tool catalog without constructing lazy tools or issuing hardware queries during search. Turkish character and word-order matching; guarded target references and stable Health keys. Other page-specific controls remain indexed when visible.
- Main-page navigation history (Alt+Left/Right), optional last-page startup in Experimental, window geometry persistence. Experimental Home switches between one and two columns according to available space; chart ranges 1/5/15/30/60 min and monotonic sample times with missing-data gaps.
- Searchable existing theme choices and compact appearance settings. Runtime UI mode is fixed at startup, so saving a preference for the next launch does not change the running shell. Theme changes still require restart.
- Read-only operation summary (Ctrl+Shift+O) for known GUI operations, and preference/config-restore restart checks before and after confirmation. This does not track arbitrary external operations or guard every possible shutdown path. CPU stress release and cooling fan restoration remain busy until their callbacks complete.
- Scenes: search, stable selection, unsaved-change prompts, failed deletion/selection recovery, revision guards against late hardware readbacks overwriting a different scene's editor.
- Lighting: bounded 32-snapshot draft Undo/Redo, reset history on readback/load/successful apply/reset; no implicit hardware writes from Undo/Redo. Improved editor sizing and pending-write tracking.
- Backup: preserve selected archive paths and scroll positions across refresh; reject actions on merely focused unselected rows; atomic verification-state saves and more flexible log display.
- 36 Turkish translation entries; all existing theme palettes retained. New Qt theme runtime fixture registered with CMake, plus eight static Stage 4 regression checks.

## Validation

- 26 test runners executed: 23 passed; three failed. Eight new Stage 4 static checks passed; five translation checks passed.
- Failing suites: `gui/tests/advanced_17_static_test.py`, `tests/test_evidence_lab_static.py`, `tests/test_session_audit_static.py`. Their failing assertion titles match the independently rerun Stage 3 baseline (5, 7, 11 respectively). These are unresolved failures, not a clean test suite.
- Native C++ CO recovery: 183,621 crash scenarios and 327,669 clean scenarios passed. Native CO walk: 454,710 walks and 2,353,711 probes passed. These are algorithm simulations, not hardware stress tests.
- All 129 Rust source files are byte-identical to Stage 3.
- Dependency install attempted with apt-get update and apt-get install (Qt6 development packages, Rust/Cargo). Debian DNS resolution failed, and requested packages were unavailable from local package indexes. No Qt6 development installation succeeded.
- CMake with CENTURION_BUILD_TESTS=ON and CENTURION_SANITIZE=ON detects GCC 14.2.0 but fails at Qt6 6.4 discovery (Widgets, Network, Svg, DBus). No GUI binary was built. Native Qt tests, visual regression tests, sanitizer runtime checks and actual hardware behavior remain unverified.
- Full command outputs are supplied in the separate Stage 4 testlogs archive.

## Reproduction (Debian-based development environment with package access)

```sh
sudo apt update
sudo apt install build-essential cmake ninja-build pkg-config qt6-base-dev qt6-svg-dev qt6-tools-dev qt6-tools-dev-tools
cmake -S gui -B build-gui -G Ninja -DCENTURION_BUILD_TESTS=ON -DCENTURION_SANITIZE=ON
cmake --build build-gui -j"$(nproc)"
QT_QPA_PLATFORM=offscreen ctest --test-dir build-gui --output-on-failure
```

Use the project README for the full helper/Rust build requirements. Do not run hardware-writing operations simply to validate GUI layout.

## Remaining work

The earlier Stage 1–3 reports enumerate remaining items. The full common design system, live theme switching, comprehensive global setting index, operation scheduler, scene diff/dependency graph, lighting effect timeline, extended backup restore preview, and comprehensive visual/HiDPI and hardware verification are not completed. Baseline static test failures require further investigation. No hardware-write algorithm or authorization policy was deliberately changed in this package.
