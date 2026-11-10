# NVcurve Xid 154 hardening — 2026-11-04

The changes in this branch target **mixed GPU state / repeated writes / unsynchronized private register probing** during GUI, CLI, and OpenRC autoload profile application. They cannot prove that all NVIDIA Xid 154 failures originate in NVcurve. Xid 154 may report a prior UVM/GSP/driver fault, and a driver in a fatal state may require a reboot.

## Changes

1. **Common profile apply path**: GUI root helper, CLI `nvcurve profile apply`, and OpenRC autoload call `ops::apply_profile_verified`, invoking the same single-attempt `profiles::apply::apply_profile` transaction. The existing root cross-process `flock` serializes writers. **Do not blindly reapply on an Xid GPU**.
2. **No reset→apply gap**: the profile constructs one complete masked ClockBoostTable write. Unspecified GPU core offsets and stale active memory-domain CT offsets are zeroed in the *same* request; memory frequency offset is controlled via NVML. Any unknown or masked-out populated offset triggers a failure rather than being silently ignored.
3. **Flatten safety preflight**: the previously missing `hal::flatten_guard` now validates voltage/index/domain consistency, local frequency slope and flatten-anchor continuity, per-point target projections, two independent driver reads, and a last-minute table comparison. Its checks are conservative and may reject a profile on a driver that changes frequency bins or normalizes offsets; no offset is written on preflight rejection.
4. **Strict readback**: every requested point must be present and match exactly. Zero readback for a negative offset is **not** success. The curve is checked again after NVML setters. No automatic retries of an unsuccessful GPU write.
5. **Best-effort verified rollback**: restores the saved CT deltas with a fresh live table and confirms them by readback; restores previously known NVML memory offset and power limit if those changed. Existing NVML clock-lock ranges cannot be read back and therefore **cannot be guaranteed to be reconstructed**; the code releases locks and reports the incomplete rollback if necessary. On a fatal driver error, even CT rollback can fail and must not be described as successful.
6. **Reset and snapshots**: GUI/CLI curve reset verifies the written zeros and attempts a verified CT rollback when unsuccessful; reset-all stops if CT reset fails before modifying NVML offsets. Snapshot restore validates binary size, version, metadata, GPU identity/UUID (when saved), masks, per-point limits and reads back the restored values. Driver-private words in old snapshot buffers are never replayed as raw requests.
7. **Blackwell register sensors locked against writes**: hotspot and GDDR7 partition reads stay on by default. The CLI, `nvcurve-sensors` (nonblocking: busy samples are skipped) and the root helper take the cross-process GPU write lock, so no register read overlaps an nvcurve CT write.
8. ~~**NvAPI version size check**: an experimental V3 structure cannot be represented in the 16-bit length field (88,844 > 65,535). It is not issued.~~ **Wrong — reverted 2026-11-07, see "Correction" below.**
9. **GUI profile decoding**: malformed, fractional, non-finite, out-of-range or non-integer per-point JSON offsets are rejected before numeric conversion.

## Verification and limitations

- `python3 tests/nvcurve_safety_static_test.py`: 13/13 **source contract** checks pass (not a Rust compiler or a GPU test).
- Independent GUI static tests pass for experimental layout, experimental UI, Health and Studio V4.
- Turkish translation catalogue test fails **unchanged on the original archive** (separate localization issue).
- The editing container has no `rustc`, `cargo`, or Qt6 development headers, so **no Rust unit test, Qt compilation or NVIDIA hardware validation was performed**. A source-only patch is **not** a verified production binary.

## Safe Gentoo/OpenRC validation

1. Disable boot application during evaluation: `sudo rc-update del nvcurve-autoload default` (and `sudo rc-service nvcurve-autoload stop` if running).
2. Build on your Gentoo machine using the project's ordinary installation/build workflow. Before enabling autoload, run `cargo test -p nvcurve` and `cargo test --workspace --locked`; then build the Qt GUI via the documented CMake/Qt6 dependencies.
3. Keep the private register probe opt-in **unset**. Verify `nvcurve read --json` and `nvcurve sensors --json` are read-only. Observe the GPU at stock settings over multiple clean boots.
4. With an idle GPU and after saving a rollback snapshot, apply a **conservative known-stable profile manually**, once. Read back the V/F table and check kernel logs. A hard failure or `Xid 154` means stop; **do not repeatedly retry** the same profile in the same boot.
5. Only after repeatably stable results should you consider turning on the OpenRC autoload service. A 2692-MHz flatten/240-MHz positive-offset profile is not validated as electrically safe merely because it passes source-side checks.

No GPU or user configuration files were modified by the offline code patch. Existing profiles are not overwritten or automatically re-enabled.

## Correction — 2026-11-07 (Xid 154 / UVM channel error at the game-start scene switch)

Profiles that were stable before this branch crashed at the scene switch when a game started. Five changes in this branch were wrong or unnecessary:

1. **V3 base tuples were switched off by mistake.** NVIDIA's `MAKE_NVAPI_VERSION` is `sizeof | (ver << 16)` in one u32. The driver builds its own constant the same way, so the 88,844-byte request is `0x35B0C`, the word earlier builds sent. Without the base tuples every base became `freq − delta`. That value is wrong on capped tops and on points the driver holds ~1000 MHz below stock (the GUI's `takeGpuPoints` documents both cases). The curve is now read under the previous scene's core cap, so flatten offsets could be computed too high, and the flatten guard can't see it because it uses the same base. `nvapi::version_word` restores the request. V3 bases are used only if they match the V1 status on unmodified, non-plateau points (`vfcurve::v3_agrees`).
2. **The rollback left the previous curve uncapped.** NVML can't read the previous core cap back. Releasing the lock over the old curve brought back a curve that may have depended on that cap, with its high-voltage points overclocked. A rollback that changed the core cap now goes to **stock** (verified), and the cap is released only after that.
3. **The VRAM lock was reset on every apply** when the profile had none. That contradicts the GUI (`mirrorProfileLimits`: "otherwise leaves it") and adds a memory-clock change at every scene switch. It is now left alone unless the profile sets one.
4. **A profile without `power_limit_w` forced NVML's default power limit.** That overrode the cTGP / Dynamic Boost that the scene's firmware step had just set, or failed and rolled the whole curve back. GUI profiles never set a power limit, so it is now changed only when the profile names one. The core cap is now the last NVML write.
5. **Every apply rewrote every CT point**, including all memory-domain points already at 0. Only points whose stored delta differs are sent now; verification still covers all of them. Re-applying the active curve writes nothing.
