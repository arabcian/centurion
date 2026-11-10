# NVcurve V/F transactional safety — Xid 154 investigation patch

This branch keeps the **existing flatten math** (target kHz minus offset-free base kHz) and adds a read-only preflight, a single prepared ClockBoostTable write, strict read-back, and best-effort rollback. This is a *defensive patch*, not proof that any GPU overclock or undervolt is electrically stable.

## What changed

- `flatten_guard.rs` compares two successive driver reads. It checks point count, identity, domain, voltage and previous CT offset; tolerates up to 45 MHz base-clock drift; checks the projected plateau, its preceding anchor, and folds the offsets introduce into the V/F curve (the stock curve's own shape is never judged: Blackwell has 150-200 MHz jumps at one voltage). It rejects anomalies **before any profile GPU write**.
- One shared operation is used for GUI helper, CLI profile apply and boot autoload. Each already holds `/run/nvcurve.lock`. No reset-to-stock gap and no automatic retry after a failed OC write.
- The same ClockBoostTable write includes every programmable core point and zeroes memory-domain CT offsets. Old nonzero CT points outside the populated VFP curve are either explicitly zeroed (if in the device mask) or rejected. Profiles with nonzero memory offset in other P-states are rejected instead of silently compounding offsets.
- Read-back comparisons are exact; missing points, unexpected positive offsets or clamped negative deltas are treated as errors. NVML memory offset and power limits are also verified when queryable. The clock-lock API does **not** disclose the previous lock range, so previously set locks cannot be restored exactly after an error. This is reported in the error, not hidden.
- Snapshots are restored only if metadata matches the GPU (UUID when available, otherwise name), the point mask and validated offsets. No raw snapshot driver control bytes are replayed.
- GUI reset, CLI `write --reset`, CLI `write --reset-all`, and `reset-all` now verify CT results and try to roll back incomplete writes.
- Blackwell `REGISTER_OP` sensor reads (hotspot, GDDR7 partitions) are on, as before. The CLI, sensor helper and root helper take the process-level GPU write lock for them, so they never sample during an nvcurve write. This cannot serialize with third-party GPU tools.
- ~~The experimental V3 struct is 88,844 bytes, greater than the 16-bit NVAPI version size field. The invalid request is no longer sent.~~ Wrong: the version word is `sizeof | ver<<16`, the same as NVIDIA's macro (`0x35B0C`). The V3 request is sent again, and its bases are cross-checked against the V1 status (see NVCURVE-XID154-HARDENING.md, "Correction").

## Important limitations

1. There is **no compatible Rust/Cargo toolchain in the patch authoring environment**. Static source checks passed (`12/12`), but **Rust compilation, all Rust unit tests, and real NVIDIA hardware operation remain unverified**. Build on Gentoo before considering any GPU write.
2. NVIDIA UVM Xid 154 is a recovery requirement, **not** a diagnosis of its initiating failure. A correct CT write can still overclock an unstable device; NVIDIA driver/GSP, firmware, UVM and other tuning software remain possible sources.
3. Two coherent VFP reads cannot establish electrical stability. In particular, the V1 fallback can be misleading if the driver clamps frequencies. Conservative profiles and a separate workload stability test are still required.
4. Rollback is best-effort and can fail if the GSP/UVM is already faulted. NVML does not expose prior GPU and memory clock-lock values; a failed transaction may release those locks rather than restore their previous ranges. The returned error states this.
5. Old snapshots without metadata are intentionally rejected. If a profile's GPU name differs from the device, or an unsupported P-state cannot be verified, application fails closed instead of silently applying a possibly incorrect setting.

## Gentoo verification

Run from the repository root:

```bash
python3 tests/nvcurve_safety_static_test.py
cargo test -p nvcurve --lib
cargo test -p nvcurve --bins
cargo build --release -p nvcurve
```

The Python tests are source-level checks, not a Rust parser or GPU simulation. A successful `cargo build` is still not proof of GPU safety.

To isolate the startup fault, disable boot autoload before deploying the test build (do not re-enable until you have established stability):

```bash
sudo rc-update del nvcurve-autoload default
```

Check `dmesg` for NVRM/Xid and UVM faults after each controlled boot. Do not repeatedly reapply a profile to a GPU in Xid 154 recovery state. The particular `EXPERIMENTAL` profile with +240 MHz below the index-72 flatten and a 2692 MHz flat top may be *rejected* if the actual driver V/F anchor is higher than the plateau; this is an intentional safety failure, not a compatibility bug.
