# Centurion 36 Themes — 8 Diagnostic Features

Source base: `centurion-main-36-themes.zip` (2026-11-21). All existing features and 36 theme IDs retained.

## Health → Problems → Evidence Lab

The Evidence Lab is explicitly **on-demand**. It does not poll hardware, change voltage or firmware, wake a sleeping NVIDIA GPU, run a stress test or launch fsck. Press **Scan evidence** to ask the existing read-only privileged `health-reader` for a bounded kernel-event scan and current sysfs topology/AER values. Without access to `/dev/kmsg`, the scan reports an access failure rather than reporting health. The scan includes at most 300 *classified* events from this boot; kernel message retention is finite. An empty report does not mean healthy.

- **H02 — Ryzen MCE Core/CCD Atlas:** machine check CPU, MCA bank and any APIC ID explicitly printed by the kernel; Linux logical-CPU to `core_id`, package, die, thread sibling and L3-sharing topology mapping. No assumption that L3 group equals a uniquely numbered CCD or that logical CPU = CO core. Missing CPU/APIC mapping remains Unknown. Test fixtures cover unknown IDs.
- **H03 — PCIe AER Delta Tracker:** correctable, nonfatal and fatal AER counters for all sysfs-exported AER-capable devices, including zero counts. Comparison uses the last *same-boot* snapshot, saved atomically in the user's local data directory after pressing Scan. Counter decrease, disappearing device, a different boot or inaccessible reading produces Unknown, not a negative delta. AER counters do not establish exact event time.
- **H04 — NVIDIA Xid Case Analyzer:** analyzes Xid 154 recovery actions against up to 16 preceding Xid/GSP/AER/D3cold events in the previous five minutes of the *same boot*. Xids on different identified GPUs are excluded, events without identifiable device are labeled device-unverified. Main Health list also no longer labels an arbitrary prior Xid the root cause. Causation remains unproven.
- **H05 — F2FS Integrity Guard:** F2FS errors, corrupt metadata, checkpoint failures, failed I/O, invalid block/node addresses; distinguishes normal mount/GC operations and `error_policy` text. Shows F2FS mounts and bounded matching entries from the existing Health history. Explicitly does not run a repair on a mounted volume.
- **H09 — Wi-Fi / Bluetooth Failure Ledger:** classifies firmware load failures, controller errors and timeouts, including MediaTek and Intel. Counts by observed adapter/controller where log IDs exist; missing IDs remain unknown. Includes bounded older entries from the existing rotated `health.log`; normal association and ordinary Bluetooth setup events are not marked as failures.

## Health → Advanced → System tools

These continue using the existing lazy `ToolPage`, with no automatic scheduled work.

- **S01 — Hardware snapshot:** compares currently exposed PCI BDF/vendor/device/driver/link, loaded kernel modules with available versions, kernel, BIOS, microcode and *redacted* cmdline flags against a user-saved baseline. Press **Save baseline** to persist; **Compare baseline** compares against current readings. The baseline is not overwritten by Run again; file permissions are owner-only and saving is atomic. PCI renumbering and dynamic idle link speeds are contextual differences, not asserted faults. The snapshot intentionally excludes serial numbers, MAC addresses, UUIDs and non-allowlisted kernel-argument values.
- **S03 — EDID / VRR:** reads connector status and *DRM-exposed* EDIDs, validates header and individual 128-byte block checksums, detects truncated extensions, parses base detailed timings and frequency-range descriptors, lists DRM modes and exported `vrr_capable` when available. Does **not** edit EDID. Monitor frequency-range descriptors are explicitly **not** reported as proven VRR range. Connector EDID may already be overridden; not guaranteed to represent physical EEPROM.
- **S06 — NVIDIA / Kernel Module Parameters Diff:** examines `modprobe.d` NVIDIA options (excluding commented lines), NVIDIA-related boot parameters and runtime values from `/sys/module/.../parameters` and `/proc/driver/nvidia/params`. `modinfo -p nvidia` remains available on the page when installed. Missing runtime fields are Unknown; mismatched encodings are labeled for review, not declared broken. Does not invoke `nvidia-smi` or wake the dGPU.

## Changed/new files

`crates/centurion-helpers/src/health.rs`, `health_diagnostics.rs`, `lib.rs`, `bin/health-reader.rs`;
`gui/src/healthtab.cpp/.h`, `healthevidence.cpp/.h`, `eventinfo.cpp`, `systools.cpp`, `diagnostictools.cpp/.h`;
`gui/CMakeLists.txt`, `gui/i18n/centurion_tr.ts`;
`gui/tests/diagnostictools_test.cpp`, `tests/test_evidence_lab_static.py`.

## Reproducible testing

On a Gentoo system with Rust/Cargo and Qt6 development packages installed:

```bash
# From the repository root:
cargo test -p centurion-helpers health::tests --lib
cargo test -p centurion-helpers health_diagnostics::tests --lib
python3 tests/test_evidence_lab_static.py
python3 gui/tests/health_ui_static_test.py
python3 gui/tests/theme_palettes_static_test.py
cmake -S gui -B build/gui-tests -DCENTURION_BUILD_TESTS=ON
cmake --build build/gui-tests --target centurion-diagnostictools-test
ctest --test-dir build/gui-tests -R '^diagnostictools$' --output-on-failure
```

If the package name differs, inspect `crates/centurion-helpers/Cargo.toml` before running the cargo commands.

**Verification performed in this editing environment:** static source integration/security/legacy layout tests, theme palette test, Turkish translation consistency and TS XML parsing. **Not performed:** Rust compilation or Qt6 compilation/runtime/physical-machine verification; this environment has neither Cargo/Rust nor Qt6 development files. The integration is supplied as source, not as an already-validated built executable.

## Safety and limitations

These tools gather observational data, not definitive diagnoses. Root helper remains confined to classified event reading and sysfs topology. GPU/CPU Curve Optimizer, scene automation, firmware and NVcurve write paths were not changed. AER and hardware baseline JSON are saved under the user's application data directory only after the user selects the corresponding action. AER history tracks the most recent comparable sample; S01 preserves one user-selected baseline. Saved health history follows existing 8 MiB rotation. Localization of new menu/tab controls is included in the existing Turkish catalog; detailed technical report text is currently English.
