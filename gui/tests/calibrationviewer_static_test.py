#!/usr/bin/env python3
from pathlib import Path
import unittest
root=Path(__file__).resolve().parents[1]
class ViewerContract(unittest.TestCase):
    def test_snapshot_only(self):
        s=(root/'src/optimizetab.cpp').read_text(); s=s[s.index('const auto viewCalibration'):s.index('auto *presetDiff')]
        self.assertIn('QIODevice::ReadOnly',s)
        self.assertIn('f.read((8LL<<20)+1)',s)
        self.assertIn('size()>20000',s)
        self.assertIn('sig.value("version").toInt()!=10',s)
        self.assertIn('QSaveFile out(target)',s)
        self.assertIn('setDirectWriteFallback(false)',s)
        self.assertIn('centurion-signature-%1.json',s)
        self.assertIn('out.write(bytes)',s)
        self.assertIn('hash(bytes,QCryptographicHash::Sha256)',s)
        self.assertNotIn('privileged::',s)
        self.assertNotIn('QProcess',s)
    def test_prediction_and_weight_guards(self):
        s=(root/'src/optimizetab.cpp').read_text()
        self.assertIn('calibrationmath::prediction(mu.toDouble(),sd.toDouble(),obs.toDouble())',s)
        self.assertIn('gap || group!=previousGroup',s)
        self.assertIn('effectiveKnown.at(i)',s)
        self.assertIn('values.constBegin()',s)
        self.assertIn('values.constEnd()',s)
if __name__=='__main__': unittest.main(verbosity=2)
