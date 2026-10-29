# centurion-gpustress — specification

Health → Curve stability → **GPU test**: a vendor-neutral Vulkan tool that checks whether a GPU core
curve (undervolt / overclock) or a VRAM clock offset is stable, by doing work whose correct result is
known and counting every wrong bit. It sits next to the existing Memory test (`gui/src/memtest.*`,
`gui/src/memtestbox.*`) and follows the same shape.

## 0. Ground rules for the implementer

- **Clean-room.** Write this from this document only. Do not read, copy or translate the source of any
  existing GPU memory tester or GPU stress tool. If something here is unclear, choose and document the
  choice in the code; do not go and look at how another tool does it.
- License: `GPL-3.0-or-later` like the rest of the workspace. Every new dependency must be
  GPLv3-compatible (MIT, Apache-2.0, BSD, zlib). Planned: `ash` (MIT OR Apache-2.0), `naga`
  (MIT OR Apache-2.0, build-dependency only), `serde_json`, `libc` (already used).
- Runs **as the user**, never via pkexec. No root, no sysfs writes. It does not touch clocks or curves;
  it only loads the GPU while the user's curve is in effect.
- Match the repo: small, plain code; no abstractions for one use; comments state *why*.

## 1. Layout

- New crate `crates/centurion-gpustress` (own crate so the helpers do not link Vulkan), one binary
  `centurion-gpustress`. Add it to the workspace members and to `packaging/install-files.sh`
  plus the Arch / Fedora / Gentoo packaging next to `centurion-cooltest`.
- Vulkan is loaded at run time (`ash::Entry::load()`), so building needs no Vulkan SDK and a machine
  without `libvulkan.so.1` gets a clean `init_failed` instead of a link error.
- Shaders are WGSL files in `crates/centurion-gpustress/shaders/`, compiled to SPIR-V in `build.rs`
  with `naga` and embedded with `include_bytes!(concat!(env!("OUT_DIR"), …))`. No external shader
  compiler, offline/vendored distro builds keep working, and the shader source is what is shipped.
  Only 32-bit integer and f32 operations; no extensions beyond Vulkan 1.1 core.
- GUI: `gui/src/gpustress.{h,cpp}` (logic, no widgets, unit-tested in `gui/tests/gpustress_test.cpp`)
  and `gui/src/gpustressbox.{h,cpp}` (the box on the Curve stability page), wired like `MemTestBox`.

## 2. Command line

```
centurion-gpustress list
centurion-gpustress vram --device <pci> --percent <10..95> --seconds <s>
centurion-gpustress core --device <pci> --seconds <s>
```

- `<pci>` is the PCI address as in sysfs, e.g. `0000:01:00.0`. Devices are matched through
  `VK_EXT_pci_bus_info`; when a driver lacks it, by `vendorID`/`deviceID` against
  `/sys/bus/pci/devices/<pci>/{vendor,device}`, and only if that match is unique.
  Laptops have two GPUs (iGPU + dGPU): never pick "the first device" implicitly.
- `list` prints one JSON array: `[{"pci","name","vendor_id","device_id","type":"discrete|integrated|other","vram_mib","driver"}]`.
  CPU implementations (llvmpipe / lavapipe) are listed with `"type":"cpu"` so tests can use them; the GUI hides them.
- `--seconds` is the test time after start-up. The tool ends itself at the deadline (finishing the
  current pass is not required; stop at the next submission boundary).
- SIGTERM / SIGINT: stop at the next submission boundary, print the `end` line with `"status":"stopped"`.

## 3. Output (stdout, one JSON object per line, flushed per line)

```
{"ev":"start","mode":"vram","pci":"…","name":"…","driver":"…","bytes":N,"chunks":K}
{"ev":"progress","t":12.0,"passes":40,"bytes_checked":N,"errors":0}       ← at most once per second
{"ev":"error","t":…,"pass":P,"pattern":"addr|walk1|walk0|solid|random|core","word":W,"expected":X,"actual":Y}
{"ev":"end","status":"pass|fail|stopped|device_lost|init_failed","t":…,"passes":P,"errors":E,"msg":"…"}
```

- `expected` / `actual` are 32-bit values as `"0x%08x"` strings. For `core` mode `expected` is the
  reference run's value (see §5), not a mathematically known value.
- At most 16 `error` lines per pass and 200 per run; `errors` in `progress` / `end` is the full count.
- Diagnostics go to stderr, never stdout.
- Exit code: 0 pass, 1 errors found, 2 device lost, 3 init failure or bad arguments, 4 stopped.
  `status` and the exit code always agree.

## 4. VRAM mode

**Allocation.** Pick the `DEVICE_LOCAL` heap of the device. Budget = heap budget from
`VK_EXT_memory_budget` when present, else the heap size; target = `percent` of it, but never more
than the budget minus 256 MiB (the desktop is using that GPU too). Allocate in chunks of
`min(256 MiB, maxStorageBufferRange, maxMemoryAllocationSize)`; one `VkBuffer` + memory per chunk.
On `VK_ERROR_OUT_OF_DEVICE_MEMORY` stop allocating and test what was obtained; under 64 MiB is
`init_failed` ("could not allocate enough VRAM"). Report the real size in `start`.

**Passes.** One pass = one pattern over all chunks: a *write* dispatch fills every 32-bit word, a
barrier (`SHADER_WRITE` → `SHADER_READ`), then a *check* dispatch recomputes the expected value of
each word, compares, `atomicAdd`s a mismatch counter and stores the first 16 mismatches
(word index, expected, actual) into a small host-visible result buffer. The value of a word depends
only on `(pattern, global word index, pass seed)` — stateless, so the check needs no copy of the data.
Global word index = chunk index × words per chunk + word offset (so neighbouring chunks differ).

Patterns, cycled in this order; `seed` = pass number:
1. `addr`   — `index ^ hash(seed)`: catches address-line faults (two addresses landing on one cell).
2. `walk1`  — `1u << ((index + seed) % 32)`: one set bit walking across bit lanes.
3. `walk0`  — the complement of `walk1`.
4. `solid`  — alternate passes of `0x00000000` and `0xffffffff` written, checked, then the inverse
   written and checked within the same pass (two write/check rounds; catches bits stuck or slow to flip).
5. `random` — `hash(index, seed)` with a 32-bit integer mixing function (e.g. a PCG-style or
   murmur-finaliser-style hash; pick one, write it in both WGSL and Rust).

**Submission size.** Each `vkQueueSubmit` covers one chunk for one dispatch and is waited on with a
fence before the next, so no single submission comes near a driver watchdog (desktop GPUs reset a
queue after ~2–5 s). Each invocation handles a fixed small number of consecutive words
(e.g. 4 × `u32` per invocation, workgroup size 256); dispatch counts are within `maxComputeWorkGroupCount`.

## 5. Core mode

Goal: keep the shader cores at full clock with long dependent arithmetic and detect any wrong result.

- Output buffer: `invocations × 4` words, `invocations` = 1 Mi (fills any current GPU). Workgroup 256.
- Each invocation runs a loop of `n` iterations over four independent accumulators (ILP), mixing
  f32 FMA and integer ops, e.g. per iteration and per accumulator:
  `f = fma(f, a, b)` with `a` in [0.90, 0.999] and small `b` derived from the invocation id, so values
  stay finite and never reach denormals; `u = (u * 0x9E3779B1u) ^ (u >> 15) ^ bitcast<u32>(f)`.
  At the end write the four `u` values. Exact formula is the implementer's choice; requirements are:
  bounded (no inf/NaN/denormal), every step depends on the previous one, both f32 and integer units used.
- **Reference.** A deterministic shader on correct hardware gives the same bits every time with the
  same pipeline and inputs. So: run the dispatch twice at start; if the two agree, that result is the
  reference. If they disagree, the GPU is already unstable: count the differing words as errors and
  retry up to 3 times to get two agreeing runs; if none agree, `end` with `fail`.
  Every later run is compared word by word with the reference (on the GPU, same counter / first-16
  scheme as §4). Seed changes every 60 s (new reference pair) so a fault tied to one data set is not missed.
- **Calibration.** Start with `n = 1024`, time one dispatch, scale `n` so one dispatch takes ~100 ms,
  cap at 500 ms. Recalibrate when the seed changes (clocks may have settled).

## 6. Failure handling

- `VK_ERROR_DEVICE_LOST` from any call: print `end` with `device_lost`, exit 2. Do not try to
  recreate the device. (A lost device under a curve is itself a failed test.)
- Allocation / pipeline / instance failure before the first pass: `init_failed`, exit 3, `msg` says what.
- No panics on the expected failure paths; `panic = "abort"` is set workspace-wide.

## 7. GUI (`gpustress.{h,cpp}`, `gpustressbox.{h,cpp}`)

Mirror `memtest.h`:

- `struct Plan { QStringList args; QString error; }` and `plan(mode, pci, percent, seconds)`.
- Device choice: from `centurion-gpustress list`, hide `cpu` type, preselect the discrete GPU.
  Do not offer the test while the dGPU is unusable (the condition the curve guard already uses).
- `struct Run` fed line by line (`feed`): errors, passes, last status, device-lost flag.
- `verdict(run, exitCode, crashed, cancelled, gpuFaults)` → Pass / Fail / Incomplete:
  - Fail: any error, `device_lost`, or `gpuFaults > 0`.
  - Incomplete: cancelled by the user, crashed, or no `end` line.
  - Pass: `end` status `pass` and exit 0.
  An error found is a failure however the run ended (same rule as the memory test).
- `gpuFaults`: `CurvePage::onFault` forwards events to the box as it does for `MemTestBox`; during a
  run count `xid` (except the follow-ups 45 and 154, as elsewhere) and `amdgpu` events.
- Deadline: the tool ends itself; if it is still running `seconds + 120` s after start, kill it and
  record Incomplete (same idea as `STUCK_GRACE_S`).
- Results: `$XDG_STATE_HOME/centurion/gpustress.json`, JSON array, newest 50, each with mode, pci,
  device name, size, seconds, passes, errors, verdict, and the GPU curve state in effect at start
  (taken the same way Curve stability attributes states from `oc-journal.jsonl`).
- Suggested defaults: VRAM 80 %, 10 min; Core 10 min. Text tells the user to run VRAM after a memory
  offset change and Core after a core curve change, and that a pass is evidence, not proof.

## 8. Tests

- Rust unit tests: pattern functions and the hash (the Rust copies used for documentation/tests must
  match the WGSL — test known input/output pairs in both by running the shader on lavapipe, below).
- Rust integration test (`tests/lavapipe.rs`): if a `cpu`-type device is listed (Mesa lavapipe), run
  `vram --percent 10 --seconds 3` and `core --seconds 3` and assert `status == "pass"`; skip with a
  message when no such device exists.
- Fault injection behind a cargo feature `fault-inject` (off in release): after the write dispatch of
  pass 3, `vkCmdFillBuffer` flips one word; the integration test asserts exactly one `error` line with
  the right word index and `status == "fail"`, exit 1. Core mode: perturb one invocation's input for
  one run; assert errors > 0.
- GUI: `gpustress_test.cpp` like `memtest_test.cpp` — plan arguments, line parsing, every verdict
  branch (including "error then cancelled" = Fail and "fault event, zero errors" = Fail).

## 9. Docs

Add a **GPU test** paragraph to `docs/TECHNICAL.md` next to **Memory test**, and a short user-facing
note in `HANDBOOK.md` where curve testing is described.
