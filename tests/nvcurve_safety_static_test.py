#!/usr/bin/env python3
"""Static, GPU-free regression checks for nvcurve safety patch.
Not a replacement for cargo test or NVIDIA device testing.
"""
from pathlib import Path
import unittest
R=Path(__file__).resolve().parents[1]
def src(rel): return (R/rel).read_text()
class Invariants(unittest.TestCase):
 def test_nvapi_v3_guards_overflow(self):
  s=src('crates/nvcurve/src/nvapi.rs'); v=src('crates/nvcurve/src/hal/vfcurve.rs')
  self.assertIn('if size > u16::MAX as usize',s)
  self.assertIn('if V3_SIZE > u16::MAX as usize',v)
  size=0x68+255*348
  self.assertEqual(size,88844)
  self.assertGreater(size,65535)
 def test_flatten_legacy_math_and_guard(self):
  s=src('crates/nvcurve/src/hal/vfcurve.rs')
  g=src('crates/nvcurve/src/hal/flatten_guard.rs')
  a=src('crates/nvcurve/src/profiles/apply.rs')
  m=src('crates/nvcurve/src/hal/mod.rs')
  self.assertIn('let base = pt.base_freq_khz();',s)
  self.assertIn('pub fn flatten_deltas(curve: &CurveState, from: i64, mhz: i64) -> Result',s)
  self.assertIn('assert_eq!(flatten_deltas(&nob, 72, 2752).unwrap()[&80], 232_000)',s)
  self.assertIn('pub mod flatten_guard;',m)
  self.assertIn('pub fn verify_flatten_preflight(',g)
  self.assertIn('if p.delta_khz != q.delta_khz',g)
  self.assertIn('if projected_khz != target_khz',g)
  self.assertIn('let confirmation = match vfcurve::read_curve(g, &gname)',a)
  self.assertIn('flatten_guard::verify_flatten_preflight(&state, &confirmation, from, mhz, &deltas)',a)
  self.assertLess(a.index('flatten_guard::verify_flatten_preflight'),a.index('vfcurve::write_prepared_offsets(g, &deltas'))
 def test_exact_readback_and_missing_points(self):
  s=src('crates/nvcurve/src/hal/vfcurve.rs')
  self.assertIn('pub fn verify_readback(deltas: &PointDeltas',s)
  self.assertIn('got == expected\n}',s)
  self.assertIn('None => Some(format!("point {p}: missing',s)
  self.assertIn('assert!(!readback_matches(-500_000, 0))',s)
 def test_same_apply_path(self):
  cli=src('crates/nvcurve/src/bin/nvcurve.rs')
  ops=src('crates/nvcurve/src/ops.rs')
  autoload=src('crates/nvcurve/src/profiles/apply.rs')
  self.assertIn('ops::apply_profile_verified(a.gpu(), n, cfg, 1)',cli)
  self.assertIn('let out = apply_profile(gpu_index, name, cfg)?;',ops)
  self.assertIn('match apply_profile(gpu_index, name, cfg)',autoload)
  self.assertNotIn('vfcurve::reset_all_offsets(g, false);\n    if rc != 0 { return Err(format!("Reset failed',ops)
 def test_ct_clear_before_mem_offset(self):
  s=src('crates/nvcurve/src/profiles/apply.rs')
  self.assertLess(s.index('vfcurve::write_prepared_offsets(g, &deltas'),s.index('limits::set_clock_offsets(None, Some(requested_mem), idx)'))
  self.assertLess(s.index('let mismatch = vfcurve::verify_readback(&deltas'),s.index('limits::set_clock_offsets(None, Some(requested_mem), idx)'))
  self.assertIn('if pt.domain == Domain::Memory { deltas.insert(pt.index as i64, 0); }',s)
  self.assertIn('old_offsets.iter().enumerate().skip(state.points.len())',s)
  self.assertIn('previous.retain',s)
 def test_txn_rollback_and_limits(self):
  s=src('crates/nvcurve/src/profiles/apply.rs')
  self.assertIn('revert_profile(g, idx, &previous',s)
  self.assertIn('old_power',s)
  self.assertIn('changed_locks',s)
  self.assertIn('validate_curve_shape(&state, &deltas)',s)
  self.assertIn('nonzero_other_memory_pstates(idx)',s)
 def test_register_probe_opt_in(self):
  s=src('crates/nvcurve/src/hal/sensors.rs')
  self.assertIn('NVCURVE_UNSAFE_REG_SENSORS',s)
  self.assertLess(s.index('if std::env::var("NVCURVE_UNSAFE_REG_SENSORS")'),s.index('let b = nvcall(fid::REGISTER_OP'))
 def test_snapshot_safety(self):
  s=src('crates/nvcurve/src/hal/snapshot.rs')
  self.assertIn('Snapshot metadata has no GPU identity',s)
  self.assertIn('validate_write(&deltas, MAX_DELTA_KHZ)',s)
  self.assertIn('get_boost_mask(gpu)?',s)
 def test_gui_string_khz_offset(self):
  s=src('gui/src/nvidiatab.cpp')
  self.assertIn('value.isString() ? value.toString().toLongLong(&valid)',s)
  self.assertIn('if (!valid)',s)
 def test_source_braces_simple(self):
  # A lightweight quote/comment aware brace scanner. NOT a Rust parser.
  files=['crates/nvcurve/src/nvapi.rs','crates/nvcurve/src/hal/vfcurve.rs','crates/nvcurve/src/hal/flatten_guard.rs',
   'crates/nvcurve/src/hal/sensors.rs','crates/nvcurve/src/hal/limits.rs',
   'crates/nvcurve/src/hal/snapshot.rs','crates/nvcurve/src/profiles/apply.rs',
   'crates/nvcurve/src/profiles/native.rs','crates/nvcurve/src/ops.rs',
   'crates/nvcurve/src/bin/nvcurve.rs']
  for f in files:
   s=src(f); depth=0; status='normal'; i=0; level=0
   while i<len(s):
    c=s[i]; nextc=s[i+1:i+2]
    if status=='normal':
     if c=='/' and nextc=='/': status='line'; i+=1
     elif c=='/' and nextc=='*': status='block';level=1;i+=1
     elif c=='"':status='string'
     elif c=='{':depth+=1
     elif c=='}':depth-=1;self.assertGreaterEqual(depth,0,f)
    elif status=='line':
     if c=='\n':status='normal'
    elif status=='block':
     if c=='/' and nextc=='*':level+=1;i+=1
     elif c=='*' and nextc=='/':level-=1;i+=1;status='normal' if level==0 else status
    elif status=='string':
     if c=='\\': i+=1
     elif c=='"':status='normal'
    i+=1
   self.assertEqual(depth,0,f)
if __name__=='__main__': unittest.main(verbosity=2)
