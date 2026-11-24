#!/usr/bin/env python3
"""Compile the actual interval/age code used by Channel View, no Qt required."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
code=r'''
#include "wifiage.h"
#include <cassert>
#include <random>
#include <iostream>
int main() {
 using namespace wifiage;
 auto ht=channelRange(2437,2437,20), wide=channelRange(2437,2447,40);
 auto a=channelRange(5180,5210,80), b=channelRange(5240,5250,160), c=channelRange(5260,5290,80);
 assert(ht && wide && a && b && c);
 assert(ht->low==2427 && ht->high==2447 && wide->low==2427 && wide->high==2467);
 assert(overlapMHz(*ht,*wide)==20); assert(overlapMHz(*a,*b)==80);
 assert(overlapMHz(*a,*c)==0); assert(overlapMHz(*ht,*a)==0);
 assert(!channelRange(5955,5985,80)); assert(!channelRange(5180,5210,320));
 assert(!channelRange(5180,5200,80)); assert(!channelRange(2437,2437,80));
 assert(!channelRange(2412,2402,40)); assert(!channelRange(5180,5210.5,80));
 assert(!channelRange(NAN,5210,80)); assert(!channelRange(5180,INFINITY,80));
 auto exact=ageFromSnapshot(5,1234,1400,1500,1800); assert(exact.observed && exact.ms==566);
 auto fallback=ageFromSnapshot(100,{},1400,1500,1800); assert(!fallback.observed && fallback.ms==500);
 assert(ageFromSnapshot(100,{}, {},1500,1800).ms==400);
 assert(!ageFromSnapshot({},1700,1400,1500,1800).ms);
 assert(!ageFromSnapshot(5,1234,1400,1500,1499).ms);
 assert(!ageFromSnapshot(UINT64_MAX,{},1400,1500,1800).ms);
 assert(!ageFromSnapshot({},1234,{}, {},1800).ms);
 assert(!ageFromSnapshot({},1234,1600,1500,1800).ms);
 assert(!ageFromSnapshot(10,1234,1600,1500,1800).ms);
 assert(!ageFromSnapshot(10,1450,1400,1500,1800).ms);
 std::mt19937 rng(19);
 for (int i=0;i<100000;++i) {
   const int f=5150+rng()%746, g=5150+rng()%746, w=20*(1+rng()%9);
   auto r=channelRange(f,g,w);
   if (r) { assert(r->high-r->low==w); assert(r->low<=f-10 && r->high>=f+10); assert(band(r->low)==1 && band(r->high)==1); assert(overlapMHz(*r,*r)==w); assert(overlapMHz(*r,*a)==overlapMHz(*a,*r)); }
 }
 std::cout << "PASS: timestamp fallback, range boundary and overlap cases + 100000 random fixtures\n";
}
'''
with tempfile.TemporaryDirectory() as td:
    src=Path(td)/'test.cpp';src.write_text(code)
    for flags in [[],['-O2'],['-fsanitize=address,undefined','-fno-sanitize-recover=undefined','-fno-omit-frame-pointer','-no-pie']]:
        exe=Path(td)/'test'
        subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(src),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
