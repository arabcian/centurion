#!/usr/bin/env bash
# Offline-capable verification wrapper. Device-writing tests are NEVER run here.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
mkdir -p build-verification
logdir="$PWD/build-verification"
fail=0
: > "$logdir/status.txt"
run() {
  local name="$1"; shift
  printf '\n=== %s ===\n' "$name"
  ("$@") 2>&1 | tee "$logdir/$name.log"
  local rc=${PIPESTATUS[0]}
  printf '%s exit=%d\n' "$name" "$rc" | tee -a "$logdir/status.txt"
  if ((rc)); then fail=1; fi
  return "$rc"
}
missing() {
  printf '%s: MISSING REQUIRED TOOL: %s\n' "$1" "$2" | tee -a "$logdir/status.txt"
  fail=1
}

if command -v python3 >/dev/null 2>&1; then
  run python-tests python3 -m unittest discover -s tests -p '*test*.py' -v || :
  run gui-python-tests python3 -m unittest discover -s gui/tests -p '*test.py' -v || :
  run gui-source-checks bash -c 'set -e; for f in gui/tests/*static_test.py; do python3 "$f"; done' || :
else
  missing python-tests python3
fi
run shell-syntax bash -c 'set -e; while IFS= read -r -d "" f; do bash -n "$f"; done < <(find . -type f -name "*.sh" -not -path "./build-*/*" -print0)' || :

if command -v cargo >/dev/null 2>&1; then
  run cargo-format cargo fmt --all -- --check || :
  run cargo-check cargo check --workspace --all-targets --locked || :
  run cargo-test cargo test --workspace --locked || :
  run cargo-clippy cargo clippy --workspace --all-targets --locked -- -D warnings || :
else
  missing rust-toolchain 'cargo + rustc + rustfmt + clippy'
fi
if command -v cmake >/dev/null 2>&1; then
  if run cmake-config cmake -S gui -B build-verification/gui -DCMAKE_BUILD_TYPE=Debug \
      -DCENTURION_BUILD_TESTS=ON -DCENTURION_LTO=OFF '-DCMAKE_CXX_FLAGS=-Wall -Wextra -Wpedantic'; then
    if run cmake-build cmake --build build-verification/gui --parallel 2; then
      run gui-ctest env QT_QPA_PLATFORM=offscreen ctest --test-dir build-verification/gui --output-on-failure || :
    fi
  fi
else
  missing cmake cmake
fi
printf '\nVerification logs: %s\n' "$logdir"
exit "$fail"
