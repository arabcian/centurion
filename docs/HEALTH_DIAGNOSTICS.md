# Health diagnostics: Evidence Lab, Investigations, interpreted System tools

Every diagnostic produces a `diag::Report` (`gui/src/diagreport.h`): a verdict, findings in plain language
(what was seen, why it matters, what to do), tables of the readings, the limits of what the sources can tell,
and the raw readings behind a toggle. `DiagReportView` draws it the way Drive health draws a drive; *Copy
report* / *Save report…* export the same content as text. A source that cannot be read is **Unknown**, never
"no problem".

| Where | Code | What it answers | Sources |
| --- | --- | --- | --- |
| Problems → Evidence Lab | H02 | Which core / unit a machine check names, on the Curve Optimizer's own CCD / slot map | health-reader `diagnostics`, `ryzen::detect()`, `coevidence` bank classes |
| | H03 | PCIe AER counters and their growth since the last scan of this boot | `aer_dev_*` (kernel format `TOTAL_ERR_COR 14`) |
| | H04 | Every Xid / GSP event by code with its meaning (`eventinfo`), Xid 154 cases | `diagnostics` → `xid`, `xid_cases` |
| | H05 | F2FS's own need-fsck / checkpoint-error / on-disk-error flags, logged integrity errors | `/sys/fs/f2fs/<dev>/stat/{sb_status,cp_status}`, `errors`, health.log |
| | H09 | Wi-Fi / Bluetooth firmware load failures, crashes, timeouts per adapter | `diagnostics`, health.log |
| Problems → Investigations | H18 | Guided triage: runs the checks of a symptom and collects what needs a look | the checks below |
| | H01 | Did a kernel / NVIDIA / BIOS / microcode change bring more unclean ends or faults? | boot history + per-boot `versions` (boot guard) |
| | H06 | NVMe SMART / Health log and Error Information Log, decoded | health-reader `nvme_deep` (nvme-cli, read-only) |
| | H10 | USB enumeration failures, over-current, EMI port resets, dead xHCI, per port | kernel.log |
| | H12 | Failed suspends (kernel `suspend_stats`), sleeps per boot, boots that died asleep | `/sys/power/suspend_stats`, sleep.log, sleep-failed.log, sleep-inflight |
| | H13 | CO Search journal, crash guards, past searches, unclean ends per CO state | CO Search records, boot history curves |
| | H15 | Flight-recorder coverage of the previous boot and its last readings | boot history `flight` |
| | H16 | Memory-related Xids; on request nvidia-smi ECC / row-remap / page-retirement | `diagnostics`, `nvidia-smi -q -d ECC,ROW_REMAPPER,PAGE_RETIREMENT` |
| | H17 | Incident notebook: status and notes per classified event, with its explanation | health.log, `incident-notes.json` |
| Advanced → System tools | S01 | Hardware changes against a saved baseline, grouped (device gone, driver lost…) | sysfs, `/proc/modules`, `hardware-baseline.json` |
| | S03 | Display identity, preferred mode, range limits, FreeSync / HDMI VRR range, size, bit depth, colour formats, HDR and colour spaces, audio, backlight devices, damaged EDIDs; *Panel controls…* sets panel brightness and refresh rate (as the user, via `centurion-display`) | `/sys/class/drm/*/edid` |
| | S04 | Link width / speed against both ends of the link; width loss vs. idle down-clocking | `current/max_link_*`, port above |
| | S06 | NVIDIA options configured vs. in effect (0x02 = 2, Y = 1), Wayland modeset, sleep hooks | modprobe.d, cmdline, `/sys/module`, `/proc/driver/nvidia/params` |
| | S07 | Wireless adapters, rfkill, firmware the kernel asked for and did not find | sysfs, kernel.log |
| | S08 | Firmware PPT targets, platform profile, cpufreq caps, boost, RAPL, power source, Dynamic Boost | firmware-attributes, cpufreq, powercap |
| | S10 | RAM, swap, zram + zswap double compression, THP, MGLRU, PSI pressure, NUMA / L3 | procfs, sysfs |
| | S11 | WMI objects with their drivers (Lenovo GUIDs Centurion uses), firmware settings, ACPI tables | `/sys/bus/wmi`, firmware-attributes |
| | S13 | The charger's USB-PD profiles, contract, battery health and charge limit | `/sys/class/typec`, `usb_power_delivery`, power_supply |
| | S15 | Sound cards / codecs, competing sound servers, missing session manager, dummy output | `/proc/asound`, `/proc/*/comm` (own user), `pactl info`, `wpctl status` |
| | S18 | Kernel options Centurion's features need, compressed firmware the kernel cannot read, missing firmware, driverless devices | kernel config, `/lib/firmware`, kernel.log, sysfs |

## Records added for these reports

- **Per-boot versions** (`stability.rs`): the boot entry in `/var/lib/centurion/exposure.json` gains
  `versions: {kernel, bios, microcode, nvidia}` when it is opened; a value read later (the NVIDIA module loads
  after the guard) is added once and never replaced. `boothist` passes it to the GUI.
- **Died-in-sleep marker** (`packaging/sleep/centurion-sleep-log`): the pre hook writes
  `/var/lib/centurion/sleep-inflight` (`<boot_id> <time> <kind>`), the post hook removes it. A marker of another
  boot found by the next pre hook is appended to `/var/log/centurion/sleep-failed.log`.
- **NVMe** (`health_nvme.rs`): SMART fields under fixed names whichever tool spelled them (nvme-cli's
  `avail_spare` / `percent_used` / kelvin, smartctl's names), and the error-log entries with their status field.
- **Xid list** (`health_diagnostics.rs`): every Xid / GSP event of the scan window, not only Xid 154 cases.

## Fixed in the earlier diagnostics

- AER totals: the kernel prints `TOTAL_ERR_COR 14` (no colon); the parser required a colon, so every count was
  Unknown — in Evidence Lab and on Events & alerts.
- H06 read smartctl field names from nvme-cli output: spare and life used never showed; the temperature was kelvin.
- H12 counted suspend/resume lines in logs that keep only warnings: always 0. It now uses `suspend_stats`.
- H01 recorded versions only when the page was opened in that boot: comparisons almost never had data.
- H17 offered every kernel warning as an incident; it lists classified events only.
- H13 printed the attempt count of an array as an empty string.
- S06 reported `0x02` against `2` as different.
- S18 checked a fixed MediaTek firmware list instead of what drivers actually requested.

## Tests

- `gui/tests/advanceddiagnostics_test.cpp`, `gui/tests/diagnostictools_test.cpp`: every report over fixtures
  and a fake sysfs / procfs tree (ctest `advanceddiagnostics`, `diagnostictools`).
- `tests/test_health_diagnostics_contract.py`: no state-changing tool, no write path outside the private
  store, CO records read-only, NVMe helper limited to log reads, every code reachable.
- Rust: `health_nvme`, `health::aer_sysfs_format_and_unknowns`, `stability::versions_are_filled_once_and_never_replaced`.
