#!/usr/bin/env python3
"""Metadata and source contracts only; QWidget behavior is a separate CTest."""
import re
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
R=Path(__file__).resolve().parents[1]
def src(name): return (R/'src'/name).read_text()
class NavigationTargets(unittest.TestCase):
    def test_registered_ids(self):
        ids=[]
        for name in ('hometab.cpp','ryzentab.cpp','inteltab.cpp','fwattrtab.cpp','nvidiatab.cpp','amdgputab.cpp'):
            ids += re.findall(r'navigationtargets::mark\([^,]+, QStringLiteral\("([^"%]+)"\)',src(name))
        self.assertEqual(len(ids),len(set(ids)))
        self.assertGreater(len(ids),25)
        amd=re.findall(r'\{"(amd\.[^"]+)", "([^"]+)", "([^"]+)"\}',src('navigationtargets.h'))
        self.assertEqual(len(amd),19)
        self.assertEqual(len({x[0] for x in amd}),19)
        self.assertEqual(len({x[1] for x in amd}),19)
        cat=ET.parse(R/'i18n/centurion_tr.ts')
        translations={m.findtext('source') for c in cat.findall('context') if c.findtext('name')=='NavigationTargets' for m in c.findall('message') if m.findtext('translation')}
        self.assertTrue(all(title in translations for _,title,_ in amd))
    def test_no_probe_during_index(self):
        s=src('uinavigator.cpp'); block=s.split('// Registered targets',1)[1].split('// Only *already visible*',1)[0]
        self.assertIn('navigationtargets::entries(page)',block)
        for call in ('privileged::','->show(','->setCurrentIndex(','->click(','->rebuild(','->discover('): self.assertNotIn(call,block)
        self.assertIn('navigationtargets::reveal(d.page.data(), d.stableTarget)',s)
        h=src('navigationtargets.h'); collect=h.split('inline QList<Entry> entries',1)[1].split('inline QWidget *resolve',1)[0]
        self.assertIn('ambiguous.contains(e.id)',collect)
        self.assertNotIn('setCurrentIndex',collect)
        self.assertNotIn('new QWidget',collect)
        reveal=h.split('inline bool reveal',1)[1].split('// AMD overdrive',1)[0]
        self.assertIn('resolve(page, id)',reveal)
        self.assertIn('isTabEnabled',reveal)
        self.assertIn('isVisibleTo(page)',reveal)
        self.assertNotIn('->click(',reveal)
    def test_actual_editor_identity(self):
        self.assertIn('a.device + QLatin1Char(\'/\') + a.name',src('fwattrtab.cpp'))
        self.assertIn('ryzen.ccd.%1.slot.%2',src('ryzentab.cpp'))
        self.assertIn('navigationtargets::markAmd(w, name)',src('amdgputab.cpp'))
        self.assertIn('exactKey ? 12000 : exactTitle ? 11000',src('uinavigator.cpp'))
if __name__=='__main__': unittest.main()
