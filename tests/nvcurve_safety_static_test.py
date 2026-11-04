#!/usr/bin/env python3
"""GPU-free source contract checks. Not a Rust compiler or hardware test."""
from pathlib import Path
import unittest
ROOT = Path(__file__).resolve().parents[1]
def src(p): return (ROOT / p).read_text()
class Invariants(unittest.TestCase):
    def test_nvapi_struct_size_bounds(self):
        n=src('crates/nvcurve/src/nvapi.rs'); v=src('crates/nvcurve/src/hal/vfcurve.rs')
        self.assertIn('if size > u16::MAX as usize',n)
        self.assertIn('if V3_SIZE > u16::MAX as usize',v)
        self.assertEqual(0x68+255*348, 88844)
        self.assertGreater(88844,65535)

    def test_flatten_calculation_is_checked(self):
        s=src('crates/nvcurve/src/hal/vfcurve.rs')
        self.assertIn('pub fn flatten_deltas(curve: &CurveState, from: i64, mhz: i64) -> Result',s)
        self.assertIn('let base = pt.base_freq_khz();',s)
        self.assertIn('target.checked_sub(base)',s)
        self.assertIn('assert_eq!(flatten_deltas(&nob, 72, 2752).unwrap()[&80], 232_000)',s)

    def test_preflight_wired_before_write(self):
        a=src('crates/nvcurve/src/profiles/apply.rs')
        g=src('crates/nvcurve/src/hal/flatten_guard.rs')
        m=src('crates/nvcurve/src/hal/mod.rs')
        self.assertIn('pub mod flatten_guard;',m)
        self.assertIn('pub fn verify_flatten_preflight(',g)
        for phrase in ['if p.delta_khz != q.delta_khz','if projected_khz != target_khz','MAX_BASE_DRIFT_KHZ','MAX_ANCHOR_OVERSHOOT_KHZ','validate_curve_shape']:
            self.assertIn(phrase,g)
        self.assertIn('vfcurve::read_curve_with_raw_ct(g, &gname)?',a)
        self.assertLess(a.index('flatten_guard::verify_flatten_preflight'),a.index('vfcurve::write_prepared_offsets(g, &deltas'))
        self.assertLess(a.index('if latest != previous_offsets'),a.index('vfcurve::write_prepared_offsets(g, &deltas'))

    def test_strict_readback(self):
        v=src('crates/nvcurve/src/hal/vfcurve.rs')
        self.assertIn('pub fn verify_readback(deltas: &PointDeltas',v)
        self.assertIn('got == expected\n}',v)
        self.assertIn('None => Some(format!("point {p}: missing',v)
        self.assertIn('assert!(!readback_matches(-500_000, 0))',v)
        self.assertIn('assert_eq!(verify_readback(&d, &[0]).len(), 2)',v)

    def test_shared_apply_path_and_no_blind_retries(self):
        cli=src('crates/nvcurve/src/bin/nvcurve.rs'); ops=src('crates/nvcurve/src/ops.rs')
        app=src('crates/nvcurve/src/profiles/apply.rs'); root=src('crates/nvcurve/src/bin/nvcurve-root-helper.rs')
        self.assertIn('ops::apply_profile_verified(a.gpu(), n, cfg, 1)',cli)
        self.assertIn('let out = apply_profile(gpu_index, name, cfg)?;',ops)
        self.assertIn('match crate::ops::apply_profile_verified(gpu_index, name, cfg, 1)',app)
        self.assertIn('ops::apply_profile_verified(0, name, &config(), 3)',root)
        self.assertNotIn('vfcurve::reset_all_offsets(g, false);\n    if rc != 0 { return Err(format!("Reset failed',ops)
        self.assertNotIn('std::thread::sleep(',app)

    def test_mem_ct_cleared_and_verified_before_nvml(self):
        s=src('crates/nvcurve/src/profiles/apply.rs')
        self.assertIn('if pt.domain == Domain::Memory { deltas.insert(pt.index as i64, 0); }',s)
        self.assertIn('for (i, &old) in old_offsets.iter().enumerate()',s)
        self.assertLess(s.index('vfcurve::write_prepared_offsets(g, &deltas'),s.index('limits::set_clock_offsets(None, Some(requested_mem), idx)'))
        self.assertLess(s.index('let mismatch = vfcurve::read_clock_offsets(g)'),s.index('limits::set_clock_offsets(None, Some(requested_mem), idx)'))
        self.assertIn('nonzero_other_memory_pstates(idx)',s)

    def test_rollback_of_known_nvml_and_ct(self):
        a=src('crates/nvcurve/src/profiles/apply.rs')
        self.assertIn('revert_profile(g, idx, &previous',a)
        self.assertIn('vfcurve::restore_offsets_verified(g, previous)',a)
        for phrase in ['old_power','old_mem','changed_locks','previous GPU clock-lock range cannot be restored','previous VRAM clock-lock range cannot be restored','Final verification:']:
            self.assertIn(phrase,a)
        self.assertLess(a.index('if changed_locks.memory_offset {'),a.index('vfcurve::restore_offsets_verified(g, previous)'))

    def test_register_probe_on_by_default_and_locked(self):
        for f in ['crates/nvcurve/src/hal/sensors.rs','crates/nvcurve/src/bin/nvcurve-sensors.rs',
                  'crates/nvcurve/src/bin/nvcurve.rs','crates/nvcurve/src/bin/nvcurve-root-helper.rs','gui/src/nvidiatab.cpp']:
            self.assertNotIn('NVCURVE_UNSAFE_REG_SENSORS',src(f))
        self.assertIn('ops::gpu_try_lock()',src('crates/nvcurve/src/bin/nvcurve-sensors.rs'))
        self.assertIn('let sensor_probe',src('crates/nvcurve/src/bin/nvcurve.rs'))

    def test_curve_shape_ignores_stock_shape(self):
        s=src('crates/nvcurve/src/hal/flatten_guard.rs')
        self.assertIn('step < stock_step - MAX_LOCAL_DOWN_KHZ',s)
        self.assertNotIn('MAX_LOCAL_UP_KHZ',s)
        self.assertNotIn('MAX_INTERPOLATION_DEVIATION_KHZ',s)

    def test_snapshot_restore_only_validated_offsets(self):
        s=src('crates/nvcurve/src/hal/snapshot.rs')
        self.assertIn('Snapshot metadata has no GPU identity',s)
        self.assertIn('if got != expected_uuid',s)
        self.assertIn('Snapshot point-mask mismatch',s)
        self.assertIn('validate_write(&deltas, MAX_DELTA_KHZ)',s)
        self.assertIn('vfcurve::write_prepared_offsets(gpu, &deltas',s)
        self.assertIn('vfcurve::restore_offsets_verified(gpu, &old_offsets)',s)
        self.assertNotIn('nvcall_raw(fid::SET_CLOCK_BOOST_TABLE',s)

    def test_reset_readback_and_abort_before_nvml_changes(self):
        s=src('crates/nvcurve/src/ops.rs'); root=src('crates/nvcurve/src/bin/nvcurve-root-helper.rs')
        self.assertIn('pub fn reset_gpu_curve_verified(',s)
        self.assertIn('pub fn reset_vf_offsets_verified(',s)
        self.assertIn('vfcurve::verify_readback(&zero, &now)',s)
        self.assertIn('ops::reset_vf_offsets_verified(a.gpu(), domain)',src('crates/nvcurve/src/bin/nvcurve.rs'))
        self.assertIn('Err(e) => { errs.push(format!("V/F reset: {e}")); return (done, errs); }',s)
        self.assertIn('ops::reset_gpu_curve_verified(0)',root)

    def test_gui_profile_offset_parsing(self):
        s=src('gui/src/nvidiatab.cpp')
        self.assertIn('khz = value.toString().toLongLong(&valid)',s)
        self.assertIn('std::isfinite(raw)',s)
        self.assertIn('std::trunc(raw) != raw',s)
        self.assertIn('if (khz < -2000000 || khz > 1000000) valid = false;',s)
        self.assertIn('if (!valid)',s)
        self.assertIn('return blackwell ? regTemp(0x00AD0AA0) : channel(9);',s)

    def test_snapshot_metadata_required_and_rollback_verified(self):
        s=src('crates/nvcurve/src/hal/snapshot.rs')
        self.assertIn('Snapshot metadata write failed: {e}; discarding incomplete snapshot',s)
        self.assertIn('let _ = std::fs::remove_file(&bin);',s)
        self.assertIn('Snapshot CT write failed {rc}: {reason}; rollback attempt: {rollback:?}',s)
        self.assertNotIn('map(|now| vfcurve::write_prepared_offsets',s)
        self.assertEqual(s.count('vfcurve::restore_offsets_verified(gpu, &old_offsets)'),2)

    def test_rust_cast_comparison_precedence(self):
        # Regression: Rust parses `x as i64 < bound` as a generic type
        # argument without parentheses. This broke the Gentoo 1.98.1 build.
        import re
        for f in (ROOT / 'crates/nvcurve/src').rglob('*.rs'):
            code = f.read_text()
            invalid = re.search(r'\bas\s+(?:u\d+|i\d+|usize|isize)\s*<\s*[^=]', code)
            self.assertIsNone(invalid, f'{f}: cast/comparison precedence requires parentheses')
        guard = src('crates/nvcurve/src/hal/flatten_guard.rs')
        self.assertIn('(p.index as i64) < from', guard)

    def test_ops_no_unused_profile_native_import(self):
        ops = src('crates/nvcurve/src/ops.rs')
        self.assertNotIn('use crate::profiles::native::{load_profile, profile_path};', ops)

    def test_syntactic_braces(self):
        # Quote- and comment-aware nesting heuristic. Not a Rust parser.
        files=['crates/nvcurve/src/nvapi.rs','crates/nvcurve/src/hal/vfcurve.rs','crates/nvcurve/src/hal/flatten_guard.rs',
            'crates/nvcurve/src/hal/sensors.rs','crates/nvcurve/src/hal/limits.rs',
            'crates/nvcurve/src/hal/snapshot.rs','crates/nvcurve/src/profiles/apply.rs',
            'crates/nvcurve/src/profiles/native.rs','crates/nvcurve/src/ops.rs',
            'crates/nvcurve/src/bin/nvcurve.rs','crates/nvcurve/src/bin/nvcurve-sensors.rs',
            'crates/nvcurve/src/bin/nvcurve-root-helper.rs']
        for f in files:
            s=src(f); depth=0; mode='normal'; i=0; level=0
            while i<len(s):
                c=s[i]; ne=s[i+1:i+2]
                if mode=='normal':
                    if c=='/' and ne=='/':mode='line';i+=1
                    elif c=='/' and ne=='*':mode='block';level=1;i+=1
                    elif c=='"':mode='str'
                    elif c=='{':depth+=1
                    elif c=='}':depth-=1;self.assertGreaterEqual(depth,0,f)
                elif mode=='line':
                    if c=='\n':mode='normal'
                elif mode=='block':
                    if c=='/' and ne=='*':level+=1;i+=1
                    elif c=='*' and ne=='/':level-=1;i+=1;mode='normal' if not level else mode
                elif mode=='str':
                    if c=='\\':i+=1
                    elif c=='"':mode='normal'
                i+=1
            self.assertEqual(depth,0,f)

if __name__=='__main__': unittest.main(verbosity=2)
