#!/usr/bin/env python3
"""Qt-free native simulation of the crash guard invariants with manual CO controls.

Compiles the ACTUAL `Tuning`, `Walk` and `coreCandidate()` (costrategy.h / .cpp) to check that a value
that crashed the machine is never probed again on that core: not later in the same search (resumed
after the reboot, with the tuning changed), and not in a NEW search with any start or step.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/costrategy.cpp').read_text()
header = (root / 'src/costrategy.h').read_text()
function = re.search(r'int Search::Walk::coreCandidate\(int limit, Tuning tuning\) const \{.*?\n\}', source, re.S)
tuning = re.search(r'struct Tuning \{.*?\n\};', header, re.S)
walk = re.search(r'    struct Walk \{.*?\n    \};', header, re.S)
assert function and tuning and walk, 'native test could not locate current search implementation'
# Mirrored below: Search::report() (Stage::PerCore) and guardAgainstCrash(); fail if they change.
for fragment in ['if (w.up) w.done = true;', 'w.floor = std::max(w.floor, cand_);',
                 'if (w.pass < 0) w.done = true;\n            else w.up = true;',
                 'w.floor = std::max(w.floor, failed);\n        if (w.pass <= w.floor) w.pass = std::min(0, w.floor + 1);']:
    assert fragment in source, f'costrategy.cpp changed: {fragment!r}'
compiler = shutil.which('g++')
if compiler is None:
    print('SKIP: g++ not installed')
    raise SystemExit(0)
code = '#include <algorithm>\n#include <cassert>\n#include <cstdio>\n#include <set>\n' + tuning.group() + '''
struct Search {
''' + walk.group() + '''
};
''' + function.group() + r'''
static void report(Search::Walk &w, int cand, bool pass) {
    w.seeded = true;
    if (pass) { w.pass = cand; if (w.up) w.done = true; }
    else { w.floor = std::max(w.floor, cand); if (w.pass < 0) w.done = true; else w.up = true; }
}
static void guard(Search::Walk &w, int failed) {
    w.floor = std::max(w.floor, failed);
    if (w.pass <= w.floor) w.pass = std::min(0, w.floor + 1);
}
/// Runs a walk to the end; every probe must stay above `crash` (0 = none recorded).
/// `crashAt` >= 0: that probe crashes the machine; returns the crashed value (0 = no crash happened).
static int run(Search::Walk &w, int limit, Tuning t, int stable, int crashAt, int crash, Tuning after) {
    int n = 0, crashed = 0;
    std::set<int> tried;
    for (;;) {
        const int c = w.coreCandidate(limit, t);
        if (c > 0) { w.done = true; break; }
        assert(c < 0 && c >= limit && c > w.floor && c < w.pass);
        assert(tried.insert(c).second);
        assert((crash == 0 || c > crash) && "crash guard repeated");
        assert((crashed == 0 || c > crashed) && "crash value retried after the reboot");
        if (n++ == crashAt) {
            crashed = c;
            report(w, c, false);  // the resumed search counts it as failed ...
            guard(w, c);          // ... and the persistent guard holds it
            t = after;            // settings changed after the reboot
        } else report(w, c, c >= stable);
        assert(n < 100 && w.floor < w.pass);
        if (w.done) break;
    }
    return crashed;
}

int main() {
    const Tuning profiles[] = {{-5, 1}, {-15, 3}, {-25, 9}, {-40, 10}, {-5, 5}, {-35, 7}};
    const Tuning aggressive{-50, 10};
    long crashes = 0, clean = 0;
    for (Tuning t : profiles)
        for (int limit = -50; limit <= -5; ++limit)
            for (int start : {0, -1, -15, -30, -50})
                for (int stable = limit; stable <= 0; ++stable)
                    for (int crashAt = -1; crashAt < 12; ++crashAt) {
                        Search::Walk w{0, limit - 1, true, false, false};
                        w.start = std::clamp(start, limit, 0);
                        const int c = run(w, limit, t, stable, crashAt, 0, aggressive);
                        if (c == 0) { ++clean; continue; }
                        ++crashes;
                        assert(w.pass > c);
                        // A new search with the guard applied, any start and step: never at or below the crash.
                        for (Tuning n : {aggressive, Tuning{-5, 1}})
                            for (int s2 : {0, -50, c, c + 1}) {
                                Search::Walk fresh{0, limit - 1, true, false, false};
                                fresh.start = std::clamp(s2, limit, 0);
                                guard(fresh, c);
                                run(fresh, limit, n, stable, -1, c, n);
                                assert(fresh.pass > c);
                            }
                    }
    std::printf("PASS: %ld crash scenarios + %ld clean scenarios; guard survives manual tuning and new searches\n", crashes, clean);
}
'''
with tempfile.TemporaryDirectory(prefix='centurion-co-crash-tuning-') as tmp:
    src = Path(tmp) / 'test.cpp'
    exe = Path(tmp) / 'test'
    src.write_text(code)
    subprocess.run([compiler, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
