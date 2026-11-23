#!/usr/bin/env python3
"""Static checks for boot-preset visibility; not a Qt or OpenRC integration test."""
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
CPP = (ROOT / 'gui/src/optimizetab.cpp').read_text()
HDR = (ROOT / 'gui/src/optimizetab.h').read_text()
TUNE = (ROOT / 'crates/centurion-helpers/src/bin/tune-helper.rs').read_text()
OPENRC = (ROOT / 'packaging/openrc/centurion-tune').read_text()

class BootPresetHomeUITest(unittest.TestCase):
    def test_saved_boot_is_distinct_from_selection(self):
        self.assertIn('void OptimizeTab::updateHomeBootStatuses()', CPP)
        self.assertIn('boot_.value(QStringLiteral("values")).toObject()', CPP)
        self.assertIn('Checkboxes select pending changes; they do not indicate boot status.', CPP)
        self.assertIn('Saved for boot:', CPP)
        self.assertIn('void OptimizeTab::loadHomeBootValues(const QString &home)', CPP)
    def test_loading_does_not_write_system_settings(self):
        section = CPP.split('void OptimizeTab::loadHomeBootValues(', 1)[1].split('\nQWidget *OptimizeTab::homePage(', 1)[0]
        self.assertIn('const QSignalBlocker', section)
        self.assertIn('validFor(r, value)', section)
        self.assertIn('r.home != home', section)
        self.assertNotIn('runOp(', section)
        self.assertNotIn('applyValues(', section)
        self.assertNotIn('setBoot(', section)
    def test_only_visible_manual_home_pages_trigger_poll(self):
        self.assertIn('holder->installEventFilter(this)', CPP)
        self.assertIn('poll_->start()', CPP.split('bool OptimizeTab::eventFilter(',1)[1].split('// `fromPoll`',1)[0])
        self.assertIn('bool eventFilter(QObject *watched, QEvent *event) override', HDR)
    def test_persisted_boot_separate_from_service(self):
        self.assertIn('const BOOT_FILE: &str = "/etc/centurion/tune-boot.json"', TUNE)
        self.assertIn('fn op_set_boot(req: &Value)', TUNE)
        self.assertIn('fn op_boot() -> Value', TUNE)
        self.assertIn('centurion-boot-guard" check', OPENRC)
    def test_status_does_not_masquerade_as_boot_success(self):
        self.assertIn('Live matches:', CPP)
        self.assertIn('Different:', CPP)
        self.assertIn('Unavailable:', CPP)
        self.assertIn('bootDiffers', CPP)

if __name__ == '__main__':
    unittest.main()
