# NVcurve Xid 154 safety modifications (2026-11-04)

Scope: the uploaded `centurion-nvcurve-test.zip`, modifying only NVcurve-related Rust, the NVIDIA-tab JSON offset validation, and safety documentation/tests.

| Component | Change |
|---|---|
| `crates/nvcurve/src/hal/flatten_guard.rs` | New consistency guard; two driver reads; plateau, anchor and local V/F anomaly checks |
| `crates/nvcurve/src/hal/vfcurve.rs` | Reject invalid V3 struct-size request; checked flatten arithmetic; prepared CT writer; exact and missing-point-sensitive readback |
| `crates/nvcurve/src/nvapi.rs` | Refuse versioned request sizes exceeding the low 16-bit field |
| `crates/nvcurve/src/profiles/apply.rs` | Shared transactional apply with preflight, one CT write, readback before NVML memory writes, final verification, best-effort rollback, detection of stale memory offsets and unrepresented CT points, no blind retries |
| `crates/nvcurve/src/ops.rs` | Unify apply callers; no reset interlude; verified curve reset path for GUI and CLI; fail closed during reset-all |
| `crates/nvcurve/src/hal/sensors.rs` | Blackwell REGISTER_OP disabled unless explicitly enabled by `NVCURVE_UNSAFE_REG_SENSORS=1` |
| `crates/nvcurve/src/bin/{nvcurve,nvcurve-sensors,nvcurve-root-helper}.rs` | Shared path, skip opt-in sensor samples when the GPU write lock is busy, verified reset |
| `crates/nvcurve/src/hal/snapshot.rs` | GPU metadata / mask / offset validation; replay checked offsets only; readback and rollback attempt |
| `gui/src/nvidiatab.cpp` | Validate malformed, nonintegral and out-of-range JSON offsets before conversion; remove direct private Blackwell register reads |
| `tests/nvcurve_safety_static_test.py` | 13 GPU-free source regression tests |

## Verification performed in the authoring environment

- `python3 tests/nvcurve_safety_static_test.py`: **13 passed**.
- Rust unit tests were authored for flattened preflight, memory CT clearing, invalid profile indices and exact readback, but **not executed**. `cargo` and `rustc` were not installed and there is no GPU device passed through to the environment.
- No system services were modified, no profiles were autoloaded, and no NVIDIA hardware registers were written here.

## Hardware safety disclosure

This patch **reduces known software hazards** but does not prove that Xid 154 is caused by NVcurve or that an existing OC is stable. `gpu_clock_cap_mhz=2692` with `gpu_clock_comp_mhz=100` requests a cap of 2792 MHz, not 2692 MHz; the patch now reports that difference as a warning. Clock lock restoration is not guaranteed because NVML does not expose the former lock ranges. The V3 offset-free base query is unavailable with this declared struct and is intentionally suppressed rather than sending a truncated/invalid NvAPI request.

**First test**: keep `nvcurve-autoload` disabled, build/test the source, use a conservative profile only if the GPU is currently healthy, and inspect kernel logs before any subsequent trial.

## Gentoo / Rust 1.98.1 compile fix (2026-11-04)

- Parenthesized two cast-before-`<` expressions in `hal/flatten_guard.rs` to prevent Rust from parsing the comparison as a const-generic argument (reported as seven cascading errors).
- Removed unused `load_profile` and `profile_path` imports from `ops.rs`.
- Added static regression checks for this cast/comparison parsing hazard.
- This is a targeted fix for the uploaded build log; native Cargo/Qt compilation is **not** available in the patching environment. Run `cargo build --release --frozen` and the Gentoo ebuild on the target system to confirm and reveal any later errors.
