#!/usr/bin/env python3
"""Actual GUI registry, independent of Qt and hardware."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
code = r'''
#include "operationregistry.h"
#include <cassert>
using namespace operations;
int main() {
    Registry r;
    const auto w=classify("ryzen-co-helper","set_coall");
    const auto rd=classify("ryzen-co-helper","probe");
    auto reset=classify("ryzen-co-helper","reset"); reset.recoveryOverride=false;
    const auto claim=std::vector<Claim>{{Resource::CpuCurve,Access::Write}};
    auto lease=r.tryLease("search",claim); assert(lease);
    assert(!r.tryLease("other search",claim));
    assert(!r.tryBegin("external write",w));
    assert(!r.tryBegin("scene reset",reset));
    auto read=r.tryBegin("probe",rd); assert(read);
    assert(!r.tryBegin("owned write",w,*lease)); // live reader conflicts
    assert(r.finish(*read,Outcome::ReportedOk));
    assert(!r.tryBegin("wrong resource",classify("tune-helper","apply"),*lease));
    assert(!r.tryBegin("empty claims",{},*lease));
    auto child=r.tryBegin("owned write",w,*lease); assert(child);
    assert(!r.releaseLease(*lease));
    assert(!r.finish(*lease,Outcome::ReportedOk));
    assert(r.transition(*child,Phase::TimedOut));
    assert(!r.tryBegin("owned reset",reset,*lease));
    assert(!r.tryBegin("external write",w));
    auto emergency=r.tryBegin("emergency",classify("ryzen-co-helper","reset")); assert(emergency);
    assert(r.finish(*child,Outcome::ReportedOk));
    assert(r.history().back().outcome==Outcome::Unknown);
    assert(!r.tryBegin("owned reset",reset,*lease));
    assert(r.finish(*emergency,Outcome::ReportedOk));
    auto restore=r.tryBegin("owned reset",reset,*lease); assert(restore);
    assert(!r.releaseLease(*lease));
    assert(r.finish(*restore,Outcome::ReportedOk));
    assert(!r.tryBegin("external write between reset and stock test",w));
    // The tracked previous Scene owns only the CPU step, not firmware or GPU.
    auto sceneCpu=r.tryBegin("scene CPU restore",w,*lease); assert(sceneCpu);
    assert(!r.tryBegin("tray CPU profile",w));
    assert(!r.tryBegin("scene GPU with CPU lease",classify("nvcurve-root-helper","apply_gpu_offsets"),*lease));
    assert(!r.releaseLease(*lease));
    assert(r.transition(*sceneCpu,Phase::TimedOut));
    assert(!r.releaseLease(*lease));
    assert(r.finish(*sceneCpu,Outcome::ReportedOk));
    assert(r.history().back().outcome==Outcome::Unknown);
    auto firmware=r.tryBegin("firmware",{}); assert(firmware);
    assert(!r.tryBegin("external CPU",w));
    assert(r.finish(*firmware,Outcome::ReportedFailure));
    assert(r.releaseLease(*lease));
    assert(!r.tryBegin("stale owner",w,*lease));
    auto lease2=r.tryLease("second",claim); assert(lease2);
    auto child2=r.tryBegin("owned write",w,*lease2); assert(child2);
    r.abandonLease(*lease2);
    assert(r.active().size()==2);
    assert(!r.tryBegin("new owner after destruction",w,*lease2));
    assert(!r.tryBegin("external write",w));
    assert(r.finish(*child2,Outcome::Unknown));
    assert(r.active().empty());
    auto lease3=r.tryLease("third",claim); assert(lease3);
    r.abandonLease(*lease3); assert(r.active().empty());
    assert(!r.releaseLease(*lease3));
    assert(r.tryBegin("new unowned",w));
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d)/'test.cpp'; p.write_text(code); exe=Path(d)/'test'
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++20','-Wall','-Wextra','-Werror',*os.environ.get('CENTURION_TEST_CXXFLAGS','-O2').split(),'-I',str(root/'src'),str(p),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS: scoped lease, timeout, emergency override, restoration and owner destruction')
