#!/usr/bin/env python3
"""Source contracts only; not a substitute for Qt or helper behavior tests."""
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[2]
def src(name): return (ROOT/'gui/src'/name).read_text()
class Contract(unittest.TestCase):
    def test_scene_correlation(self):
        s=src('scenes.cpp')
        self.assertIn('transactionId_ = QUuid::createUuid()',s)
        self.assertIn('gpuDeferTransactionId_ = transactionId_',s)
        self.assertIn('std::exchange(gpuDeferTransactionId_',s)
        self.assertIn('if (transactionId_ != id) return;',s)
        self.assertIn('{"transaction_id", transactionId_}',s)
        self.assertIn('sceneoutcome::terminal',s)
        self.assertIn('deferredInFlight_ > 0',s)
        self.assertIn('if (scenes_->operationActive())',src('mainwindow.cpp'))
        self.assertIn('detail.left(2048)',src('scenestab.cpp'))
    def test_quit_rechecks_after_dialog(self):
        s=src('mainwindow.cpp').split('bool MainWindow::canQuit()',1)[1].split('void MainWindow::sampleOperations()',1)[0]
        self.assertIn('page->hasUnappliedChanges()',s)
        self.assertIn('lighting_->hasUnappliedChanges()',s)
        self.assertIn('return discard && activeOperations().isEmpty()',s)
    def test_cached_scan_and_revision(self):
        s=src('netpage.cpp')
        view=s.split('auto *channelView =',1)[1].split('vwn->addLayout(rwn);',1)[0]
        self.assertIn('const QJsonObject snapshot = wifiScanSnapshot_',view)
        self.assertIn('wifiage::age',view)
        self.assertNotIn('runUser(',view)
        self.assertNotIn('refreshWifiNets()',view)
        rescan=s.split('void NetPage::rescanWifi()',1)[1].split('void NetPage::blacklistNetwork',1)[0]
        self.assertIn('rev != wifiScanRevision_ || iface != wifiIface_->currentText()',rescan)
        self.assertIn('rev == wifiScanRevision_ && iface == wifiIface_->currentText()',rescan)
        self.assertIn('CLOCK_BOOTTIME',s)
    def test_channel_interval_and_adapter_contract(self):
        gui=src('netpage.cpp')
        helper=(ROOT/'crates/centurion-helpers/src/wifi.rs').read_text()
        view=gui.split('auto *channelView =',1)[1].split('vwn->addLayout(rwn);',1)[0]
        self.assertIn('wifiRange(o)',view)
        self.assertIn('wifiage::overlapMHz',view)
        self.assertIn('seen_boottime_ms',view)
        self.assertIn('dump_completed_boottime_ms',view)
        self.assertNotIn('runUser(',view)
        self.assertIn('iface_index(&iface) != before',helper)
        self.assertIn('iface.as_deref() != Some(requested)',helper)
        self.assertIn('let iface = match scan_iface(req)',helper)
        self.assertIn('selectedMatches == 1',gui)
if __name__=='__main__': unittest.main(verbosity=2)
