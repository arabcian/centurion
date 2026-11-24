#!/usr/bin/env python3
"""Source wiring checks only; Qt behavior is covered by native CTest fixtures."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
co=(root/'src/cosearchdialog.cpp').read_text()
priv=(root/'src/privileged.cpp').read_text()
scene=(root/'src/scenes.cpp').read_text()
telemetry=(root/'src/telemetryservice.cpp').read_text()
assert co.count('privileged::runUntilExit(privileged::helperPath("ryzen-co-helper")')==4
assert 'cpuLease_ = privileged::acquireCpuCurveLease(this);' in co
assert '}, cpuLease_);' in co
assert 'void CoSearchDialog::requestStockReset' in co and 'QTimer::singleShot(250, this' in co
assert 'recoveryCancelled_ = true; stop_->setEnabled(false);' in co
assert 'search_.reset(); replay_.reset();' in co
assert co.index('if (recoveryCancelled_)', co.index('auto discard =')) < co.index('if (removeJournal()) after();', co.index('beginRecovery();', co.index('auto discard =')))
assert 'if (waitForExit) claims.recoveryOverride = false;' in priv
assert 'registry.finish(operation, !r.reached' in priv
assert 'privileged::runUntilExit(privileged::helperPath(name)' in scene
assert 'if (delivered >= it->deadline)' in telemetry
assert 'for (int i : w.pending)' in telemetry
assert 'CENTURION_PRIVILEGED_FIXTURE' in (root/'CMakeLists.txt').read_text()
print('PASS source wiring for terminal exit, CO cancellation, lease and telemetry deadline; not Qt execution')
