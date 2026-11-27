#!/usr/bin/env python3
"""Dependency-free integration and no-write security contracts for 17 on-demand diagnostic features.
Not a substitute for a compiled GUI/CLI integration test on Gentoo.
"""
from pathlib import Path
root=Path(__file__).resolve().parents[2]
read=lambda p:(root/p).read_text()
a=read('gui/src/advanceddiagnostics.cpp')
h=read('gui/src/advancedhealth.cpp')
s=read('gui/src/systools.cpp')
t=read('gui/src/healthtab.cpp')
dp=read('gui/src/diagnosticspage.cpp')
c=read('gui/CMakeLists.txt')
r=read('crates/centurion-helpers/src/health_nvme.rs')
reader=read('crates/centurion-helpers/src/bin/health-reader.rs')
ids=['H01','H06','H10','H12','H13','H15','H16','H17','H18','S04','S07','S08','S10','S11','S13','S15','S18']
checks={f'{id} wired': (id in (h+a) if id[0]=='H' else id in (s+a)) for id in ids}
checks.update({
    'investigation page registered': 'new AdvancedHealthPage' in dp and 'new DiagnosticsPage' in t,
    'advanced source built': 'src/advanceddiagnostics.cpp' in c and 'src/advancedhealth.cpp' in c,
    'fixture test configured': 'centurion-advanceddiagnostics-test' in c,
    'NVMe helper registered': 'pub mod health_nvme;' in read('crates/centurion-helpers/src/lib.rs') and 'Some("nvme_deep")' in reader,
    'NVMe only whitelisted operations': '"smart-log"' in r and '"error-log"' in r and '"format"' not in r,
    'NVMe output drained asynchronously': 'mpsc::sync_channel' in r and 'stdout.take(MAX_OUTPUT + 1)' in r,
    'NVMe timeout': 'child.kill()' in r and 'TIMEOUT' in r,
    'NVMe unknown format remains unknown': 'error log format unrecognized' in r,
    'GPU active query opt-in': 'activeGpuQuery()' in h and 'nvidia-smi' in h,
    'GPU active query bounded': 'QTimer::singleShot(5000' in h,
    'wizard category selection': 'category_->currentData()' in h,
    'safe incident note store': 'diagsrc::saveJson(path, all)' in a and 'QSaveFile f(path);' in read('gui/src/diagsources.cpp') and 'ReadOwner | QFileDevice::WriteOwner' in read('gui/src/diagsources.cpp'),
    'PCIe same-boot comparison': 'linkDelta' in read('gui/src/systemdiagnostics.cpp') and 'sameBoot' in read('gui/src/systemdiagnostics.cpp') and 'previousLinks_' in s,
    'boot baseline uncertainty': 'No boot has its versions recorded yet' in a and 'no recorded versions' in a,
    'no CO crash guard modifications': 'co-search-crash-guards.json' in a and 'readCrashGuards' not in a,
    'no automatic setting writes': not any(x in a for x in ['setpci', 'ryzenadj', 'nvidia-settings', 'efibootmgr', 'modprobe -r']),
})
for name,ok in checks.items(): print(('PASS' if ok else 'FAIL')+' '+name)
assert all(checks.values()),'New feature static integration checks failed'
print(f'PASS: {len(checks)} checks')
