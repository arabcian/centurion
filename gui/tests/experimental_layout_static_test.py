#!/usr/bin/env python3
"""Source-only regression guards, NOT a substitute for Qt compilation or device QA."""
from pathlib import Path
import re
ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / 'gui/src'
read = lambda name: (BASE / name).read_text()
files = {n: read(n) for n in ('hometab.cpp', 'ryzentab.cpp', 'nvidiatab.cpp',
        'optimizetab.cpp', 'overviewpage.cpp', 'healthtab.cpp', 'scenestab.cpp',
        'lightingtab.cpp', 'fwattrtab.cpp', 'experimentalui.h', 'theme.cpp')}
checks = {
 'Home actually builds real 2-column workspace': 'homeWorkspace' in files['hometab.cpp'] and 'temperaturePlot_->append(cpu, gpu)' in files['hometab.cpp'],
 'Home never introduces NVML polling': 'nvidia-smi' not in files['experimentalui.h'] and 'NVML' not in files['experimentalui.h'],
 'Home Classic 2x2 layout retained': 'cells->addWidget(boxes[i], i / 2, i % 2)' in files['hometab.cpp'],
 'Ryzen existing SMU slot layout retained': 'slots_.append({ccd, s, e, d, id, cppc, mhz, volt, watt})' in files['ryzentab.cpp'],
 'Ryzen focused editor is linked to actual slot': 'slots_[n].entry->setText' in files['ryzentab.cpp'] and 'slots_[n].disable->setChecked' in files['ryzentab.cpp'],
 'Ryzen labels use CO steps, not millivolts': 'CO steps' in files['ryzentab.cpp'],
 'NVIDIA uses original VfCurveWidget': 'work->addWidget(curve)' in files['nvidiatab.cpp'] and 'vf_ = new VfCurveWidget' in files['nvidiatab.cpp'],
 'NVIDIA original write/reset operations retained': all(t in files['nvidiatab.cpp'] for t in ['applyOffsets', 'reset_all', 'resetCurve', 'toggleDefault']),
 'Optimize autotune parameters preserved': all(t in files['optimizetab.cpp'] for t in ['autoGoal_', 'autoCtx_', 'weightsBtn', 'calBtn', 'applySelected']),
 'Health reports from real overview helper': 'userhelper::run(req' in files['overviewpage.cpp'] and 'rebuildCards(cards)' in files['overviewpage.cpp'],
 'Health actions have actual destinations': all(t in files['healthtab.cpp'] for t in ['"smart"', '"tools"', '"klog"', 'showPage(page)']),
 'Scenes dynamic editor preserved': 'editorHost->setLayout(right)' in files['scenestab.cpp'] and 'saveCurrent()' in files['scenestab.cpp'],
 'Lighting physical keyboard retained': 'new KeyboardView' in files['lightingtab.cpp'] and 'work->addWidget(kbBox)' in files['lightingtab.cpp'],
 'Firmware discovery and safety checks retained': all(t in files['fwattrtab.cpp'] for t in ['FwattrTab::discover()', 'refreshLockState()', 'applyAll()', 'readOnlyAttr']),
 'Experimental sections all use shared workspace style': all(t in files['theme.cpp'] for t in ['homeWorkspace','ryzenWorkspace','nvidiaWorkspace','optimizeWorkspace','healthWorkspace','scenesWorkspace','lightingWorkspace']),
 'Original callbacks unchanged for operations': all(t in files['hometab.cpp'] for t in ['&HomeTab::refreshLive', 'applyProfile(', 'setFanTarget(']),
 'Health overview QGroupBox header is explicit': '#include <QGroupBox>' in files['overviewpage.cpp'],
 'Optimize mixed widget list has nine explicit QWidget casts': files['optimizetab.cpp'].split('for (QWidget *w : {static_cast<QWidget *>(presetCombo_)', 1)[1].split('})', 1)[0].count('static_cast<QWidget *>(') == 8,
}
for key, ok in checks.items():
    if not ok: raise AssertionError(key)
    print('PASS', key)
print('PASS:', len(checks), 'layout source checks')
