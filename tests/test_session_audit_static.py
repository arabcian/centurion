"""Static regression guards for session diagnostics (not a runtime/compile test).
Run: python3 tests/test_session_audit_static.py
"""
from pathlib import Path
import unittest
ROOT = Path(__file__).resolve().parents[1]

def source(relative):
    return (ROOT / relative).read_text(encoding='utf8')

class SessionAuditGuards(unittest.TestCase):
    def test_rust_aer_colon_parser_and_fixture(self):
        s=source('crates/centurion-helpers/src/health.rs')
        self.assertIn('strip_prefix(key)', s)
        self.assertIn('aer_total(&s, key)', s)
        self.assertIn('fn aer_sysfs_format_and_unknowns()', s)
    def test_rust_aer_identifies_endpoint(self):
        s=source('crates/centurion-helpers/src/health.rs')
        self.assertIn('"vendor": vendor.trim()', s)
        self.assertIn('"device": device.trim()', s)
    def test_gui_aer_identity_and_atomic_permissions(self):
        s=source('gui/src/evidencereports.cpp')
        self.assertIn('sameDevice', s)
        self.assertIn('!a.value(QStringLiteral("device")).toString().isEmpty()',s)
        self.assertIn('f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)',source('gui/src/diagsources.cpp'))
    def test_system_pcie_actual_multiline_total(self):
        s=source('gui/src/systemdiagnostics.cpp')
        self.assertIn('advanceddiagnostics::aerTotal(rd(root, base + QLatin1Char', s)
        self.assertIn('Endpoint identity changed at this PCI address', s)
    def test_incident_read_modify_write(self):
        gui=source('gui/src/advancedhealth.cpp')
        self.assertIn('incidentNote(key)',gui)
        self.assertIn('saved.value(QStringLiteral("note"))', gui)
        s=source('gui/src/advanceddiagnostics.cpp')
        self.assertIn('diagsrc::saveJson(path, all)',s)
        self.assertIn('f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)',source('gui/src/diagsources.cpp'))
    def test_nvme_helper_drained_and_timeout_coherent(self):
        rust=source('crates/centurion-helpers/src/health_nvme.rs')
        cpp=source('gui/src/advancedhealth.cpp')
        self.assertIn('std::thread::spawn(move ||',rust)
        self.assertIn('const TIMEOUT: Duration = Duration::from_secs(4)',rust)
        self.assertIn('const MAX_CONTROLLERS: usize = 8',rust)
        self.assertIn('90000 : 30000',cpp)
    def test_h16_query_can_fail_safely(self):
        s=source('gui/src/advancedhealth.cpp')
        self.assertIn('timedOut',s)
        self.assertIn('ok ? out : QString()',s)
        self.assertIn('Query ECC (wakes the GPU)',s)
    def test_audio_external_checks_async(self):
        tools=source('gui/src/systools.cpp')
        s=source('gui/src/systemdiagnostics.cpp')
        self.assertIn('{"@diag", "S15"}, {"wpctl", "status"}, {"pactl", "info"}',tools)
        begin=s.index('Report audio(')
        end=s.index('return r;',begin)
        self.assertNotIn('waitForFinished',s[begin:end])
    def test_zcat_stdout_drained(self):
        s=source('gui/src/systemdiagnostics.cpp')
        self.assertIn('out += p.readAllStandardOutput();  // drained while it runs',s)
    def test_co_guard_invalid_is_unknown(self):
        s=source('gui/src/advanceddiagnostics.cpp')
        self.assertIn('The crash-guard file is unreadable', s)
        self.assertIn('Its trial state is unknown', s)
    def test_sleep_deduplicated_by_boot_and_sequence(self):
        s=source('gui/src/advanceddiagnostics.cpp')
        self.assertIn('diagsrc::mergeUnique',source('gui/tests/advanceddiagnostics_test.cpp'))
        self.assertIn('if (exactSleepRecords.contains(identity)) continue',s)
        self.assertIn('id = spans[i].id;',s)
    def test_flight_cadence_not_fixed_15_seconds(self):
        s=source('gui/src/advanceddiagnostics.cpp')
        self.assertIn('flight_interval',s)
        self.assertIn('expected * 5 / 2',s)
        self.assertIn('The recorder interval changed during boot',s)
        rust=source('crates/centurion-helpers/src/boothist.rs')
        self.assertIn('b["flight_interval"] = flight["interval"].clone();',rust)
    def test_gui_fixtures_added(self):
        s=source('gui/tests/advanceddiagnostics_test.cpp')
        for token in ('1 gap','crash-guard','aerTotal(', 'incidentNote(', 'endpoint.remove', 'never resumed'):
            self.assertIn(token,s)

if __name__=='__main__':
    unittest.main(verbosity=2)
