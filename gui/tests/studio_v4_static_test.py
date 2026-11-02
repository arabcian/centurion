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
    'Health keeps one searchable section list in both modes': 'split->addWidget(rail);' in health and 'healthPagePicker' not in health,
    'Health deep links select through the navigation tree': 'navigation_->setCurrentItem(item)' in health,
    'System tools keep original toolPages and their tree': 'toolSplit->addWidget(toolNavigation_)' in health and 'toolPages_->addWidget(page)' in health,
    'Intel has three task workspaces': all(x in intel for x in ['Voltage && safety','Limits && throttling','Startup && daemon','root->addWidget(work, 1)']),
    'Intel Classic retains original scrolling form': 'mid->addLayout(left, 1)' in intel and 'mid->addLayout(right, 1)' in intel,
    'Firmware groups stack at full width, no duplicate': 'pl->addWidget(box);' in fw and 'studioGrid' not in fw and 'rows_.append({a, slider, spin, apply, def})' in fw,
    'Firmware still locks non-Custom profile': 'refreshLockState()' in fw and 'setBusy(busy_)' in fw,
    'Scenes automation is on-demand, still real widgets': 'experimentalui::disclosure' in scenes and 'right->addWidget(autoBox)' in scenes,
    'Home original controls and EC plot': 'homeWorkspace' in home and 'temperaturePlot_->append(cpu, gpu)' in home,
    'Ryzen original slot widgets and diagnostics': 'slots_.append({ccd, s, e, d, id, cppc, mhz, volt, watt})' in ryzen and 'ACTIVITY LOG' in ryzen,
    'NVIDIA real curve editor and on-demand logs': 'vf_ = new VfCurveWidget' in nvidia and 'ACTIVITY LOG' in nvidia,
    'Optimize real autotune parameters and settings search': 'autoGoal_' in optimize and 'jumpToSetting(settingSearch_->text())' in optimize,
    'AMD GPU dynamic firmware controls preserved': 'cards_ = cards()' in amd and 'rebuildForm()' in amd,
    'Lighting physical keyboard and effect editor': 'new KeyboardView' in lighting and 'work->addWidget(kbBox)' in lighting,
    'Disclosure spans the row': 'setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed)' in ui,
    'Studio tab captions escape the ampersand': all(t in intel for t in ['tr("Voltage && safety")', 'tr("Limits && throttling")', 'tr("Startup && daemon")']) and 'tr("Hardware && firmware")' in home,
    'Firmware sits in the Tuning group': 'id == QLatin1String("Firmware")) return QStringLiteral("TUNING")' in main,
    'Home has no duplicate metric strip': 'expCpuValue_' not in home and 'experimentalui::strip' not in home,
    'Stability tests are separate framed runs': all('new QGroupBox(tr("%s"))' % t in (ROOT / f).read_text() for f, t in [('cpustressbox.cpp', 'CPU test'), ('memtestbox.cpp', 'Memory test'), ('gpustressbox.cpp', 'GPU test')]),
    'Home cards share one grid (aligned rows, equal columns)': 'cells->setColumnStretch(1, 1)' in home and 'cells->addWidget(detailCard, 1, 1)' in home and 'QSplitter' not in home,
    'Disclosure does not apply any setting': 'QToolButton *disclosure' in ui and 'panel->setVisible(on)' in ui and 'QProcess' not in ui,
}
for label, result in checks.items():
    if not result: raise AssertionError(label)
    print('PASS', label)
print('PASS:',len(checks),'Studio composition checks (not a binary test)')
