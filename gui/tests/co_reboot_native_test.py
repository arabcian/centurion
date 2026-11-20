#!/usr/bin/env python3
"""Qt-free native simulation of crash guard invariants with manual CO controls.

Compile ACTUAL C++ candidate function to ensure step customization cannot bypass a
persisted crash boundary or replay a candidate after a reboot.
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
assert function and tuning
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
  const Tuning profiles[] = {{-15,5,3}, {-10,3,3}, {-25,9,3}, {-40,20,5}, {-5,1,1}, {-35,17,2}};
  int crashes=0,noCrash=0;
  for(auto initial : profiles)
    for(int limit=-50;limit<=-5;++limit)
      for(int stable=limit;stable<=0;++stable)
        for(int crashAt=0;crashAt<12;++crashAt) {
          Search::Walk w;
          w.floor=limit-1;
          int n=0,failed=1;
          std::set<int> tried;
          Tuning t=initial;
          for(;;) {
            const int c=w.coreCandidate(limit,t);
            if(c>0) break;
            assert(c<0&&c>=limit&&c>w.floor&&c<w.pass);
            assert(tried.insert(c).second);
            if(failed<0) assert(c>std::min(0,failed+1)&&"crash guard repeated");
            if(n++==crashAt) {
              failed=c;
              w.floor=std::max(w.floor,std::min(-1,c+1));
              if(w.pass<=w.floor)w.pass=std::min(0,w.floor+1);
              w.coarse=false;
              // Change settings after reboot: must not reopen protected negatives.
              t={-50,20,3};
            } else if(c>=stable) w.pass=c;
            else {w.floor=c;w.coarse=false;}
            w.seeded=true;
            assert(n<100);
            assert(w.floor<w.pass);
          }
          if(failed<0) {++crashes;assert(w.pass>=std::min(0,failed+2));}
          else ++noCrash;
        }
  std::printf("PASS: %d crash scenarios + %d clean scenarios; guard survives manual tuning\n",crashes,noCrash);
}
'''
with tempfile.TemporaryDirectory(prefix='centurion-co-crash-tuning-') as tmp:
    src=Path(tmp)/'test.cpp'
    exe=Path(tmp)/'test'
    src.write_text(code)
    subprocess.run([compiler,'-std=c++20','-O2','-Wall','-Wextra','-Werror',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
