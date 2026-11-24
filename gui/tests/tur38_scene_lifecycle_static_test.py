#!/usr/bin/env python3
"""Source wiring only, not a Qt runtime fixture or hardware test."""
from pathlib import Path
src=Path(__file__).resolve().parents[1]/'src'
co=(src/'cosearchdialog.cpp').read_text()
sc=(src/'scenes.cpp').read_text()
li=(src/'lighting.cpp').read_text()
pr=(src/'privileged.cpp').read_text()
header=(src/'cosearchdialog.h').read_text()
assert 'return finalizing_ || running_' in header
assert 'if (running_ || cpuLease_ || finalizing_)' in co
restore=co.split('auto restoreScene = [this, finishRestoration]',1)[1]
assert 'applyTracked(priorScene_, cpuLease_' in restore
assert 'releaseCpuLease();' not in restore
assert 'if (!alive) return;' in restore
assert 'outcome == QStringLiteral("returned_unverified")' in restore
assert 'if (!done || !cpuLease || operationActive()' in sc
assert 'applyCpuProfile(c.name, true, done, trackedCpuLease_)' in sc
assert '}, {}, trackedCpuLease_);' in sc
assert 'auto tracked = std::exchange(trackedDone_, {});' in sc
finish=sc.split('void SceneEngine::finish()',1)[1].split('void SceneEngine::gpuDeferredFire()',1)[0]
assert finish.index('Q_EMIT finished(current_, ok_, log_);') < finish.index('if (tracked) tracked(outcomeReceipt);') < finish.index('busy_ = false;')
assert 'if (soft && step.undo) applied_.removeLast();' in sc
assert 'if (unknown) incomplete_ = true;' in sc
assert 'eff.isEmpty() || eff == p' not in sc
assert 'r.json.value("effective").toString(old)' not in sc
assert sc.count('privileged::run(')==1 # game prune emergency cleanup retained separately
assert 'runUntilExit(const QString &helper, const QByteArray &payload' in pr
assert 'if (waitForExit) claims.recoveryOverride = false;' in pr
assert 'privileged::runUntilExit(privileged::helperPath(QStringLiteral("lighting-helper"))' in li
on_timeout=li.split('QObject::connect(timer, &QTimer::timeout',1)[1].split('QObject::connect(proc, &QProcess::errorOccurred',1)[0]
assert 'finish(' not in on_timeout and 'proc->kill();' in on_timeout
assert 'if (!state->failure.isEmpty()) r.error = state->failure;' in li
assert 'reply.value("ok").isBool()' in li
assert 'blockingReset()' in co # emergency destructor fallback not removed
print('PASS: T38 Scene/CO terminal callback, explicit lease, honest partial result and lighting exit source wiring (not Qt execution)')
