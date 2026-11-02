#!/usr/bin/env python3
"""Cross-page source checks only: Qt compilation and device interaction remain mandatory."""
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2] / 'gui' / 'src'
read = lambda f: (ROOT / f).read_text()
main, health, intel, scenes = [read(f) for f in ('mainwindow.cpp', 'healthtab.cpp', 'inteltab.cpp', 'scenestab.cpp')]
fw, ryzen, nvidia, home, optimize, amd, lighting, ui = [read(f) for f in ('fwattrtab.cpp', 'ryzentab.cpp', 'nvidiatab.cpp', 'hometab.cpp', 'optimizetab.cpp', 'amdgputab.cpp', 'lightingtab.cpp', 'experimentalui.h')]
checks = {
    'Main menu uses canonical hardware-gated tabs, not duplicated panels': 'tabs_->indexOf(page)' in main and 'rebuildExperimentalNavigation()' in main,
    'Compact rail is actually implemented': 'collapsed ? 48 : 154' in main and 'button->setFixedHeight(32)' in main,
    'Health has one picker rather than two active rails': 'else { rail->setParent(this); rail->hide(); }' in health and 'healthPagePicker' in health,
    'Health picker forwards to original selection model': 'navigation_->setCurrentItem(it.value())' in health,
    'System tools keep original toolPages and their picker': 'healthToolPicker' in health and 'toolPages_->addWidget(page)' in health,
    'Intel has three task workspaces': all(x in intel for x in ['Voltage & safety','Limits & throttling','Startup & daemon','root->addWidget(work, 1)']),
    'Intel Classic retains original scrolling form': 'mid->addLayout(left, 1)' in intel and 'mid->addLayout(right, 1)' in intel,
    'Firmware real attribute editors can be split, no duplicate': 'studioGrid->addWidget(box' in fw and 'rows_.append({a, slider, spin, apply, def})' in fw,
    'Firmware still locks non-Custom profile': 'refreshLockState()' in fw and 'setBusy(busy_)' in fw,
    'Scenes automation is on-demand, still real widgets': 'experimentalui::disclosure' in scenes and 'right->addWidget(autoBox)' in scenes,
    'Home original controls and EC plot': 'homeWorkspace' in home and 'temperaturePlot_->append(cpu, gpu)' in home,
    'Ryzen original slot widgets and diagnostics': 'slots_.append({ccd, s, e, d, id, cppc, mhz, volt, watt})' in ryzen and 'ACTIVITY LOG' in ryzen,
    'NVIDIA real curve editor and on-demand logs': 'vf_ = new VfCurveWidget' in nvidia and 'ACTIVITY LOG' in nvidia,
    'Optimize real autotune parameters and settings search': 'autoGoal_' in optimize and 'jumpToSetting(settingSearch_->text())' in optimize,
    'AMD GPU dynamic firmware controls preserved': 'cards_ = cards()' in amd and 'rebuildForm()' in amd,
    'Lighting physical keyboard and effect editor': 'new KeyboardView' in lighting and 'work->addWidget(kbBox)' in lighting,
    'Disclosure does not apply any setting': 'QToolButton *disclosure' in ui and 'panel->setVisible(on)' in ui and 'QProcess' not in ui,
}
for label, result in checks.items():
    if not result: raise AssertionError(label)
    print('PASS', label)
print('PASS:',len(checks),'Studio composition checks (not a binary test)')
