#!/usr/bin/env python3
"""Exercise the real EDID field map, including every CTA offset/length and corrupt captures."""
from pathlib import Path
import subprocess, tempfile, os
root = Path(__file__).resolve().parents[1]
code = r'''
#include "edidfields.h"
#include <cassert>
#include <cstdio>
#include <random>
#include <cmath>
using namespace edidfields;
void fix(std::vector<uint8_t>& d,size_t b) { unsigned sum=0; for(size_t i=b*128;i<b*128+127;i++)sum+=d[i]; d[b*128+127]=uint8_t(-sum); }
void bounds(const Inspection& v,size_t n) { std::vector<int> seen(n); for(auto f:v.fields){assert(f.length && f.offset+f.length<=n); for(size_t j=f.offset;j<f.offset+f.length;j++)assert(++seen[j]==1);} }
int main(){
 std::vector<uint8_t>d(256); for(int i=1;i<7;i++)d[i]=255; d[126]=1; fix(d,0); d[128]=2; d[129]=3; d[130]=12; d[132]=0x67; fix(d,1);
 auto a=inspect(d); assert(trusted(a)); bounds(a,d.size()); assert(a.fields.size()>16); assert(!trusted(inspect(std::span(d).first(128))));
 d[200]^=1; assert(!trusted(inspect(d))); d[200]^=1;
 d[130]=0;fix(d,1);assert(!trusted(inspect(d)));d[130]=12;fix(d,1);
 auto trailing=d;trailing.push_back(0x77);auto partial=inspect(trailing);assert(!trusted(partial));assert(partial.fields.back().kind==Kind::Unknown && partial.fields.back().offset==256);bounds(partial,trailing.size());
 d[126]=0;fix(d,0); assert(!trusted(inspect(d))); d[126]=1;fix(d,0);
 for(int end=0;end<256;++end)for(int len=0;len<32;++len){d[130]=end;d[132]=uint8_t(0x60|len);fix(d,1);auto v=inspect(d);bounds(v,d.size()); if(end!=0&&(end<4||end>127||(end>4&&5+len>end))) assert(!trusted(v));}
 // A 2560x1600 progressive timing: 287.24 MHz / (2720*1760) = 60 Hz.
 std::vector<uint8_t> timing(256); for(int i=1;i<7;i++)timing[i]=255; timing[126]=1;
 auto dtd=[&](size_t o){timing[o]=0x34; timing[o+1]=0x70; timing[o+2]=0; timing[o+3]=160; timing[o+4]=0xa0;
    timing[o+5]=0x40; timing[o+6]=160; timing[o+7]=0x60; timing[o+17]=0x1a;};
 dtd(54); timing[128]=2; timing[129]=3; timing[130]=4; dtd(132); fix(timing,0);fix(timing,1);
 auto modes=progressiveTimings(timing); assert(modes.size()==2 && modes[0].offset==54 && modes[1].offset==132);
 assert(modes[0].hActive==2560 && modes[0].vActive==1600 && modes[0].hBlank==160 && modes[0].vBlank==160);
 assert(std::abs(modes[0].refreshHz()-60.0)<0.01);
 timing[254]=1;fix(timing,1);assert(!trusted(inspect(timing)));
 timing[254]=0;fix(timing,1);
 timing[54+17]|=0x80;fix(timing,0);assert(progressiveTimings(timing).size()==1);
 timing[132+3]=0;fix(timing,1);assert(progressiveTimings(timing).empty());
 dtd(132);fix(timing,1);assert(progressiveTimings(timing).size()==1);
 timing[255]^=1;assert(progressiveTimings(timing).empty());timing[255]^=1;
 assert(progressiveTimings(std::span(timing).first(255)).empty());
 timing[130]=3;fix(timing,1);assert(progressiveTimings(timing).empty());
 // Identical DisplayID dimensions with the two timing clock units.
 for (const auto typeVII : {false, true}) {
   std::vector<uint8_t> did(256);
   for (int i=1; i<7; ++i) did[i]=255;
   did[126]=1; fix(did,0);
   const size_t pos=136, sectionChecksum=156;
   did[128]=0x70; did[129]=typeVII?0x20:0x13; did[130]=23;
   did[133]=typeVII?0x22:0x03; did[135]=20;
   const unsigned pclk=typeVII?148501:14850, encoded=pclk-1;
   did[pos]=uint8_t(encoded); did[pos+1]=uint8_t(encoded>>8); did[pos+2]=uint8_t(encoded>>16);
   auto word=[&](size_t off,unsigned n){did[pos+off]=uint8_t(n); did[pos+off+1]=uint8_t(n>>8);};
   word(4,1919); word(6,279); word(8,87); word(10,43);
   word(12,1079); word(14,44); word(16,3); word(18,4);
   auto seal=[&]{unsigned sum=0; for(size_t i=129;i<sectionChecksum;++i)sum+=did[i]; did[sectionChecksum]=uint8_t(-sum); fix(did,1);};
   seal();
   auto decoded=detailedTimings(did);
   assert(decoded.size()==1 && decoded[0].offset==pos && decoded[0].length==20);
   assert(decoded[0].source==(typeVII?TimingSource::DisplayIdTypeVII:TimingSource::DisplayIdTypeI));
   assert(decoded[0].pixelClockKHz==(typeVII?pclk:10*pclk));
   assert(decoded[0].hActive==1920 && decoded[0].vActive==1080 && std::abs(decoded[0].refreshHz()-60)<0.01);
   did[sectionChecksum]^=1; fix(did,1); assert(detailedTimings(did).empty());
   word(8,290); seal(); assert(detailedTimings(did).empty());
   word(8,87); did[135]=19; seal(); assert(detailedTimings(did).empty());
 }
 std::mt19937 rng(123);for(int i=0;i<30000;i++){std::vector<uint8_t> b(rng()%4200);for(auto& x:b)x=uint8_t(rng());auto v=inspect(b);bounds(v,b.size()); if(b.size()<128)assert(!trusted(v));}
 std::puts("PASS: 8192 CTA boundary combinations + 30000 random captures + progressive timing/invalid/interlaced fixtures");
}'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/'test.cpp'; src.write_text(code)
    for flags in [[],['-O2'],['-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']]:
        exe=Path(tmp)/'test'
        subprocess.run([os.environ.get('CXX','g++'),'-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(src),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
