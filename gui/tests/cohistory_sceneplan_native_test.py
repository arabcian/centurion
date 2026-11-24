#!/usr/bin/env python3
"""Exercise actual Qt-independent CO evidence accounting and Scene planner."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
code=r'''
#include "cohistoryfields.h"
#include "sceneplanfields.h"
#include "operationregistry.h"
#include <cassert>
#include <iostream>
int main(){
    using namespace cohistory;
    std::vector<Observation> v{{"a",0,0,-15,Result::Passed},{"b",0,0,-15,Result::Failed},
        {"c",0,0,-15,Result::Interrupted},{"d",0,0,-20,Result::Passed},{"e",1,0,-15,Result::Passed},
        {"dup",0,0,-15,Result::Passed},{"dup",0,0,-20,Result::Failed},{"",0,0,-15,Result::Passed},
        {"bad",0,0,-51,Result::Passed},{"zero",0,0,0,Result::Passed},{"unknown",0,0,-15,Result::Unknown}};
    auto s=summarize(v); assert(s.cells.size()==3&&s.rejected==6);
    const auto &c=s.cells.at({0,0,-15});assert(c.passed==1&&c.failed==1&&c.interrupted==1&&c.contradictory());
    assert(!s.cells.at({0,0,-20}).contradictory());assert(!s.cells.at({1,0,-15}).contradictory());
    using namespace sceneplan;
    for(unsigned mask=0;mask<(1u<<14);++mask)for(auto cpu:{Cpu::None,Cpu::Ryzen,Cpu::Intel}){
        auto bit=[&](int n){return bool(mask&(1u<<n));};
        Inputs i{bit(0),bit(1),bit(2),bit(3),bit(4),bit(5),bit(6),bit(7),bit(8),bit(9),bit(10),bit(11),bit(12),bit(13),cpu};
        const auto p=steps(i); assert(std::is_sorted(p.begin(),p.end()));assert(std::adjacent_find(p.begin(),p.end())==p.end());
        assert(contains(p,Step::FanBoostCheck)==(bit(0)&&bit(1)));
        assert(contains(p,Step::CpuRyzen)==(bit(3)&&cpu==Cpu::Ryzen));assert(contains(p,Step::CpuIntel)==(bit(3)&&cpu==Cpu::Intel));assert(contains(p,Step::CpuUnsupported)==(bit(3)&&cpu==Cpu::None));
        assert(contains(p,Step::Firmware)==bit(2));assert(contains(p,Step::Gpu)==bit(4));assert(contains(p,Step::Command)==bit(12));assert(contains(p,Step::Brightness)==bit(13));
        if(bit(13))assert(p.back()==Step::Brightness);
    }
    for(unsigned m=0;m<64;++m){bool pause=m&1,cal=m&2,game=m&4,target=m&8,busy=m&16,same=m&32;
        const auto a=sourceAction(pause,cal,game,target,busy,same);
        assert(a==(pause?SourceAction::Paused:cal?SourceAction::CalibrationDeferred:game?SourceAction::GameDeferred:!target?SourceAction::NoTarget:busy?(same?SourceAction::Coalesce:SourceAction::Queue):SourceAction::Apply));}
    using namespace operations;
    Registry registry;
    auto read=registry.tryBegin("gpu-read",classify("nvcurve-root-helper","read_gpu_curve"));assert(read);
    auto read2=registry.tryBegin("gpu-read2",classify("nvcurve-root-helper","read_sensors"));assert(read2);
    assert(!registry.tryBegin("gpu-write",classify("nvcurve-root-helper","apply_named_profile")));
    // Multi-resource rejection must not reserve the other claim.
    auto profile=registry.tryBegin("gpu-profile",classify("nvcurve-root-helper","write_nvcurve_profile"));assert(profile);
    auto tune=registry.tryBegin("tune-helper",classify("tune-helper","apply"));assert(tune);
    assert(!registry.tryBegin("tune-profile-helper",classify("tune-profile-helper","apply_preset")));
    assert(registry.history().back().outcome==operations::Outcome::Conflict);
    assert(registry.transition(*tune,Phase::TimedOut));
    assert(!registry.tryBegin("tune-profile-helper",classify("tune-profile-helper","apply_preset")));
    auto cleanup=registry.tryBegin("cleanup",classify("tune-profile-helper","release"));assert(cleanup);
    assert(registry.finish(*cleanup,operations::Outcome::ReportedOk));
    assert(!registry.tryBegin("tune-profile-helper",classify("tune-profile-helper","apply_preset")));
    assert(registry.finish(*tune,operations::Outcome::ReportedOk));
    assert(registry.history().back().outcome==operations::Outcome::Unknown);
    auto allowed=registry.tryBegin("tune-profile-helper",classify("tune-profile-helper","apply_preset"));assert(allowed);
    auto cpu=registry.tryBegin("cpu-write",classify("ryzen-co-helper","set_coall"));assert(cpu);
    assert(registry.transition(*cpu,Phase::OwnerGone));
    assert(!registry.tryBegin("cpu2",classify("ryzen-co-helper","set_coper_batch")));
    auto reset=registry.tryBegin("cpu-reset",classify("ryzen-co-helper","reset"));assert(reset);
    assert(registry.finish(*cpu,operations::Outcome::ReportedFailure));
    assert(!registry.tryBegin("cpu2",classify("ryzen-co-helper","set_coper_batch")));
    assert(registry.finish(*reset,operations::Outcome::ReportedOk));
    assert(registry.tryBegin("cpu2",classify("ryzen-co-helper","set_coper_batch")));
    assert(registry.tryBegin("health-reader",classify("health-reader","boots")));
    std::cout<<"PASS: CO evidence, 49152 scene plans, 64 trigger scenarios and scoped operation reservations\n";
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d)/'test.cpp'; p.write_text(code); exe=Path(d)/'test'
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++20','-Wall','-Wextra','-Werror',*os.environ.get('CENTURION_TEST_CXXFLAGS','-O2').split(),'-I',str(root/'src'),str(p),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
