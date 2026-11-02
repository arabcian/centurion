#!/usr/bin/env bash
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
mkdir -p build-verification
logdir="$PWD/build-verification"
fail=0
run() { local name="$1"; shift; printf '\n=== %s ===\n' "$name"; ("$@") 2>&1 | tee "$logdir/$name.log"; local rc=${PIPESTATUS[0]}; printf '%s exit=%d\n' "$name" "$rc" | tee -a "$logdir/status.txt"; if ((rc)); then fail=1; fi; }
: > "$logdir/status.txt"
if command -v cargo >/dev/null 2>&1; then
  run cargo-check cargo check --workspace --all-targets --locked
  run cargo-test cargo test --workspace --locked
  run cargo-clippy cargo clippy --workspace --all-targets --locked -- -D warnings
else echo 'Rust check unavailable: cargo not installed' | tee -a "$logdir/status.txt"; fail=1; fi
if command -v cmake >/dev/null 2>&1; then
  run cmake-config cmake -S gui -B build-verification/gui -DCMAKE_BUILD_TYPE=Debug '-DCMAKE_CXX_FLAGS=-Wall -Wextra -Wpedantic'
  if [[ -f build-verification/gui/Makefile || -f build-verification/gui/build.ninja ]]; then
    run cmake-build cmake --build build-verification/gui --parallel 2
  fi
else echo 'CMake unavailable' | tee -a "$logdir/status.txt"; fail=1; fi
printf '\nLogs: %s\n' "$logdir"
exit "$fail"
