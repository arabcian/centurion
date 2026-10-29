You are implementing a new feature in Centurion, a Linux control app (Rust helpers in crates/, Qt6/C++ GUI in gui/). The full specification is docs/GPUSTRESS.md. Read it completely before writing any code.

## Clean-room rule (most important)
Implement this from docs/GPUSTRESS.md and the Centurion repository only.
- Do not look up, recall or reproduce the source code of any existing GPU memory tester or GPU stress-test tool. Do not search the web for how such tools are implemented.
- You may read documentation for the libraries you use (ash, naga, Vulkan spec, Qt).
- If the spec leaves a detail open, choose something reasonable yourself and write a one-line comment explaining the choice. Do not fill the gap by imitating another tool.

## Read first, in this order
1. docs/GPUSTRESS.md: the spec.
2. gui/src/memtest.h, gui/src/memtest.cpp, gui/tests/memtest_test.cpp, gui/src/memtestbox.{h,cpp}: the existing Memory test. The GPU test must follow the same structure and style.
3. gui/src/curvepage.{h,cpp}: how MemTestBox is wired in and how onFault forwards events.
4. docs/TECHNICAL.md, the "Memory test" and "Curve journal and stability" paragraphs.
5. Cargo.toml (workspace), crates/centurion-helpers/Cargo.toml, packaging/install-files.sh, gui/CMakeLists.txt.

## How to work
- Keep it simple: minimum code that meets the spec, no abstractions used only once, no options the spec doesn't ask for. Match the existing code style and comment style.
- Touch only what the feature needs. Don't refactor or reformat existing code. If you notice an unrelated problem, mention it at the end. Don't fix it.
- Before you start each stage, state your assumptions in a few lines. If something in the spec is contradictory or impossible (not just unspecified), stop and ask instead of guessing.

## Stages. Finish and verify each one before starting the next.
1. Crate skeleton + `list`: crates/centurion-gpustress, build.rs with naga, `centurion-gpustress list`.
   Verify: `cargo build` and `cargo clippy` are clean; `list` prints valid JSON (on lavapipe if no GPU is available).
2. VRAM mode (§4), including the `fault-inject` feature and tests/lavapipe.rs.
   Verify: the lavapipe test passes; with fault-inject there is exactly one error line, the word index is correct and the exit code is 1; SIGTERM gives "stopped" and exit 4.
3. Core mode (§5), including the reference pair, calibration and seed rotation.
   Verify: the lavapipe test passes; the injected perturbation is detected.
4. GUI: gpustress.{h,cpp}, gpustressbox.{h,cpp}, gpustress_test.cpp, wiring in curvepage and CMakeLists.
   Verify: the GUI builds; ctest passes, including every verdict branch listed in §8.
5. Packaging and docs (§1, §9).
   Verify: install-files.sh and the Arch/Fedora/Gentoo files install the new binary next to centurion-cooltest.

## When you finish
Report, briefly: what was built, every choice you made where the spec was open, the test results, and anything you could not verify (for example, no real GPU available, only lavapipe).
