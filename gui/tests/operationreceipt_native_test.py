#!/usr/bin/env python3
"""Exercise actual registry transition and receipt classification without Qt."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
code=r'''
#include "operationregistry.h"
#include "tunereceipt.h"
#include <cassert>
#include <limits>
int main() {
    using namespace operations;
    Registry r;
    const auto a=r.begin("tune-helper"), b=r.begin("health-reader");
    assert(a!=b && r.active().size()==2);
    assert(r.transition(a,Phase::TimedOut));
    assert(!r.transition(a,Phase::OwnerGone));
    assert(r.active().size()==2);
    assert(r.finish(b,Outcome::ReportedOk));
    assert(r.active().size()==1);
    assert(r.finish(a,Outcome::ReportedOk)); // a late success after timeout is not success
    assert(r.history().back().outcome==Outcome::Unknown && r.active().empty());
    assert(!r.finish(a,Outcome::ReportedOk));
    const auto c=r.begin("restore");
    assert(r.transition(c,Phase::OwnerGone) && r.active().size()==1);
    assert(r.finish(c,Outcome::ReportedFailure));
    for(int i=0;i<300;++i) r.finish(r.begin("test"),Outcome::NotStarted);
    assert(r.history().size()==200 && r.active().empty());
    using namespace tunereceipt;
    Evidence e{true,true,false,2,0,3,3,0,0};
    assert(classify(e)==Status::Match);
    e.written=0; assert(classify(e)==Status::Match); // already matching
    e.matched=2; e.missing=1; assert(classify(e)==Status::Unverified);
    e.mismatched=1; assert(classify(e)==Status::Mismatch);
    e.mismatched=0; e.refused=1; e.written=1; assert(classify(e)==Status::Partial);
    e.ok=false; assert(classify(e)==Status::Partial);
    e.written=0; assert(classify(e)==Status::Failed);
    e.ok=true; e.skipped=true; assert(classify(e)==Status::Skipped);
    e.skipped=false; e.refused=0; e.expected=-1; assert(classify(e)==Status::Unverified);
    e.valid=false; assert(classify(e)==Status::Unknown);
    e={true,true,false,1,0,3,4,0,0}; assert(classify(e)==Status::Unverified);
    e={true,true,false,1,0,3,1,std::numeric_limits<long long>::max(),0}; assert(classify(e)==Status::Unverified);
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d)/'test.cpp'; p.write_text(code)
    flags=os.environ.get('CENTURION_TEST_CXXFLAGS','-O2').split()
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(p),'-o',str(Path(d)/'test')],check=True)
    subprocess.run([str(Path(d)/'test')],check=True)
print('PASS helper lifetime and receipt classification')
