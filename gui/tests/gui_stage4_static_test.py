#!/usr/bin/env python3
"""Stage 4 source contracts. Native Qt tests and hardware QA are separate."""
from pathlib import Path
import unittest
R=Path(__file__).resolve().parents[1]/'src'
def s(name): return (R/name).read_text()
class Stage4(unittest.TestCase):
    def test_runtime_mode_frozen(self):
        t=s('theme.cpp')
        self.assertIn('bool experimentalEnabled() { return g_experimental; }', t)
        self.assertIn('g_experimental = readExperimentalChoice();', t)
        save=t.split('bool saveUiMode(',1)[1].split('void load()',1)[0]
        self.assertNotIn('g_experimental =',save)
    def test_catalogue_search_no_probes(self):
        t=s('uinavigator.cpp'); h=s('healthtab.cpp')
        self.assertIn('health->navigationEntries()',t)
        self.assertIn('toolcatalog::navigationScore(filter, d.title, {d.search})',t)
        self.assertIn('tuningnavigation::entries',t)
        self.assertIn('tabs->indexOf(d.page)',t)
        self.assertNotIn('QTreeWidgetItem *treeItem',t)
        self.assertNotIn('privileged::',t)
        body=h.split('QList<HealthTab::NavigationEntry> HealthTab::navigationEntries()',1)[1].split('QWidget *HealthTab::pageByKey',1)[0]
        self.assertNotIn('openTool(',body)
        self.assertNotIn('new QWidget',body)
        self.assertIn('toolcatalog::tool(t.key)',body)
    def test_history_and_dashboard(self):
        m=s('mainwindow.cpp');h=s('hometab.cpp');t=s('experimentalui.h')
        self.assertIn('replayingPageHistory_',m)
        self.assertIn('pageHistory_.size() > 50',m)
        self.assertIn('ui/restore_last_page',m)  # legacy preference migration
        self.assertIn('ui/experimental/',m)
        self.assertIn('ui/classic/',m)
        self.assertIn('navigationPreferencesRequested',h)
        self.assertIn('tabs_->isTabEnabled',m)
        self.assertIn('cells->removeWidget(entry.second)',h)
        self.assertIn('threshold + (lastColumns==1?24:0)',h)
        self.assertIn('home/experimentalPanelOrder',h)
        self.assertIn('visible==lastVisible',h)
        self.assertIn('sample.ms - previousMs > 6000',t)
        self.assertIn('samples_.size() > 2048',t)
        self.assertIn('now - samples_.last().ms <= 6000',t)
    def test_restart_guard(self):
        m=s('mainwindow.cpp');t=s('theme.cpp')
        self.assertIn('g_restartGuard && !g_restartGuard()',t)
        self.assertIn('window->activeOperations().isEmpty()',m)
        self.assertIn('trial->running()',m)
        self.assertIn('box->operationActive()',m)
        self.assertIn('aboutToQuit',m)
    def test_scene_selection_and_capture(self):
        t=s('scenestab.cpp')
        self.assertIn('const QSignalBlocker blocked(list_)',t)
        self.assertIn('revision != editorRevision_',t)
        self.assertIn('if (!scenes::remove(n))',t)
        self.assertIn('loaded_.name == saved.name',t)
    def test_draft_lighting_history(self):
        t=s('lightingtab.cpp')
        body=t.split('void LightingTab::stepEditHistory',1)[1].split('void LightingTab::refreshAll',1)[0]
        self.assertIn('work_ = editHistory_.at(next)',body)
        self.assertNotIn('run(',body)
        self.assertIn('editHistory_.size() > 32',t)
        self.assertIn('loaded_ = sent;\n        resetEditHistory()',t)
    def test_backup_selection_and_atomic_verification(self):
        t=s('backuppage.cpp')
        self.assertIn('QItemSelectionModel::NoUpdate',t)
        self.assertIn('keepPaths.contains',t)
        self.assertIn('if (!it || !it->isSelected()) return {};',t)
        self.assertIn('QSaveFile f(verifyStatePath())',t)
    def test_restore_states(self):
        t=s('coolingpage.cpp');c=s('cpustressbox.cpp')
        self.assertIn('fanRestoring_ = true;',t)
        self.assertIn('if (proc_ || fanTesting_ || fanRestoring_) return;',t)
        self.assertIn('releasingPreset_ = true;',c)
        self.assertIn('if (releasingPreset_)',c)
if __name__=='__main__':unittest.main()
