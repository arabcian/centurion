#!/usr/bin/env python3
"""Source-only regression contracts for the user-requested Tour 29 changes."""
from pathlib import Path
root=Path(__file__).resolve().parents[2]
def read(p): return (root/p).read_text()
def gui(p): return read('gui/src/'+p)
assert '#include "sysinfo.h"' in gui('hometab.h')
assert 'PresentationReading' in gui('sysinfo.h')
assert 'setSizes({48,' in gui('mainwindow.cpp') and 'QLayout::SetNoConstraint' in gui('mainwindow.cpp')
assert 'Having a problem? Pick what is going wrong:' in gui('overviewpage.cpp')
assert 'healthSymptomTile' not in gui('healthtab.cpp') and 'Use System Overview to start guided triage' in gui('advancedhealth.cpp')
assert 'qualityTable' in gui('optimizetab.cpp') and 'profilediff::display(receipt)' in gui('optimizetab.cpp')
assert 'scenes::sceneObject' in gui('scenestab.cpp') and 'Firmware limits' in gui('profilediff.h')
assert 'x.isUndefined()' in gui('profilediff.h') and 'y.isUndefined()' in gui('profilediff.h')
assert 'show all events'.lower() in gui('scenestab.cpp').lower() and 'groups.value(e.id' in gui('scenestab.cpp')
assert 'mismatch_count' in gui('scenes.cpp') and 'readback_missing' in gui('scenes.cpp')
assert 'scenes::activeScene()==n' in gui('scenestab.cpp') and '? theme::OK' in gui('scenestab.cpp')
assert 'captured_ = entries;' in gui('kernelparamspage.cpp') and 'QSaveFile f(path)' in gui('kernelparamspage.cpp')
assert '"entries", captured_' in gui('kernelparamspage.cpp') and 'value_limit_bytes' in gui('kernelparamspage.cpp')
assert 'op == "ksnap" ? 80 * 1024 * 1024 : 4 * 1024 * 1024' in gui('privileged.cpp')
assert 'refreshDebugfs' in gui('optimizetab.cpp') and 'debugfsRefreshing_' in gui('optimizetab.cpp')
rust=read('crates/centurion-helpers/src/nvreg.rs')
assert '"-p", module' in rust and 'crate::trusted_path' in rust and 'validate_dynamic' in rust
assert 'let catalog = discover();' in rust and 'file unchanged' in rust
assert 'iommu.strict=0' in gui('bootadvisor.cpp') and 'cpuhp.parallel=0' in gui('bootadvisor.cpp')
print('PASS: Tour 29 user-request source contracts (not Qt/Rust behavior tests)')
