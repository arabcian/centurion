#!/usr/bin/env python3
"""Source-contract checks only; not execution of Qt or Rust helpers."""
from pathlib import Path
import xml.etree.ElementTree as ET
root = Path(__file__).resolve().parents[2]
t = (root/'crates/centurion-helpers/src/bin/tune-helper.rs').read_text()
s = (root/'crates/centurion-helpers/src/tune.rs').read_text()
c = (root/'crates/centurion-helpers/src/bin/centurion-calibrate.rs').read_text()
g = (root/'gui/src/optimizetab.cpp').read_text()
j = (root/'gui/src/tunejournal.h').read_text()
assert 'fn load() -> Result<State, String>' in t and 'fn restore_scope' in t
assert 'RestoreEvidence::WrittenUnknown' in t and 'restore readback missing or different' in t
assert 'legacy alias overlaps' in t and 'storage_stamps' in t
assert 'O_NOFOLLOW | libc::O_CLOEXEC' in s and 'fd.metadata()' in s and 'fd.write(data.as_bytes())' in s
assert s.index('storage descriptor no longer matches') < s.index('fd.write(data.as_bytes())')
assert 'storage_stamp_detects_replaced_attribute' in s
assert 's[0].1 == 2 && s[1].1 == 2' in c and '!seen.insert(key)' in c
assert 'verification_v": centurion_helpers::abverify::VERSION' in c and '"passed": complete && verification_passes' in c
assert 'self.restore_settled()?' in c and 'fn load() -> Result<Journal, String>' in c
assert 'verification_v")).toDouble(-1) != 3.' in g and '"restore_confirmed"' in g
assert 'privileged::runUntilExit(helperForOp(op)' in g and '"per_key"' in g
assert 'victim < 0' in j and 'QStringLiteral("pending")' in j
ET.parse(root/'gui/i18n/centurion_tr.ts')
print('PASS source contracts (not native execution)')
