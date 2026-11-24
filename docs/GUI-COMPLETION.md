# GUI follow-up to Stage 4

This is an additional implementation increment based on centurion-GUI-stage4.zip, not a claim that all 235 original ideas are finished. No privileged helper or Rust code was changed. Qt compilation remains blocked; the GUI has not been executed or tested on hardware.

## Implemented in this increment (original IDs)

- GUI-19/20: custom 19-token RGB theme editing, versioned bounded JSON import/export, validation and persistent selection. Restart required; GUI-25 only partially addressed (main foreground contrast on panel surfaces, not all controls or palettes).
- GUI-31 partial: opt-in visibility of Experimental Home monitoring and hardware panels, saved in GUI settings; not arbitrary dashboard rearrangement. Device and power controls remain visible.
- GUI-43/44/29 partial: optional CCD/slot tiles with current candidate and textual state, actual topology, not a complete stability heatmap or independently verified result.
- GUI-65: 32-entry V/F draft undo/redo; core cap included, no hardware writes. Clears history at hardware helper boundaries. GUI-66 partial: no persistent operation archive or history inspector.
- GUI-51/71/100/190 partial: shared read-only JSON profile comparison in Ryzen, NVIDIA, optimizations and scenes (scene draft also available). Nested objects expanded, arrays atomic; not a measured hardware readback or full metadata inspector. GUI-88 (Intel) remains open.
- GUI-117 partial: read-only signature v10 inspector: measurement session grouping, contexts, quality, verification and imported game records. Limited to 8 MiB. No delete/edit or validity guarantees. GUI-116/122/123 partial, no interactive coverage map or complete A/B visualization.
- GUI-131/151 partial: pin one report in memory and compare its text with a subsequent report of the same code. No persistent session store or semantic row matching.
- GUI-142/143 partial: preview can omit raw technical text; copy/save exactly the preview. This is NOT automatic anonymization and structured findings can still contain private data.
- GUI-153 partial: S04 link delta from previous scan within one boot, verified vendor/device identity; missing or changed identities are not treated as valid deltas. No persistent topology graph.
- GUI-216/FIX-04: CPU stress capability discovery is asynchronous, bounded, start disabled before valid info. Not all other blocking calls are converted.
- Additional correctness: H03 missing AER counters are unknown, identity requires vendor and device; S04 missing width and uncorrectable AER errors are not falsely clean; EDID general refresh range is not proof of VRR; sleep report deduplicates identical records and groups by full boot identity; active ECC query bounded to five seconds.
- Turkish catalogue expanded for new text; source-contract checks added. Native Qt fixtures expanded for custom theme, PCIe identity/link, AER missing fields.

## Baseline failing suites

Three prior suites used stale source names or string literals following the diagnostics refactor. The assertions now follow the actual dispatcher, report generators, and atomic store, while retaining the read-only boundaries. Two missing behavior checks (S04 same-boot link delta and endpoint identity) now have a real implementation plus a native fixture. Sleep deduplication and AER unknown handling also have implementation changes. This is not proof that the 23 prior failing assertions represented 23 distinct runtime bugs.

## Still open (not an exhaustive per-ID audit)

- Full common design/telemetry/chart engine, globally indexed settings, operations scheduler and concurrent-operation guards, all blocking call sites, compact mode, visual regression and simulated hardware workspace (GUI-01/03/11/16/23/216–235 and FIX backlog).
- Full theme gallery, live theme switch, OLED mode, accessibility/HiDPI, flexible dashboard and detached tools (GUI-14–30 and 31–42 remainder).
- Full CO session archive, candidate decision explanation/ETA, firmware hierarchy, Intel domain and P/E dashboard, measured GPU/CPU transactions and full V/F history/validation overlay (GUI-43–93 remainder).
- Optimization source/conflict relationships, per-device workspace, journals, complete apply result reports and column customization (GUI-94–111 remainder).
- Interactive calibration coverage/validity, benchmark explorer, selective recommendation review, actual frametime charts and history (GUI-112–127 remainder).
- Full unified incident timeline, persisted diagnostic sessions/notes, privacy redaction, graph-based hardware tools, structured outputs, cooling/battery/sleep/SMART history, network process charts/firewall preview (GUI-128–188 remainder).
- Scene builder/dependency graph and dry run, lighting timeline and palette, full backup restore preview and dependency tree (GUI-189–215 remainder).

The Stage 1–4 notes remain useful for earlier changes. The complete 235-item audit has not been performed, and no blanket completion is asserted.

## Validation

26 previous Python runners passed in this environment, including the two native dependency-free CO simulation runners; the new completion smoke runner passed 7/7. Qt native tests were added but could not run: CMake cannot find Qt6Config.cmake (Qt >= 6.4). The previous Stage 4 apt attempt failed DNS resolution; no Qt SDK became available. See separately supplied logs. For a local build:

```
cmake -S gui -B build-gui -G Ninja -DCENTURION_BUILD_TESTS=ON
cmake --build build-gui -j$(nproc)
QT_QPA_PLATFORM=offscreen ctest --test-dir build-gui --output-on-failure
```

Before using any hardware write controls, verify draft undo/redo and cap state, theme imports, profile comparison, CCD labels and diagnostic data on real hardware. Test both GUI modes, both languages, and window resizing. Do not use stress or tuning writes to validate basic GUI behavior.
