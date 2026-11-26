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
std::vector<uint8_t> displayId(uint8_t version, uint8_t tag, uint8_t revision, std::vector<uint8_t> payload) {
    std::vector<uint8_t> d(256);
    std::fill_n(d.begin()+1, 6, 255);
    d[126] = 1;
    d[128] = 0x70; d[129] = version; d[130] = uint8_t(3 + payload.size());
    d[133] = tag; d[134] = revision; d[135] = uint8_t(payload.size());
    std::copy(payload.begin(), payload.end(), d.begin()+136);
    unsigned sum = 0;
    for (size_t i = 129; i < 136 + payload.size(); ++i) sum += d[i];
    d[136+payload.size()] = uint8_t(-sum);
    fix(d,0); fix(d,1);
    return d;
}
void compactDisplayIdTimings() {
    const auto compact = std::vector<uint8_t>{0x01,0x3a,0,0x80,239,68,0xa4,0x37,4,44,0x34};
    auto modes = detailedTimings(displayId(0x13,0x04,0,compact));
    assert(modes.size()==1 && modes[0].source==TimingSource::DisplayIdTypeII && modes[0].length==11);
    assert(modes[0].hActive==1920 && modes[0].hBlank==280 && modes[0].vActive==1080 && modes[0].vBlank==45);
    assert(modes[0].preferred && std::abs(modes[0].refreshHz()-60)<0.01);
    auto bad = compact;
    bad[10] = 0xff; bad[9] = 10;
    auto mixed = compact;
    mixed.insert(mixed.end(), bad.begin(), bad.end());
    assert(advertisedTimings(displayId(0x13,0x04,0,mixed)).empty());

    // Type VI may mix descriptors with and without size fields.
    std::vector<uint8_t> typeVI{0x13,0x44,0x82,0x7f,7,0x37,4,0x17,87,1,43,44,3,4};
    auto withSize = typeVI;
    withSize[2] |= 0x40;
    withSize.insert(withSize.end(), {0x96,0x28,0x01});
    typeVI.insert(typeVI.end(), withSize.begin(), withSize.end());
    modes = detailedTimings(displayId(0x13,0x13,0,typeVI));
    assert(modes.size()==2 && modes[0].source==TimingSource::DisplayIdTypeVI && modes[0].length==14);
    assert(modes[1].offset==150 && modes[1].length==17);
    assert(modes[0].pixelClockKHz==148500 && modes[0].hActive==1920 && modes[0].vActive==1080);
    typeVI.pop_back();
    assert(advertisedTimings(displayId(0x13,0x13,0,typeVI)).empty());
}
void nominalDisplayIdTimings() {
    auto nominal = [](const auto &mode, TimingSource source, unsigned hz) {
        assert(mode.source==source && !mode.hasGeometry() && mode.hActive==1920 && mode.vActive==1080);
        assert(mode.nominalRefreshHz==hz && mode.refreshHz()==double(hz));
    };
    auto modes = advertisedTimings(displayId(0x13,0x05,0,{0x94,239,59}));
    assert(modes.size()==1 && modes[0].preferred);
    nominal(modes[0],TimingSource::DisplayIdTypeIII,60);
    assert(advertisedTimings(displayId(0x13,0x05,0,{0x98,239,59})).empty());
    modes = advertisedTimings(displayId(0x13,0x11,0,{0,0,0x7f,7,0x37,4,119}));
    assert(modes.size()==1);
    nominal(modes[0],TimingSource::DisplayIdTypeV,120);

    const std::vector<uint8_t> typeIX{2,0x7f,7,0x37,4,143};
    auto edid = displayId(0x20,0x24,0,typeIX);
    modes = advertisedTimings(edid);
    assert(modes.size()==1 && progressiveTimings(edid).empty() && detailedTimings(edid).empty());
    nominal(modes[0],TimingSource::DisplayIdTypeIX,144);
    assert(advertisedTimings(displayId(0x13,0x24,0,typeIX)).empty());
    edid[142] ^= 1; fix(edid,1);
    assert(advertisedTimings(edid).empty());

    // Type X eight-byte descriptor carries the extended refresh field.
    std::vector<uint8_t> typeX{3,0x7f,7,0x37,4,143,1,1};
    modes = advertisedTimings(displayId(0x20,0x2a,0x20,typeX));
    assert(modes.size()==1 && modes[0].length==8);
    nominal(modes[0],TimingSource::DisplayIdTypeX,400);
    assert(advertisedTimings(displayId(0x20,0x32,0x20,typeX)).empty());
    assert(advertisedTimings(displayId(0x20,0x2a,0x30,typeX)).empty());
    typeX[0] = 7;
    assert(advertisedTimings(displayId(0x20,0x2a,0x20,typeX)).empty());
}
int main(){
 compactDisplayIdTimings(); nominalDisplayIdTimings();
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
 std::puts("PASS: 8192 CTA boundary combinations + 30000 random captures + detailed, compact, nominal, invalid and interlaced timing fixtures");
}'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/'test.cpp'; src.write_text(code)
    for flags in [[],['-O2'],['-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']]:
        exe=Path(tmp)/'test'
        subprocess.run([os.environ.get('CXX','g++'),'-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(src),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
