#!/usr/bin/env python3
"""Exercise the exact semantic and measured-pair comparison algorithms without Qt."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
code=r'''
#include "tuningsemantic.h"
#include "frametimepair.h"
#include <cassert>
#include <limits>
#include <random>
#include <string>
using namespace tuningsemantic;
int main() {
    const auto v=[](std::string_view s){return Value{true,true,s};};
    assert(compare(Kind::Integer,v("00042"),v("+42"))==Relation::Equal);
    assert(compare(Kind::Integer,v("-9223372036854775808"),v("-9223372036854775808"))==Relation::Equal);
    assert(compare(Kind::Integer,v("9223372036854775807"),v("9223372036854775806"))==Relation::Different);
    for(auto s:{"9223372036854775808","-9223372036854775809","1.1"," 1","1e2","+-1","+",""}) assert(compare(Kind::Integer,v(s),v("1"))==Relation::Invalid);
    assert(compare(Kind::Boolean,v("1"),v("01"))==Relation::Invalid);
    assert(compare(Kind::Choice,v("01"),v("1"))==Relation::Different);
    assert(compare(Kind::Unknown,v("1"),v("1"))==Relation::Unknown);
    assert(compare(Kind::Integer,{true,false,"1"},v("1"))==Relation::Unknown);
    assert(compare(Kind::Integer,{false,false,{}},v("0"))==Relation::LeftMissing);
    assert(compare(Kind::Integer,v("0"),{false,false,{}})==Relation::RightMissing);
    assert(compare(Kind::Integer,{false,false,{}},{false,false,{}})==Relation::BothMissing);
    std::mt19937_64 rng(73);
    for(int i=0;i<100000;++i) {
        auto a=static_cast<std::int64_t>(rng()),b=i%2?a:static_cast<std::int64_t>(rng());
        auto as=std::to_string(a),bs=std::to_string(b);
        assert(compare(Kind::Integer,v(as),v(bs))==(a==b?Relation::Equal:Relation::Different));
    }
    frametime::Log a,b; a.ms.assign(300,10.0);b.ms.assign(300,8.0);
    assert(!frametimepair::compare(a,b,false,true));assert(!frametimepair::compare(a,b,true,false));
    auto c=frametimepair::compare(a,b,true,true);assert(c);
    assert(std::abs(c->delta[0]+20.0)<1e-9 && std::abs(c->delta[4]-25.0)<1e-9);
    b.ms.resize(299);assert(!frametimepair::compare(a,b,true,true));b.ms.resize(300,8.0);
    b.rejected=16;assert(!frametimepair::compare(a,b,true,true));b.rejected=15;assert(frametimepair::compare(a,b,true,true));
    b.ms[0]=std::numeric_limits<double>::quiet_NaN();assert(!frametimepair::compare(a,b,true,true));
    b.ms[0]=8.;b.supported=false;assert(!frametimepair::compare(a,b,true,true));
    auto x=frametime::parse("interval_ns\n10000000\n"),y=frametime::parse("frametime\n10\n");assert(x.ms==y.ms);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/'test.cpp';src.write_text(code)
    binary=Path(tmp)/'test'
    flags=os.environ.get('CENTURION_TEST_CXXFLAGS','-O2').split()
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(src),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS exact semantic comparison and measured-log quality/delta fixtures (100000 integer comparisons)')
