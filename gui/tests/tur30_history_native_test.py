#!/usr/bin/env python3
"""Native pure C++ reconciliation and boot provenance; no Qt or host reads."""
from pathlib import Path
import tempfile, subprocess
root=Path(__file__).resolve().parents[1]
code=r'''
#include "incrementalrows.h"
#include "diagnostichistory.h"
#include <cassert>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>
using namespace incrementalrows;
using namespace diagnostichistory;
static void check(const std::vector<int>& old,const std::vector<int>& next) {
    const auto p=plan(old,next);assert(p.valid);auto x=old;
    for(auto it=p.remove.rbegin();it!=p.remove.rend();++it) x.erase(x.begin()+it->first,x.begin()+it->first+it->count);
    for(const auto &r:p.insert) x.insert(x.begin()+r.first,next.begin()+r.first,next.begin()+r.first+r.count);
    assert(x==next);
}
int main() {
    const std::string a="12345678-ABCD-1234-ABCD-123456789ABC",b="12345678-abcd-1234-abcd-123456789abd";
    assert(fullBoot(a)=="12345678-abcd-1234-abcd-123456789abc");
    assert(relation(a,"12345678-abcd-1234-abcd-123456789abc")==Relation::SameCaptureBoot);
    assert(relation(a,b)==Relation::DifferentCaptureBoot);
    for(const std::string s:{"","12345678","00000000-0000-0000-0000-000000000000","12345678-abcd-1234-abcd-123456789abz","12345678-abcd-1234-abcd-123456789abc "}) assert(relation(a,s)==Relation::Unknown);
    assert(!plan(std::vector<int>{1,1},std::vector<int>{1}).valid);
    assert(!plan(std::vector<int>{3,2},std::vector<int>{1}).valid);
    std::vector<int> many(10000);std::iota(many.begin(),many.end(),0);
    const auto same=plan(many,many);assert(same.valid&&same.remove.empty()&&same.insert.empty());
    auto changed=many;changed.erase(changed.begin()+42,changed.begin()+49);changed.push_back(10001);check(many,changed);
    std::mt19937 rng(31);
    for(int trial=0;trial<20000;++trial){std::vector<int>x,y;for(int j=0;j<100;++j){if(rng()%2)x.push_back(j);if(rng()%2)y.push_back(j);}check(x,y);}
    std::cout<<"PASS: 20000 structural reconciliations, 10k stable keys, invalid identities and full boot UUIDs\n";
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp)/'test.cpp';p.write_text(code);exe=Path(tmp)/'test'
    for flags in [['-O2'],['-O2','-flto'],['-O1','-fsanitize=address,undefined','-fno-sanitize-recover=undefined','-fno-omit-frame-pointer','-no-pie']]:
        subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(p),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
