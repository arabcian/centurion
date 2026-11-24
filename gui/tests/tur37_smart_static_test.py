#!/usr/bin/env python3
"""Source wiring regressions. This is NOT a Qt compile/runtime test."""
from pathlib import Path
src=Path(__file__).resolve().parents[1]/'src'
p=(src/'smartpage.cpp').read_text(); h=(src/'smarthistory.cpp').read_text(); w=(src/'smartwatch.cpp').read_text()
assert 'all.constBegin()' in p and 'all.constEnd()' in p
assert 'disks.constBegin()' in h and 'disks.constEnd()' in h
assert all('.cbegin()' not in s and '.cend()' not in s for s in (p,h,w))
assert 'smart::requestSample(this' in p and 'smart::requestSample(this' in w
assert w.count('"smart_json"')==1 and 'privileged::runUntilExit' in w
assert 'flight.active = false;' in w and 'if (flight.active)' in w and 'if (w.owner) w.callback' in w
assert '!smart::automaticSamplingEnabled()' in w and '"health/smartAutomaticSampling"), false' in h
assert 'if (complete) hist[QStringLiteral("sampled")] = today;' in h
assert 'if (accepted == 0) return {RecordStatus::NoUsableSamples' in h
assert 'f.commit()' in h and 'RecordStatus::Unavailable' in h
assert 'day >= cutoff && day < today' in p and 'interval.delta(media)' in p
assert 'status_->setText(tr("SMART check timed out; waiting for the helper to exit before another check.")' in p
print('PASS: SMART Qt iterator API, single shared helper, opt-in, complete-day marker, historical baseline, terminal timeout wiring')
