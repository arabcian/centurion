#!/usr/bin/env python3
"""Independent native tests of the exact arithmetic used by the calibration viewer."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]
code = r'''
#include "calibrationmath.h"
#include <cassert>
#include <random>
#include <iostream>
using namespace calibrationmath;
int main() {
    assert(!prediction(NAN,1,0)); assert(!prediction(1,0,2)); assert(!prediction(1,-1,2));
    assert(!prediction(-std::numeric_limits<double>::max(),1,std::numeric_limits<double>::max()));
    assert(!prediction(0,std::numeric_limits<double>::denorm_min(),1));
    auto p=prediction(1,0.5,0); assert(p && p->error==-1 && p->z==-2 && p->withinTwoSigma);
    assert(!prediction(1,0.49,0)->withinTwoSigma);
    assert(ramMatches(100,102)); assert(!ramMatches(100,103)); assert(!ramMatches(102,99));
    assert(ramMatches(UINT64_MAX,UINT64_MAX-1)); assert(!ramMatches(UINT64_MAX,0));
    assert(!validWeight(4,0.49)); assert(!validWeight(6,1.1)); assert(!validWeight(9,0));
    assert(*effectiveWeight(0,1,1,1.25)==1.25); assert(*effectiveWeight(0,0.9,1,1.25)==0.9);
    assert(!effectiveWeight(4,0,1,1));
    std::mt19937_64 rng(11); std::uniform_real_distribution<double> d(-100,100);
    for(int i=0;i<100000;++i) {
        auto a=rng(),b=rng(); const auto diff=a>b?a-b:b-a;
        // Independent widened reference, matching the Rust u128 formula.
        assert(ramMatches(a,b)==(static_cast<__uint128_t>(diff)*50<=std::max<std::uint64_t>(a,1)));
        double mu=d(rng), obs=d(rng), sd=std::abs(d(rng))+0.001;
        auto r=prediction(mu,sd,obs); assert(r && std::abs(r->z-(obs-mu)/sd)<1e-9);
        assert(r->withinTwoSigma==(std::abs(r->z)<=2));
        const auto idx=rng()%9; double v=low(idx)+(high(idx)-low(idx))*std::generate_canonical<double,53>(rng);
        assert(validWeight(idx,v)); assert(effectiveWeight(idx,v,v,v)==v);
    }
    std::cout << "PASS: 100000 prediction, RAM and weight cases plus invalid/extreme fixtures\n";
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/'test.cpp'; src.write_text(code)
    for flags in [[],['-O2'],['-fsanitize=address,undefined','-fno-sanitize-recover=undefined','-fno-omit-frame-pointer','-no-pie']]:
        binary=Path(tmp)/'test'
        subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(src),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
