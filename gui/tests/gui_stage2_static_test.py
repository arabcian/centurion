#!/usr/bin/env python3
"""Stage 2 source contracts; these do not exercise Qt rendering or hardware."""
import unittest
from pathlib import Path
R=Path(__file__).resolve().parents[1]/'src'
def src(name): return (R/name).read_text()
class Stage2(unittest.TestCase):
    def test_co_history_is_snapshot(self):
        body=src('cosearchdialog.cpp').split('void CoSearchDialog::showAttemptHistory()',1)[1].split('void CoSearchDialog::refreshTable()',1)[0]
        self.assertIn('attempts_.at(i)',body)
        self.assertIn('setEditTriggers(QAbstractItemView::NoEditTriggers)',body)
        for forbidden in ('privileged::run','search_->next(', 'writeJournal(', 'saveCrashGuard(', 'set_coper'):
            self.assertNotIn(forbidden,body)
    def test_recovery_keeps_guard_order(self):
        body=src('cosearchdialog.cpp').split('void CoSearchDialog::offerResume()',1)[1].split('void CoSearchDialog::launch()',1)[0]
        self.assertLess(body.index('saveCrashGuard(*s, *interrupted)'),body.index('QMessageBox::question(this, tr("Resume CO search")'))
        self.assertIn('Interrupted candidate (not the next candidate)',body)
        self.assertIn('recovered["core"] = i',body)
    def test_reference_curves_are_display_only(self):
        s=src('vfcurvewidget.cpp')
        body=s.split('void VfCurveWidget::setReferenceCurves',1)[1].split('void VfCurveWidget::fitAxes',1)[0]
        self.assertNotIn('pts_ =',body)
        self.assertNotIn('pointEdited(',body)
        self.assertIn('Qt::DashLine',s)
        self.assertIn('reference(readRef_',s)
        n=src('nvidiatab.cpp')
        self.assertIn('readSnapshotTime_',n)
        self.assertIn('historical, not live clocks',n)
    def test_explore_is_not_saved_as_editor_state(self):
        s=src('optimizetab.cpp')
        self.assertIn('showAutotuneReport(d, {}, true)',s)
        body=s.split('void OptimizeTab::showAutotuneReport',1)[1]
        self.assertIn('exploreOnly ? proposed',body)
        self.assertIn('exploreOnly ? nullptr : bb->addButton',body)
        # Selection-only path is explicitly scoped to non-Explore reports.
        stage = body.split('if (!exploreOnly) {\n        const auto initialSelection = collectValues();',1)[1].split('v = scrollPage(tr("Predictions & trade-offs"))',1)[0]
        self.assertIn('r.include->setChecked(keys.contains(r.key))',stage)
        for forbidden in ('applyValues(', 'privileged::run(', 'runOp(', 'writeUserPreset('):
            self.assertNotIn(forbidden,stage)
        save = body.split('if (exploreOnly) return;',1)[1]
        self.assertIn('collectValues()',save)
        self.assertIn('!r.cached && !readbackStale_',body)
    def test_inspectors_do_not_write(self):
        for filename,marker,end in [('fwattrtab.cpp','auto *inspect = new QPushButton(tr("Parameter inspector…"))','top->addWidget(inspect)'),('amdgputab.cpp','auto *capabilities = new QPushButton','top->addWidget(capabilities)')]:
            body=src(filename).split(marker,1)[1].split(end,1)[0]
            self.assertNotIn('privileged::run',body)
            self.assertIn('NoEditTriggers',body)
    def test_fan_reference_not_used_for_edits(self):
        s=src('fancurvedialog.cpp')
        body=s.split('void FanCurveWidget::setPoint',1)[1].split('void FanCurveWidget::mousePressEvent',1)[0]
        self.assertNotIn('hardware_',body)
        self.assertIn('setHardwareLevels(hw_)',s)
        self.assertIn('Dashed: last EC table',s)
if __name__=='__main__': unittest.main()
