#!/usr/bin/env python3
"""Source-level regression contracts for audit fixes.

These tests do NOT execute Rust or access hardware. Full verification needs
cargo test, clippy and driver read-back on a supported Lenovo/NVIDIA machine.
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]

def code(path):
    return (ROOT / path).read_text()

class SourceGuardrails(unittest.TestCase):
    def test_memory_write_overflow_checked_before_any_gpu_access(self):
        s = code('crates/nvcurve/src/hal/limits.rs')
        fn = s[s.index('pub fn set_clock_offsets('):s.index('pub fn reset_all_clock_offsets(')]
        self.assertIn('let mem_raw = mem_offset_mhz.map', fn)
        self.assertLess(fn.index('let mem_raw ='), fn.index('let n = nvml::ready()?'))
        self.assertNotIn('let raw = v.checked_mul(2)', fn)

    def test_no_success_on_gpu_write_without_mask_and_readback(self):
        s = code('crates/nvcurve/src/hal/vfcurve.rs')
        fn = s[s.index('pub fn write_offsets('):s.index('fn write_domain(')]
        self.assertIn('let invalid = crate::safety::validate_write', fn)
        self.assertIn('Cannot validate negative delta floor:', fn)
        self.assertIn('Cannot validate GPU V/F point mask:', fn)
        self.assertIn('ClockBoostTable changed after the supplied baseline', fn)
        self.assertLess(fn.index('get_boost_mask(gpu)'), fn.index('nvcall_raw(fid::SET_CLOCK_BOOST_TABLE'))
        self.assertLess(fn.index('nvcall_raw(fid::SET_CLOCK_BOOST_TABLE'), fn.index('verify_readback(&deltas, &observed)'))

    def test_reset_surfaces_partial_failures(self):
        s = code('crates/nvcurve/src/hal/limits.rs')
        fn = s[s.index('pub fn reset_all_clock_offsets('):s.index('// ── clock event')]
        for fragment in ['let mut errors = Vec::new()', 'cannot enumerate all memory/graphics P-states',
                         'cannot inspect offset', 'reset read-back is', 'legacy memory reset failed',
                         'incomplete NVML reset:']:
            self.assertIn(fragment, fn)
        self.assertNotIn('unwrap_or_else(|_| vec![0])', fn)

    def test_disk_write_bps_excludes_reads(self):
        s = code('crates/centurion-helpers/src/iorate.rs')
        fn = s[s.index('pub fn passive_bps('):s.index('fn load_probe(')]
        self.assertIn('let (wsect, io_ticks) = (f[6], f[9])', fn)
        self.assertIn('u128::from(wsect) * 512', fn)
        self.assertNotIn('rsect + wsect', fn)
        self.assertNotIn('filter_map(', fn)

    def test_calibration_and_percentile_and_rows_are_bounded(self):
        c = code('crates/centurion-helpers/src/calib.rs')
        b = code('crates/centurion-helpers/src/bench.rs')
        p = code('crates/centurion-helpers/src/par.rs')
        self.assertIn('self.ram_mib.abs_diff(o.ram_mib)', c)
        self.assertIn('.ceil() as usize).saturating_sub(1)', b)
        self.assertIn('assert_eq!(data.len() % row_len, 0,', p)

    def test_invalid_cli_weights_fail_instead_of_falling_back(self):
        s = code('crates/centurion-helpers/src/bin/centurion-autotune.rs')
        self.assertIn('fn parse_weights(s: &str) -> Result<Value, String>', s)
        self.assertIn('autotune::Weights::KEYS.contains(&key)', s)
        self.assertIn('if !n.is_finite()', s)
        self.assertIn('duplicate weight', s)
        self.assertIn('fn rejects_invalid_or_ambiguous_weight_input()', s)

if __name__ == '__main__':
    unittest.main()
