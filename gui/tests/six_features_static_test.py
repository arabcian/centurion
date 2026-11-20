#!/usr/bin/env python3
"""Dependency-free wiring/safety contract check. Not a C++ compile or hardware test.
Run: python3 gui/tests/six_features_static_test.py
"""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
src = root / 'src'
cmake = (root / 'CMakeLists.txt').read_text()
read = lambda name: (src / name).read_text()
scene = read('scenes.cpp')
ins = read('insights.cpp')
co = read('cosearchdialog.cpp')
checks = {
  'new GUI source linked': 'src/insights.cpp src/insights.h' in cmake and 'src/cosearchdialog.cpp src/cosearchdialog.h' in cmake,
  'unit test linked to CTest': 'add_test(NAME insights COMMAND centurion-insights-test)' in cmake,
  'incident timeline in Health': 'Hardware Incident Timeline' in read('healthtab.cpp') and 'insights::incident(' in read('healthtab.cpp'),
  'incident boot guard': 'ringBoot != b' in ins and 'if (b.isEmpty())' in ins,
  'scene tx log with boot identity': 'scene-transactions.jsonl' in scene and '"boot", currentBootId' in scene,
  'scene failure short circuits': 'if (!ok && !soft) {\n            steps_.clear();' in scene,
  'write-free failures do not roll back': 'std::exchange(softFail_, false) && !ok' in scene,
  'CO search reads live MCA counters': 'mceCounters()' in co and 'error_count' in co,
  'CO search resets when destroyed mid-search': '~CoSearchDialog()' in co and 'blockingReset()' in co,
  'scene rollback reverse order': 'applied_.takeLast()' in scene,
  'scene snapshot before fan write': '{"fan_fullspeed", "get"}' in scene,
  'scene fan curve snapshot': '{"fan_table", "get"}' in scene and 'oldFanTable_' in scene,
  'scene firmware old value snapshot': 'oldFirmware.append' in scene,
  'nonreversible scene outcomes warned': 'uncompensated_' in scene and 'Partial rollback' in scene,
  'autotune confidence UI': 'insights::confidence(d)' in read('optimizetab.cpp'),
  'pareto UI': 'insights::pareto(' in read('coolingpage.cpp'),
  'pareto context gating': 'g.size() < 2' in ins and 'highStart - lowStart > 3.0' in ins,
  'capability UI read-only': 'insights::capabilityPage()' in read('healthtab.cpp') and 'No device is woken or written' in ins,
  'CO search accessible': 'CoSearchDialog dlg(this)' in read('ryzentab.cpp'),
  'CO search stock guard': 'co-state.json' in co and 'knownStock' in co,
  'CO search user opt-in': 'Risk of a system crash' in co,
  'CO search has no user temperature cutoff': 'maxTemp_' not in co and 'tempLimit_' not in co and 'temperature()' not in co,
  'CO critical platform protection retained': 'e.value("level").toString() == "critical"' in co,
  'CO search no automatic commitment': '"op", "reset"' in co and 'Saved as Ryzen Curve profile' in co and 'load and apply it there' in co,
  'CO search hardware fault correlation': '"health-reader"' in co and 'kmsgSeq_' in co and 'validateFaultLog' in co and 'health.log")' not in co,
  'CO search watchdog': 'test_->terminate()' in co and 'pending->kill()' in co,
}
for title, passed in checks.items():
    print(('PASS' if passed else 'FAIL') + ': ' + title)
assert all(checks.values()), 'Static wiring/safety regression; no build was performed.'
print(f'PASS: {len(checks)} static source checks (not a compilation test)')
