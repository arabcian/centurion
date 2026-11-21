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
        self.assertIn("line.split_once(':')", s)
        self.assertIn('aer_total(&s, key)', s)
        self.assertIn('fn aer_sysfs_colon_format_and_unknowns()', s)
    def test_rust_aer_identifies_endpoint(self):
        s=source('crates/centurion-helpers/src/health.rs')
        self.assertIn('"vendor": vendor.trim()', s)
        self.assertIn('"device": device.trim()', s)
    def test_gui_aer_identity_and_atomic_permissions(self):
        s=source('gui/src/healthevidence.cpp')
        self.assertIn('verifiedIdentity', s)
        self.assertIn('identity changed/unverified; delta Unknown',s)
        self.assertIn('save.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)',s)
    def test_system_pcie_actual_multiline_total(self):
        s=source('gui/src/advanceddiagnostics.cpp')
        self.assertIn('aerTotal(v, QString::fromLatin1(total))', s)
        self.assertIn('Endpoint identity changed at this PCI address', s)
    def test_incident_read_modify_write(self):
        gui=source('gui/src/advancedhealth.cpp')
        self.assertIn('incidentNote(key)',gui)
        self.assertIn('saved.value(QStringLiteral("note"))', gui)
        s=source('gui/src/advanceddiagnostics.cpp')
        self.assertIn('f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)',s)
    def test_nvme_helper_drained_and_timeout_coherent(self):
        rust=source('crates/centurion-helpers/src/health_nvme.rs')
        cpp=source('gui/src/advancedhealth.cpp')
        self.assertIn('std::thread::spawn(move ||',rust)
        self.assertIn('const TIMEOUT: Duration = Duration::from_secs(4)',rust)
        self.assertIn('const MAX_CONTROLLERS: usize = 8',rust)
        self.assertIn('90000 : 30000',cpp)
    def test_h16_query_can_fail_safely(self):
        s=source('gui/src/advancedhealth.cpp')
        self.assertIn('centurionTimedOut',s)
        self.assertIn('any partial output is not a verified ECC reading',s)
        self.assertIn('Active NVIDIA ECC query (may wake GPU)',s)
    def test_audio_external_checks_async(self):
        tools=source('gui/src/systools.cpp')
        s=source('gui/src/advanceddiagnostics.cpp')
        self.assertIn('{"@advanced","S15"},{"wpctl","status"},{"pactl","info"}',tools)
        begin=s.index('QString audioDoctor()')
        end=s.index('QString kernelDependencies()',begin)
        self.assertNotIn('waitForFinished',s[begin:end].split('// Do not call waitForFinished()')[0])
    def test_zcat_stdout_drained(self):
        s=source('gui/src/advanceddiagnostics.cpp')
        self.assertIn('out += p.readAll();  // drain while child is running',s)
    def test_co_guard_invalid_is_unknown(self):
        s=source('gui/src/advanceddiagnostics.cpp')
        self.assertIn('guard count UNKNOWN', s)
        self.assertIn('trial state UNKNOWN', s)
    def test_sleep_deduplicated_by_boot_and_sequence(self):
        s=source('gui/src/advanceddiagnostics.cpp')
        self.assertIn('const QString key=m.hasMatch()',s)
        self.assertIn('if (seen.contains(key)) continue',s)
    def test_flight_cadence_not_fixed_15_seconds(self):
        s=source('gui/src/advanceddiagnostics.cpp')
        self.assertIn('flight_interval',s)
        self.assertIn('expected*5/2',s)
        self.assertIn('Cadence differs between ring header',s)
        rust=source('crates/centurion-helpers/src/boothist.rs')
        self.assertIn('b["flight_interval"] = flight["interval"].clone();',rust)
    def test_gui_fixtures_added(self):
        s=source('gui/tests/advanceddiagnostics_test.cpp')
        for token in ('gaps >75s=1','guard count UNKNOWN','trial state UNKNOWN','aerTotal(', 'incidentNote(', 'identity changed','a0000000 | 2 | 2 | 1 | 1 | unclean'):
            self.assertIn(token,s)

if __name__=='__main__':
    unittest.main(verbosity=2)
