#!/usr/bin/env python3
from pathlib import Path
import subprocess, tempfile, os
root=Path(__file__).resolve().parents[1]
code=r'''
#include "hardwarefields.h"
#include <cassert>
#include <cstdio>
#include <random>
using namespace hardwarefields;
int main(){
 assert(cpuList("3,0-2,9")==std::vector<unsigned>({0,1,2,3,9}));
 for(auto s:{"","1,","1-0","1,1","1-3,2","-1","1-65536","1--2"," 1","1x"})assert(!cpuList(s));
 assert(cpuList("0-65535")->size()==65536);
 std::vector<uint8_t>d(34);d[0]=17;d[1]=34;d[12]=0;d[13]=0x20;d[21]=0x80;d[22]=0x0c;
 assert(memoryDevice(d)->bytes==uint64_t(8192)*1048576);assert(memoryDevice(d)->speed==3200);
 d[12]=0xff;d[13]=0x7f;d[28]=0;d[29]=0x80;assert(memoryDevice(d)->bytes==uint64_t(32768)*1048576);
 d[13]=0xff;assert(!memoryDevice(d)->bytes);d[12]=d[13]=0;assert(memoryDevice(d)->empty);
 assert(!memoryDevice(std::span(d).first(13)));
 std::vector<uint8_t>a(36);a[0]='D';a[1]='S';a[2]='D';a[3]='T';a[4]=36;a[8]=2;assert(acpiHeader(a)->revision==2);a[4]=1;assert(!acpiHeader(a));
 assert(milli("2250mA","mA")==2.25);assert(!milli("2250mV","mA"));assert(!milli("-1mA","mA"));assert(!milli("12mAextra","mA"));
 std::mt19937 rng(81);for(int i=0;i<30000;i++){std::vector<uint8_t>b(rng()%100);for(auto&x:b)x=uint8_t(rng());(void)memoryDevice(b);(void)acpiHeader(b);}
 std::puts("PASS: cache list, SMBIOS numeric fields, ACPI headers, strict PD units + 30000 random buffers");
}'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/'test.cpp';src.write_text(code)
    for flags in [[],['-O2'],['-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']]:
        exe=Path(tmp)/'test'
        subprocess.run([os.environ.get('CXX','g++'),'-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(src),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
