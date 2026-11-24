#!/usr/bin/env python3
"""Test the real shared finite axis math without Qt."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]
code = r'''
#include "chartaxis.h"
#include <cassert>
#include <iostream>
#include <random>
using chartaxis::Range;
int main() {
    const double m=std::numeric_limits<double>::max(), tiny=std::numeric_limits<double>::denorm_min();
    assert(!Range::make(0,INFINITY)); assert(!Range::make(NAN,0));
    auto verify=[](double a,double b) {
        const auto r=*Range::make(a,b); const auto t=r.ticks();
        assert(t.front()==r.low && t.back()==r.high);
        double prev=-1;
        for (double v:t) { const auto f=r.fraction(v); assert(std::isfinite(f)&&f>=0&&f<=1&&f>prev); prev=f; }
        if (a==b) assert(r.fraction(a)==0.5);
        else { assert(r.fraction(r.low)==0); assert(r.fraction(r.high)==1); }
    };
    verify(-m,m); verify(m,m); verify(0,tiny); verify(-tiny,tiny); verify(-10,-1); verify(0.00001,0.00002);
    verify(m,std::nextafter(m,0.));
    std::mt19937_64 rng(42); std::uniform_real_distribution<double> d(-1,1);
    for(int i=0;i<100000;++i) verify(std::ldexp(d(rng),int(rng()%2097)-1074),std::ldexp(d(rng),int(rng()%2097)-1074));
    std::cout<<"PASS: 100000 axis ranges and extreme/subnormal/negative fixtures\n";
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/'test.cpp'; src.write_text(code)
    for flags in [[],['-O2'],['-fsanitize=address,undefined','-fno-sanitize-recover=undefined','-fno-omit-frame-pointer','-no-pie']]:
        binary=Path(tmp)/'test'
        subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(src),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
