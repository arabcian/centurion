# Centurion Optimize – Legion Pro 7 / X3D extensions (2026-11-16)

This source tree is based on the user's `centurion-main(20261116-081018).zip`. It contains changes only for the **eight checked** requests. The three unchecked requests (CPU/GPU dynamic power budget, thermal-aware autotune, Lenovo fan_mode/conservation) are intentionally untouched.

## Implementation map

1. **Optimization Conflict Detector:** Read-only current platform-profile/EPP, CPU max-frequency, X3D mode, firmware CPU/GPU readback, irqbalance and swap tier diagnostics in `tune.rs`, with a compact report in the Optimize GUI. Root helper now rejects presets that mix a new per-device/per-CCD knob with an overlapping global knob **before writing any value**.
2. **Per-NVMe controls:** Independently tune two detected namespaces: scheduler, read-ahead, queue depth and completion affinity, plus controller APST latency QoS. The displayed device identity includes model/serial, and slots are ordered by serial and namespace rather than transient `/dev/nvmeX` numbering. A removed or replaced disk can still change which device occupies a slot: **review the displayed device identity before applying saved presets**.
3. **Per-game cgroup uclamp + memory.low:** UI and launcher pass clamp minimum/maximum (0–1024) and best-effort memory protection (MiB) through authenticated `isolate_join` to cgroup v2. Bounds validated, `memory.low` capped at half of physical RAM, controllers enabled as needed, temporary cgroup removed at session end.
4. **X3D Adaptive Placement:** The new Game Launch affinity option chooses the cache/frequency CCD **at launch**, following the preset's `cpu.x3d_mode` (or live kernel hint). It avoids the soft-parked CCD. This is **not** runtime profiling or continuous per-thread migration.
5. **zram and swap tiers:** `mem_limit` for two active zram devices (only when present in sysfs); read-only display of swap priority and usage for `/proc/swaps`; warning when disk swap has at least as much priority as zram. **No automatic swapoff, swapon, reprioritization or compression-algorithm reset**, because these can cause stalls or OOM on a running system. Configure persistent swap priorities in boot management instead.
6. **F2FS controls:** For two sysfs instances when present: `iostat_enable`, `gc_idle` and `min_gc_sleep_time`. These only appear when real sysfs files exist. Filesystem-specific controls may have kernel-dependent semantics; avoid setting without first measuring I/O and wear impact.
7. **Per-CCD wake latency:** CCD0/CCD1 `power/pm_qos_resume_latency_us`, using detected CPU group membership. Do not simultaneously select global `cpu.wake_latency_us` (preflight rejects overlapping files).
8. **IRQ / NIC queues:** Per PCI-device-class IRQ affinity (NVMe, network, NVIDIA) plus physical-NIC RPS and XPS queue masks. Managed IRQs can reject manual affinity. The existing IRQ balance pause/resume and best-effort policy are extended to these targeted IRQs. NVIDIA affinity is explicitly caution-marked.

## Safety and limits

- Kernel knobs are resolved from known target types; no user-controlled arbitrary file path is accepted. Root helper retains its original baseline/rollback flow.
- Newly added hardware-specific controls are excluded from **unattended calibration**, including `--only` requests; they require deliberate manual selection.
- Global and device-specific keys targeting the same resolved sysfs file are rejected together. Older-old configurations are unaffected.
- The root helper does **not** mutate firmware CPU/GPU power budgets, thermal autotune or Lenovo fan/conservation settings.
- **Build and hardware verification are pending**: this container lacks `cargo`, `rustc`, `rustfmt` and Qt6 development packages, and does not contain the user's Lenovo device hardware. Python and GUI static tests have been run but are not a substitute for successful Cargo/CMake compilation and on-device tests.

## Gentoo verification workflow

Run these commands in the repository root on the target development machine:

```sh
./verify-build.sh
cargo test --workspace --locked
cargo check --workspace --all-targets --locked
cmake -S gui -B build-gui -DCMAKE_BUILD_TYPE=Debug -DCENTURION_BUILD_TESTS=ON
cmake --build build-gui --parallel 4
QT_QPA_PLATFORM=offscreen ctest --test-dir build-gui --output-on-failure
```

On hardware, first inspect Optimize without applying anything. Verify NVMe model/serial, supported F2FS attributes, existing zram size and memory limit, live CPU groups and available cgroup controllers. Then test one new knob at a time; use Restore to confirm baseline restoration. For IRQ and NVMe changes, test with GPU game stability, I/O workload and network traffic while watching kernel logs.
