"""Source guardrails for the 2026-11-19 audit fixes.

These assert that important safety decisions remain wired in. They are NOT a
substitute for cargo test or privileged / hardware integration tests.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def source(relative):
    return (ROOT / relative).read_text(encoding='utf-8')

NET = 'crates/centurion-helpers/src/netguard.rs'
TUNE = 'crates/centurion-helpers/src/tune.rs'
TPROF = 'crates/centurion-helpers/src/bin/tune-helper.rs'
UV = 'crates/centurion-helpers/src/intel_uv_daemon.rs'
UVHELP = 'crates/centurion-helpers/src/bin/intel-uv-helper.rs'
FW = 'crates/centurion-helpers/src/bin/fwattr-helper.rs'
BACKUP = 'crates/centurion-helpers/src/backup.rs'
NV = 'crates/nvcurve/src/hal/vfcurve.rs'


class Audit20261119(unittest.TestCase):
    def test_cen01_guard_uses_non_bypass_queue(self):
        s = source(NET)
        self.assertIn('let guard_queue = format!', s)
        self.assertIn('if cfg.guard { &guard_queue } else { &log_queue }', s)
        self.assertIn('queue num {QUEUE_NUM}\\n', s)

    def test_cen02_queue_opens_before_rules_are_replaced(self):
        s = source(NET)
        run = s[s.index('pub fn run_daemon()'):s.index('// ── helper ops')]
        self.assertLess(run.index('let queue = match Queue::open()'),
                        run.index('match load_or_degrade(&cfg'))
        self.assertNotIn('nft_remove();', run[:run.index('let queue = match Queue::open()')])

    def test_cen03_writer_keeps_independent_lock_after_timeout(self):
        s = source(TUNE)
        self.assertIn('const FIRMWARE_WRITE_LOCK:', s)
        self.assertIn('fd != lock_fd', s)
        self.assertIn('other != lock_fd', s)
        self.assertIn('libc::flock(file.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB)', s)
        self.assertGreaterEqual(s.count('libc::kill(pid, libc::SIGKILL)'), 2)

    def test_cen04_hold_requires_successful_apply_and_reset(self):
        s = source(UVHELP)
        self.assertEqual(s.count('if hold && out["ok"] == true'), 2)
        self.assertIn('apply_atomic(&intel_uv::reset_profile())', s)

    def test_cen05_failed_intel_profile_does_not_commit_applied(self):
        s = source(UV)
        self.assertIn('fn fully_applied(', s)
        self.assertIn('hold_live = fully_applied(&result)', s)
        self.assertEqual(s.count('if fully_applied(&result) { applied = Some(ac); }'), 2)

    def test_cen06_restore_tar_nonzero_is_error(self):
        s = source(BACKUP)
        self.assertIn('if tcode != 0 { return Err(format!("tar failed with status {tcode}; restore may be incomplete', s)
        self.assertNotIn('target.is_none() || tcode != 2', s)

    def test_cen07_reload_only_commits_policy_after_nft_transaction(self):
        s = source(NET)
        block = s[s.index('if RELOAD.swap(false, Ordering::SeqCst) {'):s.index('// Pending log lines')]
        self.assertLess(block.index('nft_load(&c, log_blocked)'), block.index('g.cfg = c;'))
        self.assertNotIn('std::mem::replace(&mut g.cfg, c)', block)

    def test_cen08_replace_validated_and_rolled_back(self):
        s = source(TPROF)
        block = s[s.index('fn apply_with('):s.index('fn guard_after(')]
        self.assertLess(block.index('preflight_replace(values)'), block.index('restore_entries(st, Some(&stale))'))
        self.assertIn('snapshot_replace(st, &plan)', block)
        self.assertIn('rollback_replace(st, before.take().unwrap(), &saved, result)', block)
        self.assertIn('if out.get("rolled_back").is_some() { return out; }', s)

    def test_cen09_firmware_batch_must_pass_entire_preflight(self):
        s = source(FW)
        block = s[s.index('Value::Array(items) => {'):s.index('Value::Object(o) => write_one')]
        self.assertLess(block.index('checked.iter().any(Result::is_err)'), block.index('write_verified(&targets)'))
        self.assertIn('let rollbacks = write_verified(&undo_refs)', block)

    def test_cen10_v3_requires_independent_reference_points(self):
        s = source(NV)
        block = s[s.index('fn v3_agrees('):s.index('// ── Writers')]
        self.assertIn('independent.len() >= 2', block)
        self.assertIn('anchors.iter().all(', block)

    def test_cen11_reload_error_is_separate_status(self):
        s = source(NET)
        self.assertIn('enum ReloadState { Applied, Failed, Pending }', s)
        self.assertIn('if a["error"].is_string() { return ReloadState::Failed; }', s)
        self.assertIn('v["reload_state"] = json!(', s)


if __name__ == '__main__':
    unittest.main()
