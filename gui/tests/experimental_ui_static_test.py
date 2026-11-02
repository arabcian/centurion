#!/usr/bin/env python3
"""Guardrails for Experimental UI integration when Qt development files are unavailable.

Static checks do not replace a compile or a real hardware regression run.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
def source(path):
    return (ROOT / path).read_text(encoding='utf-8')

def check(title, condition):
    if not condition:
        raise AssertionError(title)
    print('PASS', title)

main = source('gui/src/mainwindow.cpp')
nav = source('gui/src/uinavigator.cpp')
theme = source('gui/src/theme.cpp')
home = source('gui/src/hometab.cpp')
optimize = source('gui/src/optimizetab.cpp')
cmake = source('gui/CMakeLists.txt')
check('all original top-level pages remain constructed', all(
    x in main for x in ['new HomeTab', 'new FwattrTab', 'new RyzenTab', 'new IntelTab',
                     'new NvidiaTab', 'new AmdGpuTab', 'new OptimizeTab',
                     'new LightingTab', 'new Zone4Tab', 'new HealthTab', 'new ScenesTab']))
check('original conditional hardware availability preserved', all(
    x in main for x in ['FwattrTab::present()', 'NvidiaTab::present()',
                     'AmdGpuTab::present()', 'lighting::present()',
                     'lighting::zone4Present()', 'sysinfo::isIntel()']))
check('all original operational connections preserved', all(
    x in main for x in ['HealthTab::levelChanged', 'HealthTab::openTab',
                     'HomeTab::dgpuRestored', 'setFanControls',
                     'SceneEngine::finished', 'TrialBar', 'DriftWatch',
                     'SmartWatch', 'LidWatch']))
check('layout switch saves via Qt settings and supports classic fallback', all(
    x in theme for x in ['CENTURION_UI', 'ui/mode', 'experimental', 'classic', 'saveUiMode']))
check('feature-preserving reorder relies on tab IDs, not recreating content', all(
    x in main for x in ['tabData(i)', 'moveTab(from, target)', 'arrangeTabs()', 'rebuildExperimentalNavigation()']))
check('Health and Home deep links follow canonical page IDs', all(
    x in main for x in ['tabs_->tabBar()->tabData(i).toString() == label',
                     'HomeTab::openPageRequested']))
check('late dGPU discovery still inserts its tab', all(
    x in main for x in ['ensureNvidiaTab()', 'tabs_->insertTab(at, nvidia_',
                     'Q_EMIT nvidiaTabAdded()']))
check('new navigator only changes navigation state', all(
    x in nav for x in ['setCurrentIndex', 'setCurrentItem', 'ensureWidgetVisible'])
    and not re.search(r'privileged::|QProcess|applyProfile|setFanTarget|run\(', nav))
check('navigator indexes sub-tabs without activating lazy pages during search', all(
    x in nav for x in ['findChildren<QTabWidget *>', 'inner->tabText(j)',
                     'findChildren<QTreeWidget *>', 'isVisibleTo(page)']))
check('Home quick-access button farm stays removed (v3: the rail navigates)',
    'openPageRequested(target.second)' not in home)
check('experimental style uses current Centurion theme tokens', all(
    x in theme for x in ['Experimental / Studio', 'QTabWidget#mainTabs',
                       'QGroupBox', '@BG0', '@ACCENT']))
check('Optimize searches real setting rows without applying them', all(
    x in optimize for x in ['jumpToSetting(const QString &query)', 'settingSearchModel_->setStringList(entries)',
                       'scroll->ensureWidgetVisible(r.name', 'r.include->setFocus'])
    and 'applySelected()' not in optimize[optimize.index('void OptimizeTab::jumpToSetting('):optimize.index('void OptimizeTab::buildRows(')])
check('CMake builds new navigator source', 'src/uinavigator.cpp' in cmake)
check('original helper source stays external to UI edits',
    'src/privileged.cpp' in cmake and 'src/uinavigator.cpp' in cmake)
check('no duplicated main-window implementation', main.count('void MainWindow::arrangeTabs()') == 1)
print('PASS: 15 Experimental UI source checks')
