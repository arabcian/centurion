#!/usr/bin/env python3
"""Independent native flight bookkeeping/cache test; no Qt or hardware."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
code=r'''
#include "telemetrybroker.h"
#include <cassert>
int main() {
    telemetry::Flights<6> f;
    auto first=f.begin(0); assert(first && !f.begin(0));
    auto other=f.begin(1); assert(other && *other!=*first);
    assert(!f.finish(0,*other) && !f.begin(0));
    // Timeout/cancel does not release an acquisition. Only the matching token does.
    assert(!f.begin(0));
    assert(f.finish(0,*first));
    auto next=f.begin(0); assert(next && *next!=*first);
    assert(!f.finish(0,*first) && !f.begin(0));
    assert(f.finish(0,*next) && f.finish(1,*other));
    assert(!f.begin(6) && !f.finish(6,1));
    for(std::size_t i=0;i<6;++i) assert(f.begin(i));
    for(std::size_t i=0;i<6;++i) assert(!f.begin(i));
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d)/'test.cpp'; p.write_text(code)
    flags=os.environ.get('CENTURION_TEST_CXXFLAGS','-O2').split()
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(p),'-o',str(Path(d)/'test')],check=True)
    subprocess.run([str(Path(d)/'test')],check=True)
print('PASS shared telemetry flight tokens')
