"""Non-compilation integration checks for the eight read-only diagnostic features.
Real Rust/Qt fixture tests live in health.rs, health_diagnostics.rs and
 gui/tests/diagnostictools_test.cpp and require cargo + Qt6 on the build host.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
def src(path): return (ROOT/path).read_text(encoding="utf-8")

class EvidenceLabIntegration(unittest.TestCase):
    def test_rust_module_registered(self):
        self.assertIn('pub mod health_diagnostics;', src('crates/centurion-helpers/src/lib.rs'))
        self.assertIn('Some("diagnostics")', src('crates/centurion-helpers/src/bin/health-reader.rs'))
        self.assertIn('health_diagnostics::report()', src('crates/centurion-helpers/src/bin/health-reader.rs'))

    def test_all_five_health_capabilities(self):
        s=src('crates/centurion-helpers/src/health_diagnostics.rs')
        for key in ['"mce"','"topology"','"aer"','"xid_cases"','"f2fs_events"','"radio_events"','"radio_summary"']:
            self.assertIn(key,s)
        self.assertIn('300_000_000',s)
        self.assertIn('CCD not verified',s)

    def test_all_health_tabs_wired_to_real_data(self):
        s=src('gui/src/healthevidence.cpp')
        reports=src('gui/src/evidencereports.cpp')
        dispatch=src('gui/src/advancedhealth.cpp')
        for code, fn in [('H02','mceAtlas'),('H03','aerDelta'),('H04','xidCases'),('H05','f2fsGuard'),('H09','radioLedger')]:
            self.assertIn('reportFor(QStringLiteral("'+code+'")',s)
            self.assertIn('code == QStringLiteral("'+code+'")) return er::'+fn,dispatch)
        for field in ['aer','xid_cases','mce','f2fs_events','radio_events']:
            self.assertIn('scan.value(QStringLiteral("'+field+'"))',reports)
        self.assertIn('new HealthEvidencePage',src('gui/src/healthtab.cpp'))
        self.assertIn('helperPath(QStringLiteral("health-reader"))',dispatch)

    def test_classification_and_gui_trigger_both_updated(self):
        r=src('crates/centurion-helpers/src/health.rs')
        ui=src('gui/src/healthtab.cpp')
        for term in ['"f2fs"','"bluetooth"','"wifi"']:
            self.assertIn(term,r)
        for term in ['"F2FS-fs"','"Bluetooth:"','"mt792"']:
            self.assertIn(term,ui)
        self.assertIn('QLatin1String("f2fs")',src('gui/src/eventinfo.cpp'))
        self.assertIn('QLatin1String("bluetooth")',src('gui/src/eventinfo.cpp'))

    def test_xid_154_not_claimed_root_cause(self):
        s=src('gui/src/healthtab.cpp')
        assert 'NOT proven cause' in s
        assert 'lastRootXidBoot_' in s and 'lastRootXidPci_' in s
        self.assertNotIn('title += QStringLiteral("  (cause:',s)
        d=src('crates/centurion-helpers/src/health_diagnostics.rs')
        self.assertIn('Temporal correlation only',d)

    def test_aer_samples_saved_per_boot_atomically(self):
        s=src('gui/src/healthevidence.cpp')
        reports=src('gui/src/evidencereports.cpp')
        store=src('gui/src/diagsources.cpp')
        for term in ['sameBoot','sameDevice','then >= 0 && cor >= 0','tr("reset")','No earlier scan of this boot']:
            self.assertIn(term,reports)
        self.assertIn('QSaveFile f(path)',store)
        self.assertIn('QFileDevice::ReadOwner | QFileDevice::WriteOwner',store)
        self.assertIn('toString().isEmpty() ? QString() : diagsrc::saveJson(samplePath()',s)

    def test_system_tools_registered(self):
        ui=src('gui/src/systools.cpp')
        for title in ['Hardware snapshot','EDID / VRR','NVIDIA module parameters']:
            self.assertIn(title,ui)
        for special in ['S01','S03','S06']:
            self.assertIn('{"@diag", "'+special+'"}',ui)
        for f in ['src/diagnostictools.cpp','src/healthevidence.cpp']:
            self.assertIn(f,src('gui/CMakeLists.txt'))

    def test_privileged_diagnostics_are_read_only(self):
        rust=src('crates/centurion-helpers/src/health_diagnostics.rs')
        for forbidden in ['std::fs::write','OpenOptions::new().write','Command::new','/dev/mem','/dev/cpu/', 'msr::write']:
            self.assertNotIn(forbidden,rust)
        cpp=src('gui/src/diagnostictools.cpp')
        for forbidden in ['pkexec','nvidia-smi','NvAPI','/dev/mem','/dev/nvidiactl','QProcess']:
            self.assertNotIn(forbidden,cpp)
        assert 'diagsrc::saveJson(baselinePath()' in cpp
        assert cpp.count('diagsrc::saveJson(') == 1

    def test_edid_parser_has_checksum_and_bounds(self):
        s=src('gui/src/diagnostictools.cpp')
        for term in ['d.size() < 128','d.startsWith(QByteArray::fromHex','sum % 256','e.truncated = e.readable < e.declared',
                     'read(32 * 128)','A general refresh range is not proof of VRR support']:
            self.assertIn(term,s)

    def test_module_params_do_not_treat_comments_as_options(self):
        s=src('gui/src/diagnostictools.cpp')
        assert "line.section(QLatin1Char('#'), 0, 0)" in s
        for token in ['/etc/modprobe.d','/proc/driver/nvidia/params','/sys/module/','/proc/cmdline', 'not exposed']:
            assert token in s

    def test_snapshot_is_opt_in_and_redacts(self):
        s=src('gui/src/diagnostictools.cpp')
        assert 'Save baseline' in src('gui/src/systools.cpp') and 'allow.contains(name)' in s
        assert 'diagsrc::saveJson(baselinePath()' in s
        assert 'QFileDevice::ReadOwner | QFileDevice::WriteOwner' in src('gui/src/diagsources.cpp')
        assert 'hardwareSnapshot(' in s and 'loadHardwareBaseline(' in s

    def test_test_targets_exist(self):
        c=src('gui/CMakeLists.txt')
        assert 'centurion-diagnostictools-test' in c and 'add_test(NAME diagnostictools' in c
        r=src('crates/centurion-helpers/src/health_diagnostics.rs')
        for case in ['cpu_identifiers_do_not_infer_ccd','xid_154_only_preceding_same_gpu','radio_ledger_groups_device_without_inventing_hardware_ids']:
            assert f'fn {case}' in r

if __name__ == '__main__': unittest.main(verbosity=2)
