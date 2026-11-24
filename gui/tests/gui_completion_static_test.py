#!/usr/bin/env python3
"""Source-contract smoke tests; do not establish GUI runtime correctness."""
from pathlib import Path
import unittest
root = Path(__file__).resolve().parents[1]
def src(name): return (root / 'src' / name).read_text()
class Completion(unittest.TestCase):
    def test_curve_undo_draft_only(self):
        s=src('nvidiatab.cpp'); body=s.split('void NvidiaTab::restoreCurveDraft(bool redo) {',1)[1].split('void NvidiaTab::recompute()',1)[0]
        self.assertIn('if (busy_ || base_.isEmpty()) return;',body)
        self.assertIn('pointOffsets_ = draft.offsets',body)
        self.assertNotIn('runHelper(',body); self.assertNotIn('privileged::run(',body)
        self.assertIn('undoCurve_.size() > 32',s)
    def test_custom_palette_validation(self):
        s=src('theme.cpp'); self.assertIn('colors.size() != 19',s); self.assertIn('^#[0-9a-fA-F]{6}$',s)
        self.assertIn('customTheme/colors',s); self.assertIn('std::array<std::array<char, 8>, 19> activeStorage',s); self.assertIn('*tok[i] = activeStorage[i].data()',s)
    def test_profile_diff_read_only(self):
        s=src('profilediff.h');self.assertIn('QIODevice::ReadOnly',s);self.assertIn('isUndefined()',s);self.assertNotIn('QIODevice::WriteOnly',s)
        for f in ['nvidiatab.cpp','ryzentab.cpp','scenestab.cpp','optimizetab.cpp']:self.assertIn('profilediff::show',src(f))
    def test_calibration_bounded_read(self):
        s=src('optimizetab.cpp');self.assertIn('sig.value("version").toInt()!=10',s);self.assertIn('f.read((8LL<<20)+1)',s)
    def test_pcie_same_boot(self):
        s=src('systemdiagnostics.cpp');self.assertIn('const bool sameBoot = !boot.isEmpty()',s);self.assertIn('!vendor.isEmpty() && !device.isEmpty()',s);self.assertIn('previousLinks_',src('systools.cpp'))
    def test_sleep_dedup(self):
        s=src('advanceddiagnostics.cpp');self.assertIn('exactSleepRecords.contains(identity)',s);self.assertIn('id = spans[i].id;',s)
    def test_cpu_capabilities_nonblocking(self):
        s=src('cpustressbox.cpp');self.assertIn('infoProc->start(program_',s);self.assertNotIn('waitForFinished',s.split('void CpuStressBox::fillDurations()',1)[0]);self.assertIn('if (info_.cores.isEmpty() || load_->count()==0) return;',s)
if __name__ == '__main__': unittest.main(verbosity=2)
