# GUI overhaul — stage 3 of 4, Health & System Intelligence

Baseline: `centurion-GUI-stage2.zip`. All earlier source changes retained.

## Implemented in this increment
- Shared diagnostic reports (`DiagReportView`): search over findings and table cells, severity filter for findings only, result counts, section navigation, snapshot display timestamp, scroll/filter retention on direct replacement of the same report, and explicit reset/disable of the previous report while a message replaces it. Original verdict, limits and complete report exports are not altered by filters. Tables preserve diagnostic order until the user requests sorting; narrower initial columns and height adaptation reduce clipping and empty space. Export preview shows the full report and explicitly warns about private data; it does not claim to redact it. Atomic `QSaveFile` checks for partial writes. The chosen save dialog exports the report captured before that dialog opened.
- H17 Incident Notebook: searchable events (including raw saved event text), scope counts, temporary in-memory per-event note/status drafts when selecting/filtering/reloading; UTF-16 length counter consistent with existing 2000-character backend limit. Explicit Save is still required; draft data is not persisted automatically. Selection and scroll are restored where possible. No changes to incident-note storage or diagnostic interpretation.
- Kernel log: exact same-boot context window is collected from both saved files before applying the normal display cap/filters. Old records in retained files are now eligible even when more than 6000 newer records exist. The context is still limited to saved warning-or-worse records and cannot reconstruct deleted or never-captured records. Follow latest toggle, displayed count, and explicit tail-limit notice; more compact control arrangement. Existing capture, mute and log-retention policy unchanged.
- System Tools: search enabled by default for raw command-output tools; up to 2000 highlights with a `+` marker when further results exist. Enter still navigates across the full displayed document, including beyond the highlight cap. Search and Copy operate on displayed (including masked) text. No new hardware probes or privileged operations. Existing tool keys, favorite/recent settings, lazy initialization and masking retained.
- S03 Displays: new per-block checksum-byte, checksum sum modulo 256 and block-type table, plus a decoded detailed timings table. A matching checksum is not proof of EDID correctness; parsed timings are not the driver's complete mode list. Reads only existing EDID bytes.
- S04 PCIe Link Check: additional resolved sysfs upstream relationship table including separate device and upstream maximum rate/width. Unknown parent is shown as unknown rather than guessed. Existing PCIe diagnostic findings and calculations unchanged.
- Network Protection: compact multi-row control layout, debounced column width persistence for six tables, preserve sort/scroll and selected socket on refresh only when the identity tuple has a unique match. Bounds/identity/selection checks before resolving the current connection. Socket identity here is a snapshot, not a guarantee that the socket is still present. Connection and port views explicitly show snapshot timestamp and refresh failure; Wi-Fi list restores selection by BSSID. No firewall/rule policy/helper change. Root snapshots remain historical. Existing listening-port grouping retained.
- SMART: last successful read time (not just date), prior-read timestamp on in-progress or failed refresh, preserve table scroll. No new SMART health criteria or active drive tests.
- Boot History: preserve selected boot and scroll when possible; never restore a row hidden by the current scope filter after refresh.
- Turkish translation entries for new controls, messages and tables. Existing layout/translation preferences preserved.

## Verification
- 20/23 Python source/integration test scripts (excluding two native CO scripts) passed; the same three suites remain failing with identical failure names as Stage 2: `advanced_17_static_test.py`, `test_evidence_lab_static.py`, `test_session_audit_static.py`. These are not claimed fixed. `gui_stage3_static_test.py` 9/9; translation checks 5/5; title-case check passed.
- Both native CO test scripts also passed (separate from the Qt GUI): 183621 crash scenarios + 327669 clean scenarios; 454710 per-core walks and 2353711 probes. Total Python scripts passing 22/25, counting those two runners. No CO algorithm change.
- Added native C++ fixtures for old kernel-log context (8000 records, same-boot identity, missing and boundary cases), EDID checksum rows/corruption and PCIe parent/max-rate rows. These are NOT executed here: CMake configuration with `CENTURION_BUILD_TESTS=ON` failed because Qt6 6.4+ development configuration is missing. Native GUI compilation, CTest, visual/HiDPI checks and physical hardware validation remain pending.
- No Rust source, privilege policies, hardware write paths or solver algorithms modified. No automatic changes to BIOS, clocks, firewall rules or kernel parameters introduced by the new inspectors.

## Remaining work
This is an incremental delivery, not completion of every idea GUI-128–188. Full unified incident/session timeline, graph-based evidence relationships, persisted diagnostic session comparison, selective/redacted export, graphical PCIe device tree, EDID binary/VRR visualization, Wi-Fi channel chart, process-level traffic graphs, firewall-rule dry-run, cooling/battery/sleep comparisons and complete tool-output structuring remain open. Stage 1/2 outstanding items are in their respective notes. Stage 4 still pending.

## Local verification
```
cmake -S gui -B build-gui -G Ninja -DCENTURION_BUILD_TESTS=ON
cmake --build build-gui -j$(nproc)
QT_QPA_PLATFORM=offscreen ctest --test-dir build-gui --output-on-failure
```
Check report filters do not affect full copy/save, report preview and privacy labels, old kernel log navigation (including a restrictive level filter), hidden/vanished network selection, incident draft restoration on filtering/reload, EDID checksums, boot filter/selection, SMART stale labels, and small-window Turkish layouts. No dangerous hardware operation is required for these UI tests.
