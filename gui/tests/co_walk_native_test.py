#!/usr/bin/env python3
"""Compile the actual isolated C++ per-core candidate function (Qt not required).

This tests the range/binary-refinement arithmetic on every supported limit and
synthetic stability boundary; gui/tests/costrategy_test.cpp tests the full Qt
strategy and snapshot/confirmation behavior on systems with Qt 6.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/costrategy.cpp').read_text()
match = re.search(r'int Search::Walk::coreCandidate\(int limit\) const \{.*?\n\}', source, re.S)
assert match, 'per-core candidate implementation was not found'
assert 'stage_ = Stage::PerCore' in source, 'per-core must skip all-core pre-search'
assert 't.perCore = QList<int>(cores_.size(), 0);' in source, 'peers must stay stock in isolated trials'
assert 's.trialCycles * QUICK_SECONDS' in source, 'short probes must be timed as seconds'
compiler = shutil.which('g++')
if compiler is None:
    print('SKIP: g++ not installed; cannot run native arithmetic tests')
    raise SystemExit(0)

code = r'''
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <set>
struct Search {
  struct Walk {
    int pass=0, floor=0;
    bool coarse=true, done=false, seeded=false;
    int coreCandidate(int limit) const;
  };
};
''' + match.group() + r'''
int main() {
  int cases=0, probes=0, worst=0;
  for (int limit=-50; limit<=-5; ++limit) {
    for (int stable=limit; stable<=0; ++stable) {
      Search::Walk w;
      w.floor=limit-1;
      std::set<int> tried;
      int first=-100;
      int n=0;
      while (true) {
        const int c=w.coreCandidate(limit);
        if (c>0) break;
        if (n==0) first=c;
        assert(c<0 && c>=limit && c>w.floor && c<w.pass);
        assert(tried.insert(c).second && "a trial was repeated");
        if (c>=stable) w.pass=c;
        else { w.floor=c; w.coarse=false; }
        w.seeded=true;
        ++n;
        assert(n<100);
      }
      assert(first==std::max(limit,-15));
      assert(w.pass==stable);
      ++cases;
      probes+=n;
      worst=std::max(worst,n);
    }
  }
  std::printf("PASS: %d native C++ stability boundaries, %d probes, worst %d probes/core\\n", cases,probes,worst);
}
'''
with tempfile.TemporaryDirectory(prefix='centurion-co-walk-') as tmp:
    src = Path(tmp) / 'walk.cpp'
    exe = Path(tmp) / 'walk-test'
    src.write_text(code)
    subprocess.run([compiler, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
