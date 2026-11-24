# Real MainWindow fixture capture (experimental, Tur 39)

The existing `centurion --simulate` is unchanged: it is an isolated presentation workspace, not MainWindow. The separate developer launcher below selects actual production MainWindow, both shell choices, and real page/lazy Health navigation. It is not the PGO training mode and is **not yet a native-verified simulator**. Never launch a live build with PGO flags as an alternative to this sandbox.

## Run (unprivileged Linux x86-64)

Build Centurion normally with Qt 6 and translations. Install `bubblewrap` from your distribution. A user namespace must be permitted by the kernel; do not run the launcher as root. The launcher intentionally refuses to run if isolation is unavailable. Runtime libraries and fonts must be installed in the usual `/usr/lib*`, `/lib*` and system font directories; custom Qt SDK paths may need a later dedicated runtime mapping.

```sh
cmake -S gui -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCENTURION_BUILD_TESTS=ON
cmake --build build
python3 tools/real_shell_fixture.py build/centurion candidate-amd \
  --fixture amd-nvidia --mode experimental --lang en --scale 1 --size 1280x800 --theme graphite
```

Each run needs a fresh candidate directory. `--fixture` supports `amd-nvidia`, `intel-nvidia`, `intel-only`, `amd-radeon`, `missing`, `sleeping-nvidia`. Both `--mode classic` and `--mode experimental`, `--lang en/tr`, scales `1/1.25/1.5/1.75/2`, sizes `760x540/900x650/1280x800`, and themes `graphite/gruvbox/oled-black/pastel-mint-cream` are selectable. TR fails if the compiled catalogue is absent. Actual geometry/DPR and expected page sets are checked. The output manifest identifies the fixture, pages, Health navigation targets, images and blocked central helper requests. The capture loop is bounded; the launcher enforces a process timeout. Current traversal covers enabled top-level tabs, visible nested QTabWidget groups and registered Health targets; arbitrary dialog interactions and nested lazy controls are not exhaustively exercised.

## Boundaries

- The launcher builds synthetic `/sys` and `/proc` trees and mounts them read-only, plus private state, temporary directory, minimal virtual `/dev` and read-only runtime/font directories. The namespace root is tmpfs and remounted read-only. The only ordinary writable host bind is a fresh temporary candidate directory. No host home, real `/sys`, `/proc`, real device nodes, DBus sockets, or session display server are intentionally mounted. The launcher uses offscreen Qt and drops capabilities.
- Internal `--simulate-shell` checks the root/marker/read-only mount types and rejects root or unsupported ABI. Before QApplication it sets no-new-privileges and installs a seccomp filter that denies exec, new sockets/connect, selected kernel mutations and io_uring; a failed installation aborts, never falls back to live mode. This is defense in depth, not a general sandbox for hostile binaries. Use only trusted Centurion builds and trusted system libraries. Host runtime libraries/fonts and kernel information from e.g. `uname` are not deterministic fixture data.
- Central privileged and userhelper calls produce explicit asynchronous failure and increment counters, rather than fabricated success. Other direct subprocess attempts are expected to be blocked by seccomp. The counters are **requests blocked at those two entry points**, not a syscall audit or a proof of zero attempts. Fixture contents are compared before/after successful capture. No mutating controls are invoked by the capture loop. This does not implement synthetic successful helper replies, simulated writes/readback, complete per-subsystem injection, a clock or comprehensive host syscall auditing.
- The T39 environment could not install Qt, Cargo or bubblewrap, so the actual namespace + GUI combination has not been executed. The seccomp test executes the actual filter in an isolated child, and the Python contract tests validate fixtures, command construction and manifest rejection; neither substitutes for native integration. Do not treat this as A07 acceptance or use it to test physical hardware.

## Candidate baseline policy

`--baseline /path/to/reviewed/variant --require-baseline` compares the exact PNG set and SHA-256 bytes. Mismatches fail and keep the candidate output for inspection; no automatic baseline updates. The baseline path should be a reviewed directory for precisely the same fixture/mode/language/DPI/theme/size and Qt/font environment. Exact encoded bytes are intentionally conservative, not a pixel-tolerance or semantic clipping test. Clock and polling variability may require normalization before approval. No approved baseline or successful real-shell screenshot is shipped in T39. A08 remains open: actual pixel diff, stable clock/fonts, full 57 palettes, clipping/focus/contrast and intentional regression fixtures and native CI matrix.
