# Centurion — Experimental Studio v4

This source branch contains real Qt/C++ layout changes relative to the supplied Experimental Studio v3. No screenshots or rendered mockups are claimed as build output.

## Workspaces

| Area | Studio composition |
|---|---|
| App shell | 154 px navigation / 48 px compact mode, 32 px navigation rows, small top command strip, existing hardware-gated page IDs and Ctrl+K selector |
| Home | Real profile/device controls + real EC/sensor timeline, responsive split ratio saved between sessions |
| Health | The **second sidebar is removed** in Studio. A contextual picker switches all original health pages. The system tools directory similarly uses a tool picker while retaining the original tree-based deep-link model. |
| Ryzen Curve | Original CCD per-core editor preserved at full working width. Profile, Reset and All-Core controls share a top action band. Diagnostics log is disclosed on demand. |
| NVIDIA Curve | Full-width original V/F graph, original profile/actions and sensor readouts, expandable driver log |
| Intel Undervolt | Three workflows: Voltage & safety, Limits & throttling, Startup & daemon. Apply/boot actions remain pinned; original editors are moved, not duplicated. |
| AMD GPU | Dynamic firmware-dependent sections retain their original operations. Single-category cards use the whole content width rather than reserving an empty column. |
| Optimize | Original searchable advanced settings and Apply Selected flow. Autotune tools expand when requested; actual calibration status is always shown. |
| Firmware | Discovered CPU and GPU controls share a row when both exist; only one control group takes both columns, Other occupies the full row. Existing read-only/WMI/Custom-mode safety gates remain. |
| Scenes | Real scene picker and editor in a saved split layout. Pause/Resume always visible; automation rules only expanded when needed. |
| Lighting | Existing full keyboard canvas and effect editing, user-resized split stored between launches. |

Classic mode remains an alternate layout. Operational device access, privileged helpers, SMU/sysfs/MSR/NVML logic, services, and trial rollback timers are intentionally unchanged. This is a UI composition and density change, **not** a reimplementation of hardware behaviour or a hardware validation certificate.

## Validation

```bash
python3 gui/tests/experimental_ui_static_test.py
python3 gui/tests/experimental_layout_static_test.py
python3 gui/tests/health_ui_static_test.py
python3 gui/tests/studio_v4_static_test.py
cmake -S gui -B build/gui -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/gui -j"$(nproc)"
ctest --test-dir build/gui --output-on-failure
```

The authoring environment lacks Qt6 development packages. No real Qt compilation, startup screenshot, keyboard interaction, polkit or hardware test has been performed here. Run a compile and hands-on visual verification on a Qt6 (>=6.4) Gentoo system before replacing an installed build.
