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
 'Ryzen single authoritative CCD slot editor': 'coreGrid_ = gridHost' in files['ryzentab.cpp'] and 'root->addWidget(coreBox, 1)' in files['ryzentab.cpp'],
 'Ryzen labels use offset units, not misleading millivolts': 'Curve Optimizer offset for this slot' in files['ryzentab.cpp'] and ' mV' not in files['ryzentab.cpp'],
 'NVIDIA uses original full-width VfCurveWidget': 'root->addWidget(curve, 1)' in files['nvidiatab.cpp'] and 'vf_ = new VfCurveWidget' in files['nvidiatab.cpp'],
 'NVIDIA original write/reset operations retained': all(t in files['nvidiatab.cpp'] for t in ['applyOffsets', 'reset_all', 'resetCurve', 'toggleDefault']),
 'Optimize autotune parameters preserved': all(t in files['optimizetab.cpp'] for t in ['autoGoal_', 'autoCtx_', 'weightsBtn', 'calBtn', 'applySelected']),
 'Health reports from real overview helper': 'userhelper::run(req' in files['overviewpage.cpp'] and 'rebuildCards(cards)' in files['overviewpage.cpp'],
 'Health actions have actual destinations': all(t in files['healthtab.cpp'] for t in ['"smart"', '"tools"', '"klog"', 'showPage(page)']),
 'Scenes dynamic editor preserved': 'editorHost->setLayout(right)' in files['scenestab.cpp'] and 'saveCurrent()' in files['scenestab.cpp'],
 'Lighting physical keyboard retained': 'new KeyboardView' in files['lightingtab.cpp'] and 'work->addWidget(kbBox)' in files['lightingtab.cpp'],
 'Firmware discovery and safety checks retained': all(t in files['fwattrtab.cpp'] for t in ['FwattrTab::discover()', 'refreshLockState()', 'applyAll()', 'readOnlyAttr']),
 'Studio design tokens and task layouts present': all(t in files['theme.cpp'] for t in ['QWidget#expShell', 'QToolButton#expDisclosure', 'QTabWidget#intelWorkflows']) and all(t in (files['hometab.cpp'] + files['scenestab.cpp'] + files['lightingtab.cpp']) for t in ['homeWorkspace','scenesWorkspace','lightingWorkspace']),
 'Original callbacks unchanged for operations': all(t in files['hometab.cpp'] for t in ['&HomeTab::refreshLive', 'applyProfile(', 'setFanTarget(']),
 'Health overview QGroupBox header is explicit': '#include <QGroupBox>' in files['overviewpage.cpp'],
 'Optimize preset control reparenting is typed': 'static_cast<QWidget *>(presetCombo_)' in files['optimizetab.cpp'],
}
for key, ok in checks.items():
    if not ok: raise AssertionError(key)
    print('PASS', key)
print('PASS:', len(checks), 'layout source checks')
