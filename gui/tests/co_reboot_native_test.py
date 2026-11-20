#!/usr/bin/env python3
"""Qt-free native test of ACTUAL candidate function under simulated hard reboots.

The complete Qt6 strategy/recovery integration is exercised by costrategy_test.cpp
on a machine with Qt6 development headers installed. No hardware writes occur here.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'src/costrategy.cpp').read_text()
match = re.search(r'int Search::Walk::coreCandidate\(int limit\) const \{.*?\n\}', source, re.S)
assert match
compiler = shutil.which('g++')
if compiler is None:
    print('SKIP: no C++ compiler')
    raise SystemExit(0)
code = r'''
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <set>
struct Search {
  struct Walk {
    int pass=0,floor=0;
    bool coarse=true,done=false,seeded=false;
    int coreCandidate(int limit) const;
  };
};
''' + match.group() + r'''
int main() {
  int crashes=0, noCrash=0;
  for(int limit=-50; limit<=-5; ++limit)
    for(int stable=limit; stable<=0; ++stable)
      for(int crashAt=0; crashAt<12; ++crashAt) {
        Search::Walk w;
        w.floor=limit-1;
        int n=0, failed=1;
        std::set<int> tried;
        for(;;) {
          const int c=w.coreCandidate(limit);
          if(c>0) break;
          assert(c<0 && c>=limit && c>w.floor && c<w.pass);
          assert(tried.insert(c).second);
          if(failed<0) assert(c>std::min(0,failed+2)-1 && "unsafe crash boundary retried");
          if(n++ == crashAt) {
            failed=c;
            w.floor=std::max(w.floor,std::min(-1,c+1));
            if(w.pass<=w.floor) w.pass=std::min(0,w.floor+1);
            w.coarse=false;
          } else if(c>=stable) w.pass=c;
          else {w.floor=c;w.coarse=false;}
          w.seeded=true;
          assert(n<100);
          assert(w.floor<w.pass);
        }
        if(failed<0) {++crashes; assert(w.pass>=std::min(0,failed+2));}
        else ++noCrash;
      }
  std::printf("PASS: %d reboot cases + %d no-crash cases; no unsafe value replayed\n",crashes,noCrash);
}
'''
with tempfile.TemporaryDirectory(prefix='co-reboot-') as tmp:
    f = Path(tmp) / 'test.cpp'
    exe = Path(tmp) / 'test'
    f.write_text(code)
    subprocess.run([compiler, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', str(f), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
