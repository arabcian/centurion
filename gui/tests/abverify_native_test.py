#!/usr/bin/env python3
"""Compile the presentation-only ABBA evidence classifier without Qt."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
code = r'''
#include "abverify.h"
#include <cassert>
#include <limits>
using namespace abverify;
int main() {
    Evidence e{true,true,true,true,false,true,true,3,0,0,Pair{.2,.01},Pair{.1,.3}};
    assert(status(e)==Status::ReportedPass);
    e.presetOoms=1; assert(status(e)==Status::Inconsistent);
    e.passed=false; assert(status(e)==Status::NotPassed);
    e.presetOoms=0; e.decision=Pair{-.1,.3}; assert(status(e)==Status::NotPassed);
    e.rounds=2; e.complete=false; assert(status(e)==Status::Incomplete);
    e.rounds=3; e.ended=true; assert(status(e)==Status::Incomplete);
    e.passed=true; assert(status(e)==Status::Inconsistent);
    e.passed=false; e.ended=false; e.complete=true; e.gain=Pair{1,-1}; assert(status(e)==Status::Incomplete);
    e.gain=Pair{1,0}; e.decision=Pair{2,1}; assert(status(e)==Status::Incomplete);
    e.decision=Pair{0,std::numeric_limits<double>::infinity()}; assert(status(e)==Status::Incomplete);
    e.decision=Pair{.1,.3}; e.validContext=false; assert(status(e)==Status::Unknown);
    e.passed=true; assert(status(e)==Status::Unknown);
    e.schema=false; assert(status(e)==Status::Unknown);
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d)/'test.cpp'; p.write_text(code)
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++20','-Wall','-Wextra','-Werror','-O2','-I',str(root/'src'),str(p),'-o',str(Path(d)/'test')],check=True)
    subprocess.run([str(Path(d)/'test')],check=True)
print('PASS ABBA evidence status and malformed/partial outcomes')
