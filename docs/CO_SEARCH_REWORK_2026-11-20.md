# CO Search rebuild — 20 November 2026

> **Superseded in part** by `CO_SEARCH_PERCORE_RULES_2026-11-20.md` (per-core search rules, crash guard, reboot lockout).

## New behavior

- **Per-core** is the default for new CO Search settings. Prior saved mode/limit/level/scene preferences still load.
- Per-core begins at **-15** per physical core (clamped to the configured limit) instead of an all-core presearch from -5. It alternates CCDs and programs every peer core to **0 (stock)** while testing one core, so earlier offsets cannot confound isolation.
- Passes descend by **5 CO points** until a failure or the limit; a failure produces a pass/fail bracket, refined by **binary search to 1 point**. If -15 fails, it searches back toward stock. This does **not** assume all CPUs can use -15.
- **Quick probes** run `centurion-cpustress percore --load co --seconds 24` (12 s steady FFT/NTT work, then all 10 load-transition patterns for 1.2 s each). Fast/Medium/Long use 1/2/3 cycles. A final verification applies the entire curve and uses full 60 s cycles on each core individually, then all cores at once. Medium and Long increase verification cycles; passing is not a guarantee of long-term stability.
- All-core mode retains its own preexisting 60-second tests and coarse/fine search. All-core result saving and reset paths are unchanged.
- CPU sensor availability and user-set CO Search temperature stop limits have been removed, including the settings and session journal fields. CPU firmware thermal limits and throttling remain unchanged. Throttling events are recorded but **do not stop** the search. A kernel-classified **critical thermal emergency** still aborts it. Fault checking, MCA counters, root helper guard, process timeout, and stock reset remain.
- Writes now require an **atomic committed recovery journal** before the helper receives a candidate, rather than proceeding even when journal creation failed.
- Search JSON snapshots are now **version 2**. Incompatible old interrupted searches cannot be resumed safely and are not deleted without a user decision.
- Failed tests that name a CPU outside the isolated target terminate as **unattributable hardware faults** instead of incorrectly narrowing the target core's boundary.

## Implementation

`gui/src/costrategy.{h,cpp}`: isolated per-core state machine, binary search, time estimates, versioned snapshots; `gui/src/cosearchdialog.{h,cpp}`: UI and temperature handling, snapshot durability, isolation, journal and results; `crates/centurion-cpustress/src/{main.rs,co.rs}`: 24-second cycle; `gui/tests/costrategy_test.cpp`: strategy simulations; `gui/tests/co_walk_native_test.py`: a Qt-independent C++ algorithm test over every supported limit and stable boundary. The older `gui/tests/six_features_static_test.py` expectations have been updated.

## Source checks used here

- `python3 gui/tests/co_walk_native_test.py` — passes 1,311 native C++ limit/boundary combinations using the actual `coreCandidate()` function.
- Six other GUI source/i18n test scripts pass.
- Qt GUI CTest tests and Rust `cargo test` **could not run in this build container** because Qt6 development packages and Rust/Cargo are missing. This work has **not** been tested by executing undervolt writes on a Ryzen system. Validate on a Gentoo machine before relying on the results.

## Recommended full build/test on Gentoo

```bash
# from project root; install Qt6 development packages and Rust >= 1.89 first
cargo test -p centurion-cpustress
cmake -S gui -B gui/build -DCENTURION_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build gui/build --parallel
ctest --test-dir gui/build --output-on-failure
python3 gui/tests/co_walk_native_test.py
```

**Important:** Reinstall `centurion-cpustress` together with the changed GUI. An old `centurion-cpustress` binary does not understand `--load co --seconds 24`. Follow the project's `install.sh` for the normal deployment of all components. Reset CPU CO to stock before starting. Save unsaved work and keep the machine on AC power for crash testing.
