#!/usr/bin/env python3
"""Exact monotonic SMART counters; independent of Qt and physical drives."""
from pathlib import Path
import tempfile, subprocess
root = Path(__file__).resolve().parents[1]
code = r'''
#include "smarttrend.h"
#include <cassert>
#include <limits>
int main() {
    using namespace smarttrend;
    constexpr auto top = std::numeric_limits<std::int64_t>::max();
    assert(increase(top-1,top)==1);
    assert(increase(0,top)==top);
    assert(increase(top,top)==0);
    assert(!increase(top,0)); assert(!increase(-1,top));
    assert(!increase({},1)); assert(!increase(1,{}));
    Interval empty; assert(!empty.delta(0));
    Interval stable; stable.observe({}); stable.observe(top-2); stable.observe(top-1);
    assert(stable.delta(top)==2); assert(stable.delta(top-1)==1); assert(!stable.delta(0));
    Interval reset; reset.observe(100); reset.observe(2); reset.observe(101); assert(!reset.delta(102));
    Interval missing; missing.observe(0); missing.observe({}); assert(missing.delta(0)==0);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/'test.cpp'; src.write_text(code); exe=Path(tmp)/'test'
    for flags in [['-O2'], ['-O3','-flto=auto','-fno-fat-lto-objects'], ['-O1','-fsanitize=address,undefined','-fno-sanitize-recover=undefined','-fno-omit-frame-pointer','-no-pie']]:
        subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(src),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
        print('PASS:', ' '.join(flags))
