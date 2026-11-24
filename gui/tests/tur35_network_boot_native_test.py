#!/usr/bin/env python3
"""Pure process identity and retained boot record tests, no host hardware actions."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
code=r'''
#include "processidentity.h"
#include "bootcomparison.h"
#include <cassert>
#include <random>
#include <limits>
#include <string>
using namespace processidentity;
int main() {
    auto stat=[](uint64_t pid,uint64_t ticks) { std::string s=std::to_string(pid)+" (foo ) bar) S"; for(int i=0;i<18;++i) s+=" 0"; return s+" "+std::to_string(ticks)+" 0 0\n"; };
    assert(startTicks(stat(1,42),1)==42);
    assert(!startTicks(stat(1,42),2)); assert(!startTicks("1 (x) S 0",1));
    assert(!number("18446744073709551616")); assert(!number("-1")); assert(!number(" 1")); assert(!number(""));
    assert(!matches("","","1",1,"x","x")); assert(!matches("a","b","1",1,"x","x"));
    assert(!matches("a","a","1",2,"x","x")); assert(!matches("a","a","1",1,"x","y")); assert(matches("a","a","1",1,"x","x"));
    assert(!fresh(10,9,5)); assert(fresh(10,15,5)); assert(!fresh(10,16,5));
    assert(fresh(UINT64_MAX,UINT64_MAX,0));
    bootcomparison::Boot b;
    b.add(1,{"a","xid/154"}); b.add(1,{"a","xid/154"}); b.add(2,{"b","xid/154"});
    assert(b.duplicates==1 && b.counts().at("xid/154")==2);
    b.add(2,{"c","xid/79"}); b.add(2,{"b","xid/154"});
    assert(b.conflicts==1 && b.counts().at("xid/154")==1 && !bootcomparison::observed(b.counts(),"xid/79"));
    std::mt19937_64 gen(35);
    for (uint64_t i=0;i<30000;++i) {
        const auto n=gen(); assert(startTicks(stat(123,n),123)==n); assert(!startTicks(stat(123,n),124));
        assert(matches("boot","boot",std::to_string(n),n,"/wine","/wine")); assert(!matches("boot","boot",std::to_string(n),n^1,"/wine","/wine"));
        b.add(i+3,{"msg","mce"});
    }
    assert(b.capped && b.records.size()==20000 && b.counts().at("mce")==19998);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/'test.cpp';src.write_text(code)
    for flags in [[],['-O3','-flto'],['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-no-pie']]:
        exe=Path(tmp)/'test';subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(src),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
        print('PASS:', ' '.join(flags) or 'normal', '30000 fixtures')
