# Centurion — 17 advanced Health/System Tools diagnostics

Integrated onto the **36 Themes + 8 Diagnostics** source snapshot. Existing eight diagnostic features and themes remain in the source tree.

## User access

Health → Problems → **Investigations**:

| Code | Tool | Evidence/limitations |
| --- | --- | --- |
| H01 | Boot Regression Detective | Only boots with sampled versions can have their kernel/NVIDIA/BIOS versions compared; completed boots with sufficient duration are required for event rates |
| H06 | NVMe Deep Health | User-triggered `health-reader` `nvme_deep` operation; bounded `nvme-cli smart-log` and `error-log` on enumerated NVMe character devices |
| H10 | USB Stability Watch | De-duplicates retained local USB/xHCI error lines; unavailable logs do not mean no faults |
| H12 | Sleep/Resume Failure Matrix | Correlates retained suspend/resume/USB evidence and unclean boots without attributing cause |
| H13 | CO Evidence Cross-Link | Reads only CO Search session, crash guards, and history; never changes guarded CO candidates |
| H15 | Pre-crash Coverage Inspector | Computes recorded Flight Recorder coverage and gaps, not an invented crash-time sensor sample |
| H16 | GPU Memory Reliability View | Passive PCI and event view; separate **explicit** optional `nvidia-smi` query can wake GPU, 5-second timeout |
| H17 | Incident Lifecycle | Private status/notes keyed to retained event boot+sequence, atomic writes, bounded input |
| H18 | Guided Diagnostic Wizard | Incident-family selector and safe observation/record/recheck steps; no automatic remediation |

Health → System tools (also searchable):

| Code | Tool | Evidence/limitations |
| --- | --- | --- |
| S04 | PCIe Link Negotiation Lab | On-demand same-boot speed/width/AER delta; link downclock at idle may be normal |
| S07 | Wi-Fi/Bluetooth Firmware Doctor | Enumerated network adapters, USB radio identities and retained firmware logs |
| S08 | Power Limit Source Map | Read-only powercap/pstate/platform profile and passive NVIDIA runtime state; unknown SMU/EC limits stay unknown |
| S10 | Memory Architecture Explorer | NUMA, L3-sharing domains, THP, zram, PSI, cgroup memory information |
| S11 | ACPI/WMI Firmware Browser | Metadata and possible values only; no firmware-method invocations |
| S13 | USB-C PD / Charger Inspector | Type-C power role and power-supply exports; measured power is **not** negotiated PD capability |
| S15 | PipeWire Audio Doctor | Existing ALSA/sysfs data and bounded on-demand `wpctl`/`pactl` commands (no service restart) |
| S18 | Kernel Feature & Firmware Dependency Check | Bounded config read and selected optional firmware existence checks; missing files are not necessarily evidence of load failure |

## Safety model

- No new writes to SMU, MSR, PCI config space, EC, ACPI/WMI, EFI, drivers, or NVMe state.
- **H17 notes** and **H01/S04 baselines** are the only newly persisted outputs, in per-user application data directories; saved atomically, not into kernel or firmware interfaces.
- Collection occurs only after a user click. H16 active GPU query is separate and plainly labelled.
- Evidence absence, permission denial, unsupported hardware and counter resets are **Unknown** rather than asserted healthy or zero.
- H06 is the exception to purely passive polling: NVMe SMART/Error Log are non-destructive admin read commands; on uncooperative hardware/firmware they may still wake the device or cause latency. User initiates the scan explicitly.

## Tests

Pure fixture tests: `gui/tests/advanceddiagnostics_test.cpp` (boot regressions, USB deduplication, PCIe delta, flight gap unknown), integrated with optional CMake test build. Rust NVMe parsing tests in `health_nvme.rs` (whitelisted names, sanitized SMART fields, unknown formats).

Dependency-free source checks:

```sh
python3 gui/tests/advanced_17_static_test.py
python3 gui/tests/health_ui_static_test.py
python3 gui/tests/theme_palettes_static_test.py
```

Build and actual Qt tests **must** be performed on a Gentoo system with Qt6 and Rust/Cargo installed:

```sh
cargo test -p centurion-helpers health_nvme
cmake -S gui -B build-gui -DCENTURION_BUILD_TESTS=ON -DCENTURION_LTO=OFF
cmake --build build-gui -j2
ctest --test-dir build-gui --output-on-failure
```

Do not claim the UI or NVMe paths are validated on actual target hardware based on static checks alone.
