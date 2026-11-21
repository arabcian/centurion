#!/usr/bin/env python3
"""Safety contract of the Health diagnostics (Evidence Lab, Investigations, interpreted System tools).

Their behaviour is tested by the compiled fixture tests (gui/tests/advanceddiagnostics_test.cpp,
diagnostictools_test.cpp); this checks what a fixture cannot: that the sources never gain a write path
to hardware or kernel state, and that the root helper's NVMe query stays read-only.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GUI = ROOT / 'gui/src'
DIAG = ['diagreport.cpp', 'diagreportview.cpp', 'diagsources.cpp', 'advanceddiagnostics.cpp', 'advancedhealth.cpp',
        'evidencereports.cpp', 'healthevidence.cpp', 'systemdiagnostics.cpp', 'diagnostictools.cpp']


def src(name):
    return (GUI / name).read_text()


class HealthDiagnosticsContract(unittest.TestCase):
    def test_no_tool_that_changes_state_is_started(self):
        banned = ['setpci', 'ryzenadj', 'nvidia-settings', 'efibootmgr', 'modprobe -r', 'rmmod', 'fsck', 'rfkill unblock',
                  'systemctl', 'rc-service', 'echo >', 'tee ']
        for name in DIAG:
            code = re.sub(r'tr\("(?:\\.|[^"\\])*"\)', '', src(name))   # advice text may name tools; calls may not
            for b in banned:
                self.assertNotIn(f'"{b}', code, f'{name} starts {b}')

    def test_files_are_written_only_through_the_private_store_or_a_user_save(self):
        for name in DIAG:
            if name in ('diagsources.cpp', 'diagreportview.cpp'):
                continue
            self.assertNotIn('WriteOnly', src(name), f'{name} opens a file for writing')
        self.assertIn('ReadOwner | QFileDevice::WriteOwner', src('diagsources.cpp'))
        self.assertIn('QSaveFile', src('diagsources.cpp'))

    def test_co_search_records_are_only_read(self):
        a = src('advanceddiagnostics.cpp')
        self.assertIn('co-search-crash-guards.json', a)
        self.assertNotRegex(a, r'saveJson\([^)]*co-search')

    def test_nvme_helper_runs_only_log_reads(self):
        r = (ROOT / 'crates/centurion-helpers/src/health_nvme.rs').read_text()
        self.assertEqual(set(re.findall(r'"(smart-log|error-log|[a-z-]+)" => vec!\[', r)), {'smart-log', 'error-log'})
        for verb in ['format', 'sanitize', 'fw-download', 'fw-commit', 'set-feature', 'device-self-test', 'reset']:
            self.assertNotIn(f'"{verb}"', r)

    def test_every_report_code_is_reachable(self):
        inv = src('advancedhealth.cpp')
        for code in ['H01', 'H02', 'H03', 'H04', 'H05', 'H06', 'H09', 'H10', 'H12', 'H13', 'H15', 'H16', 'H17', 'H18']:
            self.assertIn(f'"{code}"', inv)
        tools = src('systools.cpp')
        for code in ['S01', 'S03', 'S04', 'S06', 'S07', 'S08', 'S10', 'S11', 'S13', 'S15', 'S18']:
            self.assertIn(f'{{"@diag", "{code}"}}', tools)


if __name__ == '__main__':
    unittest.main()
