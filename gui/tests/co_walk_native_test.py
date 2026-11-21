#!/usr/bin/env python3
"""Native C++ tests of the actual per-core search candidate implementation (without Qt).

`Tuning`, `Walk` and `coreCandidate()` are taken from costrategy.h / costrategy.cpp as they are, so
the test cannot drift from the code. It checks the per-core rules: every start, step, limit and
stability threshold; the first failure after a pass ends the core; a failed start climbs back
towards stock; nothing is probed twice, past the limit or at/below a known failure; the result is
never an unstable value and is within one step of the threshold (exact with a step of 1).
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
# The simulation below mirrors Search::report() (Stage::PerCore) and setStart(); fail if they change.
for fragment in ['if (w.up) w.done = true;', 'w.floor = std::max(w.floor, cand_);',
                 'if (w.pass < 0) w.done = true;\n            else w.up = true;',
                 'per_[i].start = std::clamp(value, limit_, 0);', 'per_.append(Walk{0, limit_ - 1, true, false, false});']:
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
// Search::report(), per-core stage.
static void report(Search::Walk &w, int cand, bool pass) {
    w.seeded = true;
    if (pass) { w.pass = cand; if (w.up) w.done = true; }
    else { w.floor = std::max(w.floor, cand); if (w.pass < 0) w.done = true; else w.up = true; }
}

int main() {
    long cases = 0, probes = 0;
    int maxProbes = 0;
    for (int step = 1; step <= 10; ++step) {
        const Tuning t{-5, step};
        assert(t.valid());
        for (int limit = -50; limit <= -5; ++limit)
            for (int start = limit; start <= 0; ++start)
                for (int stable = limit; stable <= 0; ++stable) {
                    Search::Walk w{0, limit - 1, true, false, false};
                    w.start = std::clamp(start, limit, 0);
                    std::set<int> seen;
                    int n = 0;
                    for (;;) {
                        const int c = w.coreCandidate(limit, t);
                        if (c > 0) { w.done = true; break; }
                        if (n == 0) assert(c == (start < 0 ? start : std::max(-step, limit)));
                        assert(c < 0 && c >= limit && c > w.floor && c < w.pass);
                        assert(seen.insert(c).second && "a value was probed twice");
                        report(w, c, c >= stable);
                        ++n;
                        assert(n < 60);
                        if (w.done) break;
                    }
                    // Never an unstable value; within one step of the threshold (exact with a step of 1).
                    assert(w.pass >= stable && w.pass < stable + step);
                    if (step == 1) assert(w.pass == stable);
                    ++cases; probes += n; maxProbes = std::max(maxProbes, n);
                }
    }
    {
        // A tuning change during the run only affects the next candidate.
        Search::Walk w{0, -51, true, false, false};
        w.start = -15;
        assert(w.coreCandidate(-50, {-5, 5}) == -15);
        report(w, -15, true);
        assert(w.coreCandidate(-50, {-5, 5}) == -20);
        assert(w.coreCandidate(-50, {-5, 9}) == -24);
        report(w, -24, false);  // first failure after a pass: done, result -15
        assert(w.done && w.pass == -15 && w.coreCandidate(-50, {-5, 1}) > 0);
    }
    std::printf("PASS: %ld native per-core walks, %ld probes, max %d probes/core\n", cases, probes, maxProbes);
}
'''
with tempfile.TemporaryDirectory(prefix='centurion-co-walk-') as tmp:
    src = Path(tmp) / 'test.cpp'
    exe = Path(tmp) / 'test'
    src.write_text(code)
    subprocess.run([compiler, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
