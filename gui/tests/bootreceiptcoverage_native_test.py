#!/usr/bin/env python3
"""Actual receipt coverage algorithm, no privileged operations."""
from pathlib import Path
import tempfile, subprocess
root=Path(__file__).resolve().parents[1]
code=r'''
#include "bootreceiptcoverage.h"
#include <cassert>
#include <random>
int main(){
 using namespace bootreceiptcoverage; using tunereceipt::Status;
 auto c=assess({"x","y"},{{"x",Status::Match}});assert(c.consistent&&c.matched==1&&c.requested==2&&c.missing==std::vector<std::string>{"y"});
 c=assess({"x","x"},{{"x",Status::Match}});assert(!c.consistent&&!c.matched);
 c=assess({"x"},{{"x",Status::Match},{"x",Status::Match}});assert(!c.consistent&&!c.matched);
 c=assess({"x"},{{"y",Status::Match}});assert(!c.consistent&&!c.matched);
 c=assess({},{{"x",Status::Match}});assert(!c.consistent&&!c.matched);
 c=assess({"x","y"},{{"x",Status::Unverified},{"y",Status::Skipped}});assert(c.consistent&&!c.matched&&c.missing.empty());
 std::mt19937 rng(33);
 for(int i=0;i<10000;++i){
  std::vector<std::string> expected;std::vector<Row> rows;std::size_t matches=0,missing=0;
  for(int j=0;j<32;++j){auto key=std::to_string(j);expected.push_back(key);if(rng()%4==0){++missing;continue;}
   auto status=static_cast<Status>(rng()%7);rows.push_back({key,status});if(status==Status::Match)++matches;}
  c=assess(expected,rows);assert(c.consistent&&c.matched==matches&&c.missing.size()==missing);
  rows.push_back({"unrequested",Status::Match});assert(!assess(expected,rows).consistent);
 }
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)/'test.cpp';p.write_text(code);exe=Path(tmp)/'test'
 for flags in [['-O2'],['-O2','-flto'],['-O1','-fsanitize=address,undefined','-fno-sanitize-recover=undefined','-fno-omit-frame-pointer','-no-pie']]:
  subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(p),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
print('PASS: coverage, missing/duplicate/unrequested keys and 10000 random fixtures in normal/LTO/sanitizer modes')
