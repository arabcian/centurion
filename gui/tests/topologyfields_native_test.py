#!/usr/bin/env python3
"""Exercise strict cpulist/SMT and DPM parsing, independent of Qt and hardware."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]
code = r'''
#include "topologyfields.h"
#include <cassert>
#include <random>
#include <string>
#include <iostream>
using namespace topologyfields;
int main() {
    assert(cpuList("0-3,8,10-11\n") == std::vector<int>({0,1,2,3,8,10,11}));
    assert(cpuList("8,0") == std::vector<int>({0,8}));
    for(auto v:{"", "1,", "-1", "2-1", "1-5,3", "0-9999999999", "1,,2", "1-2-3", "1 2"}) assert(!cpuList(v));
    assert(!cpuList("0-256",256));
    Cpu a{0,0,1,0,cpuList("0,8")}, b{8,0,1,0,cpuList("8,0")};
    assert(sameCore(a,b)); b.die=1; assert(!sameCore(a,b)); b.die=0; b.siblings=cpuList("8"); assert(!sameCore(a,b));
    b.siblings=cpuList("0,8"); b.package.reset(); assert(!sameCore(a,b));
    const auto d=dpm("0: 500Mhz\n1: 1500 MHz *\n2: 0mhz\n");
    assert(d && d->size()==3 && (*d)[1].active && (*d)[1].mhz==1500 && !(*d)[0].active);
    for(auto v:{"", "0: 12GT/s", "0: 500Mhz *\n1: 900Mhz *", "0: 500Mhz\n0: 200Mhz", "0: -1MHz", "0: 1000001MHz", "0: 1.2MHz", "0: 3MHz garbage", "0: 3MHz\ninvalid"}) assert(!dpm(v));
    std::mt19937 rng(123); for(int i=0;i<30000;++i) {
        std::string s(rng()%120,' '); for(char &c:s)c=char(rng()%128);
        if(auto ids=cpuList(s)) { assert(std::is_sorted(ids->begin(),ids->end())); assert(std::adjacent_find(ids->begin(),ids->end())==ids->end()); }
        if(auto v=dpm(s)) { assert(v->size()<=256); std::set<int> seen; int active=0; for(auto x:*v) { assert(seen.insert(x.index).second); assert(x.mhz>=0); active+=x.active; } assert(active<=1); }
    }
    std::cout << "PASS: 30000 random parser inputs + cpulist, SMT, DPM fixtures\n";
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/'test.cpp'; src.write_text(code)
    for flags in [[],['-O2'],['-fsanitize=address,undefined','-fno-sanitize-recover=undefined','-fno-omit-frame-pointer','-no-pie']]:
        binary=Path(tmp)/'test'
        subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(src),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
