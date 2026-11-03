# nvcurve: Flatten safety preflight (experimental)

This branch intentionally **preserves the existing flatten algorithm**:

- With an available offset-free V3 base: `delta_kHz = target_kHz - V3_base_kHz`.
- Without V3: `delta_kHz = target_kHz - (live_VFP_freq_kHz - live_CT_delta_kHz)`.

The new `crates/nvcurve/src/hal/flatten_guard.rs` module is a **read-only preflight**. It does not change the actual flatten calculation or send a stress workload to the GPU. It runs for **all profile entry points** (GUI, CLI, autoload) because they share `profiles::apply::apply_profile`.

## What the preflight checks

1. Read the V/F curve and ClockBoostTable twice from NVIDIA's driver, without writing anything.
2. Refuse changes in point count, point index, GPU/memory domain, voltage, or previous CT delta between the two reads.
3. Compare reconstructed base frequencies of the flattened points across both reads; tolerate up to **45 MHz** natural boost-bin/temperature drift. A larger difference aborts the transaction.
4. Reject absurd voltage/base readings, voltage reversals, sudden local frequency steps (more than **+180 / -45 MHz** across at most 25 mV) and a single point deviating more than **90 MHz** from local linear interpolation.
5. Confirm that **each individual selected point**, when its prepared delta is added to the *same first-read base* used by the algorithm, equals the desired flat frequency. This catches calculation, rounding, and clamp-induced deviations.
6. Confirm that the point immediately before the flatten start does not exceed the new plateau by over **30 MHz**.
7. Check the entire CT against the initial CT at preflight, and **once more immediately before the first write** to avoid overwriting another tool's concurrent settings. A stale preflight aborts without rollback, because this transaction has not changed the GPU.
8. After an accepted profile is sent, the exact CT readback must still match; a failed write triggers best-effort NVML/CT rollback. Previous NVML clock locks cannot always be restored and are reported as such.

If any check fails, the profile application returns `Flatten safety preflight: ...` and **does not apply the new GPU settings**. The user can retry when the GPU is idle or revise the anomalous voltage/frequency point.

## Safety limitations

- A consistent but *capped* VFP read can hide the true offset-free base when V3 is unavailable. Two reads **cannot prove** that this estimate is correct. The preflight is a plausibility and concurrency gate, **not proof of GPU electrical stability**.
- The checks cannot determine whether a frequency/voltage pair is stable under actual workload. No stress test is run automatically, so the GPU does not take additional stress before applying an undervolt.
- Thresholds are intentionally conservative, but different GPUs/drivers may need adjustment. A false rejection is preferable to applying an outlier.
- Blackwell raw REGISTER_OP temperature probes are also disabled by default in this branch for independent PRI/GSP concerns; opting back into that experimental path is unsafe.
- The driver may normalize offsets. The code currently requires exact CT readback; if that driver normalizes a value, the profile fails instead of silently accepting a different offset.

## Gentoo verification

From the project root:

```bash
python3 tests/nvcurve_safety_static_test.py
cargo test -p nvcurve --lib
cargo build --release -p nvcurve
```

The Python tests check wiring and source invariants; they do not execute Rust. The seven Rust `flatten_guard` tests include a healthy old-style flatten without V3, 15 MHz boost-bin drift, and rejections for incorrect offset, unstable read, voltage change, previous-offset change, anomalous point, anchor inversion, and missing point.

For a real GPU trial: disable autoload, reset the GPU tuning state using your *known safe* method, and apply only a conservative profile. Review kernel logs before attempting any aggressive frequency/voltage settings. Never run `nvcurve` operations against a GPU already experiencing recovery, GSP faults, or Xid errors.
