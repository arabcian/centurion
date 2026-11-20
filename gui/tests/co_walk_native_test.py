#!/usr/bin/env python3
"""Native C++ tests of the actual per-core search candidate implementation (without Qt).

Cover configurable starting offsets and coarse/refine steps, first failure,
1-point convergence, dynamic tuning, guard boundaries, and no repeated probes.
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
assert function and tuning, 'native test could not locate current search implementation'
assert 'stage_ = Stage::PerCore' in source
assert 't.perCore = QList<int>(cores_.size(), 0);' in source
assert 's.trialCycles * QUICK_SECONDS' in source
compiler = shutil.which('g++')
if compiler is None:
    print('SKIP: g++ not installed')
    raise SystemExit(0)

code = '''#include <algorithm>\n#include <cassert>\n#include <cstdio>\n#include <set>\n'''+tuning.group()+'''
struct Search {
  struct Walk {
    int pass=0, floor=0;
    bool coarse=true, done=false, seeded=false;
    int coreCandidate(int limit, Tuning tuning) const;
  };
};
'''+function.group()+r'''
int main() {
    const Tuning profiles[] = {
        {-15, 5, 3}, {-10, 2, 3}, {-28, 8, 3}, {-45, 20, 5},
        {-3, 1, 1}, {-25, 9, 2}, {-15, 5, 10}, {-15, 20, 3}, {-50, 20, 3}
    };
    int cases=0, probes=0, maxProbes=0;
    for (auto tuning : profiles) {
      assert(tuning.valid());
      for (int limit=-50; limit<=-5; ++limit)
        for (int stable=limit; stable<=0; ++stable) {
          Search::Walk w;
          w.floor=limit-1;
          std::set<int> seen;
          int n=0;
          while (true) {
            const int c=w.coreCandidate(limit,tuning);
            if (c>0) break;
            if (n==0) assert(c==std::max(limit,tuning.initial));
            assert(c<0 && c>=limit && c>w.floor && c<w.pass);
            assert(seen.insert(c).second && "a value was probed twice");
            if (c>=stable) w.pass=c;
            else { w.floor=c; w.coarse=false; }
            w.seeded=true;
            ++n;
            assert(n<90);
          }
          assert(w.pass==stable && "stability boundary not found");
          ++cases; probes+=n; maxProbes=std::max(maxProbes,n);
        }
    }
    {
      Tuning t{-15,5,3};
      Search::Walk w;
      w.floor=-31;
      assert(w.coreCandidate(-30,t)==-15);
      w.seeded=true; w.pass=-15;
      assert(w.coreCandidate(-30,t)==-20);
      w.pass=-20;
      assert(w.coreCandidate(-30,t)==-25);
      w.floor=-25; w.coarse=false;
      assert(w.coreCandidate(-30,t)==-23); // 3-point post-failure step
      w.floor=-23;
      assert(w.coreCandidate(-30,t)==-22); // 2-point precision
      w.floor=-22;
      assert(w.coreCandidate(-30,t)==-21); // 1-point precision
    }
    {
      Search::Walk w;
      w.floor=-51;
      auto t=Tuning{-15,5,3};
      w.seeded=true; w.pass=-15;
      t.coarseStep=9; // changed during the run
      assert(w.coreCandidate(-50,t)==-24);
      w.floor=-24; w.coarse=false; // an interrupted -25, with guard -24
      assert(w.coreCandidate(-50,t)>=-23);
      t.initial=-50; t.coarseStep=20; t.refineStep=5;
      assert(w.coreCandidate(-50,t)>w.floor);
    }
    {
      // With crash guard (-25 and -24 excluded) an aggressive initial or coarse
      // step cannot select either unsafe value.
      Search::Walk w;
      w.floor=-24;
      Tuning t{-50,20,3};
      assert(w.coreCandidate(-50,t)==-23);
      w.seeded=true; w.pass=-23;
      assert(w.coreCandidate(-50,t)>0);
    }
    std::printf("PASS: %d native tunable CO boundaries, %d probes, max %d probes/core\n",cases,probes,maxProbes);
}
'''
with tempfile.TemporaryDirectory(prefix='centurion-co-tuning-') as tmp:
    src = Path(tmp) / 'test.cpp'
    exe = Path(tmp) / 'test'
    src.write_text(code)
    subprocess.run([compiler, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
