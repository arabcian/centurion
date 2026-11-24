#!/usr/bin/env python3
"""Test real dependency-free receipt and sysfs incarnation primitives."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
code = r'''
#include "curveplan.h"
#include "sensorincarnation.h"
#include <cassert>
#include <filesystem>
#include <fstream>
int main(int argc,char **argv) {
    assert(argc==2);
    using namespace curveplan;
    const Layout layout{{3,600000},{9,700000}};
    const std::vector<Point> points{{3,600000,1300000,-100000,1200000},{9,700000,1700000,-500000,1200000}};
    auto valid=validate(layout,points); assert(valid && valid->at(9).delta==-500000);
    auto bad=points; bad[1].projected++; assert(!validate(layout,bad));
    bad=points; bad[1].voltage++; assert(!validate(layout,bad));
    bad=points; bad[1].index=3; assert(!validate(layout,bad));
    bad=points; bad[1].base=INT64_MAX; assert(!validate(layout,bad));
    bad=points; bad[1].delta=INT64_MIN; assert(!validate(layout,bad));
    bad=points; bad.pop_back(); assert(!validate(layout,bad));
    bad=points; std::swap(bad[0],bad[1]); assert(!validate(layout,bad));
    assert(!validate({},{}));
    namespace fs=std::filesystem;
    auto root=fs::path(argv[1]); auto a=root/"chipA", b=root/"chipB", alias=root/"hwmon0";
    fs::create_directory(a); fs::create_directory(b); fs::create_directory_symlink(a,alias);
    auto id=sensorincarnation::identify(alias.c_str()); assert(id && sensorincarnation::same(alias.c_str(),id));
    fs::remove(alias); fs::create_directory_symlink(b,alias); assert(!sensorincarnation::same(alias.c_str(),id));
    fs::remove(alias); assert(!sensorincarnation::identify(alias.c_str()));
    fs::create_directory_symlink(a,alias); assert(sensorincarnation::same(alias.c_str(),id));
    fs::rename(a,root/"oldChipA"); fs::create_directory(a); assert(!sensorincarnation::same(alias.c_str(),id));
    fs::remove(alias); std::ofstream(alias)<<"not a directory"; assert(!sensorincarnation::identify(alias.c_str()));
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d)/'test.cpp'; p.write_text(code)
    flags=os.environ.get('CENTURION_TEST_CXXFLAGS','-O2').split()
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(p),'-o',str(Path(d)/'test')],check=True)
    subprocess.run([str(Path(d)/'test'),d],check=True)
print('PASS exact curve receipt plan and replaced sensor directory identity')
