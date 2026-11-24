#!/usr/bin/env python3
"""Stage 3 source contracts, not a substitute for compiled/native GUI tests."""
import unittest
from pathlib import Path
R=Path(__file__).resolve().parents[1]/'src'
def src(n): return (R/n).read_text()
class Stage3(unittest.TestCase):
    def test_filter_does_not_mutate_report(self):
        s=src('diagreportview.cpp')
        body=s.split('void DiagReportView::filterReport()',1)[1].split('void DiagReportView::previewExport()',1)[0]
        self.assertIn('item->setHidden(!match)',body)
        self.assertNotIn('report_',body)
        self.assertIn('QSaveFile f(path)',s)
        self.assertIn('f.write(bytes) != bytes.size()',s)
        self.assertIn('const diag::Report captured = report_',s)
        self.assertIn('connect(copy_, &QPushButton::clicked, this, &DiagReportView::previewExport)',s)
        self.assertIn('connect(save_, &QPushButton::clicked, this, &DiagReportView::previewExport)',s)
        self.assertIn('copy->setEnabled(false);save->setEnabled(false)',s)
    def test_no_old_report_on_message(self):
        s=src('diagreportview.cpp').split('void DiagReportView::setMessage',1)[1].split('void DiagReportView::setReport',1)[0]
        for token in ('report_ = {};','hasReport_ = false','copy_->setEnabled(false)','save_->setEnabled(false)','preview_->setEnabled(false)'):
            self.assertIn(token,s)
    def test_context_before_tail(self):
        s=src('klogpage.cpp').split('void KlogPage::render()',1)[1]
        self.assertLess(s.index('context.append(*p)'),s.index('if (p->level > levelMax)'))
        self.assertLess(s.index('context.append(*p)'),s.index('shown.size() > MAX_SHOWN_LINES'))
        self.assertIn('rows = context.rows()',s)
        c=src('klogview.cpp').split('void ContextWindow::append',1)[1].split('std::vector<Row> ContextWindow::rows',1)[0]
        self.assertIn('line.boot != boot_',c)
        self.assertIn('lines_.pop_front()',c)
        self.assertIn('after_',c)
    def test_connection_selection_identity(self):
        s=src('netpage.cpp')
        key=s.split('static QString connectionKey',1)[1].split('static void rememberColumns',1)[0]
        for token in ('"proto"','"local"','"lport"','"remote"','"rport"','"pid"','"uid"','"exe"','QJsonArray key'):
            self.assertIn(token,key)
        self.assertIn('selectionMatches == 1',s)
        self.assertIn('idx < 0 || idx >= connData_.size()',s)
        self.assertIn('== connectionKey(c) ? c : QJsonObject()',s)
        self.assertIn('connSnapshotError_',s)
    def test_incident_draft_explicit_save(self):
        s=src('advancedhealth.cpp')
        self.assertIn('drafts_.insert(loadedIncident_, draft)',s)
        self.assertIn('drafts_.value(key, originalIncident_)',s)
        self.assertEqual(s.count('advanceddiagnostics::saveIncident('),1)
        self.assertIn('loadedIncident_ != it->data(0, Qt::UserRole).toString()',s)
        self.assertIn('note_->toPlainText().size() > 2000',s)
    def test_edid_and_pci_are_read_only(self):
        s=src('diagnostictools.cpp').split('Report displays(',1)[1].split('// ── S06',1)[0]
        self.assertIn('sum % 256',s)
        self.assertIn('r.tables << blocks',s)
        self.assertIn('r.tables << modesTable',s)
        self.assertNotIn('QIODevice::WriteOnly',s)
        p=src('systemdiagnostics.cpp').split('Report pcieLinks(',1)[1].split('// ── S07',1)[0]
        self.assertIn('pciAddress.match(parent).hasMatch()',p)
        self.assertIn('r.tables << topology',p)
        self.assertNotIn('QIODevice::WriteOnly',p)
    def test_boot_selection_stays_in_filter(self):
        s=src('bootpage.cpp').split('void BootPage::apply(',1)[1].split('void BootPage::showDetail()',1)[0]
        self.assertIn('retained && !retained->isHidden()',s)
        self.assertIn('pick && !pick->isHidden()',s)
    def test_tool_output_search_bounded_and_display_only(self):
        s=src('systools.cpp')
        self.assertIn('bool search = true;',s)
        self.assertIn('if (n >= 2000) { limited = true; break; }',s)
        self.assertIn('text_->document()->find(q, c)',s)
        self.assertIn('QApplication::clipboard()->setText(text_->toPlainText())',s)
    def test_smart_reading_freshness(self):
        s=src('smartpage.cpp')
        self.assertIn('Displaying previous readings from %1',s)
        self.assertLess(s.index('render(disks, record.alerts)'),s.index('lastChecked_ = QDateTime::currentDateTime()'))
if __name__=='__main__': unittest.main()
