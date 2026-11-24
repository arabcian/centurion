#!/usr/bin/env python3
from pathlib import Path
import subprocess, tempfile, os
root=Path(__file__).resolve().parents[1]
code=r'''
#include "structuredpci.h"
#include <cassert>
#include <random>
#include <cstdio>
using structuredpci::parse;
int main(){
 const std::string a="01:00.0 VGA compatible controller [0300]: NVIDIA Corporation Device [10DE:2C19] (rev a1)";
 const std::string b="0001:01:00.0 Non-Volatile memory controller [0108]: Vendor Device [1234:5678]";
 auto p=parse({a,"Subsystem: Vendor Device [17aa:1234]","Kernel driver in use: nvidia","Kernel modules: nouveau, nvidia",b});
 assert(p && p->size()==2 && p->at(0).vendor=="10de" && p->at(0).driver=="nvidia" && p->at(1).driver.empty());
 assert(!parse({a,"0000:"+a}));assert(!parse({a,a}));assert(!parse({a,"Kernel driver in use: a","Kernel driver in use: b"}));
 assert(!parse({a,"(command failed or output was truncated)"}));assert(!parse({"Kernel modules: a"}));assert(!parse({a,"Kernel modules: a;bad"}));
 assert(!parse({a,"Kernel modules: a","Kernel modules: b"}));assert(!parse({a,"Subsystem: not numeric"}));
 assert(!parse({a,std::string(1025,'a')}));assert(!parse(std::vector<std::string>(4097,a)));assert(!parse({a,std::string("a\0b",3)}));
 std::mt19937 rng(234);for(int i=0;i<12000;++i){std::string s(rng()%160,' ');for(char&c:s)c=char(rng()%256);(void)parse({a,s});}
 std::puts("PASS: PCI capture, duplicate, missing driver, errors, bounds, 12000 random rows");
}'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/'test.cpp';src.write_text(code)
    for flags in [[],['-O2'],['-O0','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']]:
        exe=Path(tmp)/'test'
        subprocess.run([os.environ.get('CXX','g++'),'-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(src),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
