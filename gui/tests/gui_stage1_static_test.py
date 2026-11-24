#!/usr/bin/env python3
"""Source contracts only; native tests and GUI integration remain required."""
import unittest
from pathlib import Path
R = Path(__file__).resolve().parents[1] / 'src'
S = (R / 'optimizetab.cpp').read_text()
class Stage1(unittest.TestCase):
    def test_explicit_state_columns(self):
        self.assertIn('"Live value", "Boot default", "Saved for boot", "New value", "State"', S)
        self.assertIn('r.available && !r.cached && !readbackStale_', S)
        self.assertIn('readbackStale_ || r.cached || !r.available', S)
    def test_display_filter_never_changes_selection(self):
        body = S.split('void OptimizeTab::updateRowPresentation',1)[1].split('void OptimizeTab::setReadbackStale',1)[0]
        self.assertIn('r.filter->currentData()', body)
        self.assertNotIn('setChecked(', body)
        self.assertNotIn('runOp(', body)
        self.assertNotIn('setEditorValue(', body)
    def test_previews_precede_writes(self):
        boot = S.split('void OptimizeTab::setBoot(',1)[1].split('void OptimizeTab::clearBoot(',1)[0]
        self.assertLess(boot.index('preview.exec()'), boot.index('runOp('))
        self.assertIn('changePreview(values, true)', boot)
        apply = S.split('void OptimizeTab::applySelected(',1)[1]
        self.assertLess(apply.index('preview.exec()'), apply.index('applyValues('))
    def test_firmware_query_async(self):
        section = (R/'systools.cpp').read_text().split('key == QLatin1String("Firmware updates")',1)[1]
        self.assertNotIn('list.waitForFinished', section)
        self.assertIn('QProgressDialog::canceled', section)
        self.assertIn('Remaining updates were not started.', section)
    def test_window_and_splitter_preferences(self):
        self.assertIn('restoreGeometry(windowSettings.value', (R/'mainwindow.cpp').read_text())
        self.assertIn('split->restoreState', (R/'cosearchdialog.cpp').read_text())
if __name__ == '__main__': unittest.main()
