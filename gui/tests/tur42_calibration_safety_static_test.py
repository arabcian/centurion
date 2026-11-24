#!/usr/bin/env python3
"""Source contracts, not hardware or Rust/Qt execution."""
from pathlib import Path
root = Path(__file__).resolve().parents[2]
def read(p): return (root / p).read_text()
c = read('crates/centurion-helpers/src/bin/centurion-calibrate.rs')
t = read('crates/centurion-helpers/src/bin/tune-helper.rs')
a = read('crates/centurion-helpers/src/abverify.rs')
s = read('crates/centurion-helpers/src/calctx.rs')
g = read('gui/src/costrategy.cpp')
n = read('crates/nvcurve/src/bin/nvcurve-root-helper.rs')
o = read('crates/nvcurve/src/ops.rs')
assert c.index('self.entries.extend(pending)') < c.index('self.save() { self.entries.truncate') < c.index('for (f, data, stamp) in &writes { Self::write')
assert 'if failed.is_empty() { Vec::new() } else { original.clone() }' in c
assert 'duplicate journal path' in c and 'write_scoped_storage(t, path, data' in c
assert c.index('Some(calibration_lock()') < c.index('Calibration::load_checked()')
assert 'calibration_recovery_pending()' in t and 'abverify::authorize(name, values)' in t
assert 'coverage_confirmed && context_confirmed' in c and 'per-device workload binding is not implemented' in c
assert 'records.iter().rev().find' in a and 'baseline(values)? != record["baseline"]' in a
assert 'if !out.status.success()' in s and 'scene original or target value unavailable; no write' in s
assert 'validTopology(cores_, limit_)' in g and 'v <= all_.floor' in g and 'values[i] <= per_[i].floor' in g
assert n.index('started while waiting for GPU lock') > n.index('let _lock = match ops::gpu_write_lock()')
assert 'gpu::curve_device_identity(gpu_index, g, &name).ok().as_ref() != Some(&identity)' in o
print('Tur42 source safety contracts: PASS (static only)')
