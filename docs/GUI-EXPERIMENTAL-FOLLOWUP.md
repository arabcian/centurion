# Experimental GUI follow-up (not release-ready)

Base: the previous GUI completion archive, incorporating all prior implementation increments. The original uploaded ZIP was reconstructed as a Git tree and exactly matched GitHub main commit `6eb741e1715229aa11fb010d531a1ed9aec81087` (tree `7900553fb957694681fab68d98e35785f24622b6`). No remote branch or commit was modified.

## New functionality

- GUI-18: searchable theme gallery with theme previews and light/dark/pastel categories. GUI-21: OLED Black palette (an appearance option, not a burn-in safeguard). The previous 56 palette IDs and order remain unchanged. GUI-23: optional compact control padding/default layout spacing, saved separately; applied at restart. Explicit widget-specific dimensions may still override the density. GUI-25: contrast checks for the new OLED palette. Full live theme switching is not implemented; several widgets cache literal colors, icons and style sheets.
- GUI-88: Intel profile comparison uses the existing read-only JSON comparison viewer.
- GUI-124: Frametime Analysis under Optimizations > Inspect. Reads explicitly chosen FLM `interval_ns`, MangoHud `frametime`, or plain millisecond samples; rejects unknown CSV columns, nonfinite/invalid values, and files larger than 16 MiB. Reports accepted/rejected sample counts, mean, P50/P95/P99, maximum, and FPS derived from mean interval. All accepted samples are used for statistics; min/max bucket plot retains spikes, x is accepted sample order, not elapsed time. This does not import into the calibration model. The parser/statistics have independent native C++ tests.
- GUI-228 partial: the shared LineChart now rejects nonfinite values, shows the nearest plotted point with a cursor and tooltip, and can export numeric plotted samples as CSV via an atomic save. Other chart classes and the V/F editor do not yet share this interaction, and there is no common zoom engine.
- GUI-195/198 partial: existing SceneEngine read-only preview now flags missing referenced CPU/GPU/tuning profiles, in addition to its current hardware and conflict checks. No execution or rollback logic is changed.
- GUI-128/134 partial: selected-incident report merges available sensor, curve, scene, and related kernel records chronologically; distinguishes boot-prefix-only kernel candidates from full boot-ID matches, bounds and validates timestamps, and does not infer root cause. Not a full persistent cross-boot session workspace.
- GUI-210/211 partial: system-image restore metadata preview follows explicit parent sidecars in oldest-first order, shows the current target and local verification records, warns on missing/ambiguous metadata or live root. It does not list archive file contents, authenticate metadata or replace privileged restore validation and confirmations.
- GUI-235 partial: isolated offscreen appearance smoke test over all built-in themes, both UI settings, both density settings, and two representative widget sizes (no hardware/helper calls). This is not screenshot baseline comparison or comprehensive full-app DPI coverage; it has not run because Qt6 is unavailable.
- New Turkish strings for the additional controls. No Rust files or privileged helper algorithms changed.

## Validation

- All 27 existing Python runners passed, including dependency-free CO search C++ simulations. Affected static and translation checks were repeated after the incident-report update.
- New frametime test compiled under C++20 with `-Wall -Wextra -Werror -fsanitize=address,undefined` and ran successfully. It checks headers/preamble, units, invalid values, statistics and preservation of outliers.
- Native GUI configure still fails at Qt6 >= 6.4 `find_package`: Debian package host DNS resolution fails and no local package indexes/SDK are available. Cargo/Rust install also failed. No full GUI build, full native Qt tests, runtime visual checks, or hardware testing is claimed.

## Outstanding scope

The prior GUI-COMPLETION and GUI-STAGE1–4 notes remain applicable except for the specific additions above. The remaining large projects are not completed: full common design system and telemetry cache, full chart engine, safe live theme switching, full hardware simulation, full visual baseline regression tests, Intel P/E topology dashboard, firmware power hierarchy, visual scene builder, lighting effect timeline, full backup file-level restore preview, and other partial or open IDs from the original 235-item plan. Do not treat this archive as a fully validated release. Experimental publication was withheld because the user requested completion and error verification before upload.
