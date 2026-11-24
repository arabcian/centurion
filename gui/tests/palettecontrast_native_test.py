#!/usr/bin/env python3
"""Exercise production palette correction without Qt, using all real presets."""
from pathlib import Path
import re, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
source = (root/'src/theme.cpp').read_text().split('const Palette PALETTES[] = {',1)[1].split('\n};',1)[0]
rows = re.findall(r'\{"([^"\n]+)",\s*"[^"\n]+",\s*\{([^}]+)\}\}',source)
assert len(rows)==57 and len({x[0] for x in rows})==57
palettes=[]
for name, raw in rows:
    colors=re.findall(r'#[0-9a-fA-F]{6}',raw)
    assert len(colors)==19,name
    palettes.append('Palette{{'+','.join('Rgb{'+','.join(str(int(c[i:i+2],16)) for i in (1,3,5))+'}' for c in colors)+'}}')
code=r'''
#include "palettecontrast.h"
#include <cassert>
#include <iostream>
#include <random>
using namespace palettecontrast;
int main() {
    const Palette presets[] = { PRESETS };
    auto verify=[](const Palette &before, const Palette &after) {
        std::array<double,6> surface{};
        for (int i=0;i<6;++i) surface[i]=luminance(before[i]);
        for(int i=0;i<19;++i) {
            if (std::find(foregrounds.begin(),foregrounds.end(),i)==foregrounds.end()) assert(before[i]==after[i]);
            else {
                assert(passes(i,after[i],surface));
                if (passes(i,before[i],surface)) assert(before[i]==after[i]);
            }
        }
        assert(repair(after)==after);
    };
    for(const auto &p:presets) { assert(repair(p)==p);verify(p,p); }
    Palette impossible{};
    impossible[0]={255,255,255}; impossible[1]={128,128,128};
    const auto copy=impossible;
    assert(!repair(impossible)); assert(impossible==copy);
    std::mt19937 rng(20261124);
    for(int t=0;t<400;++t) {
        Palette p{};
        for(auto &c:p) for(auto &v:c) v=static_cast<unsigned char>(rng()%256);
        for(int i=0;i<6;++i) for(auto &v:p[i]) v=static_cast<unsigned char>((t%2?220:0)+rng()%36);
        const auto before=p;
        const auto fixed=repair(p); assert(fixed && p==before);
        verify(p,*fixed); assert(repair(p)==fixed);
        std::array<double,6> surface{};for(int i=0;i<6;++i)surface[i]=luminance(p[i]);
        for(int token:foregrounds) {
            auto d=[&](const Rgb &c){int s=0;for(int k=0;k<3;++k){int q=int(c[k])-int(p[token][k]);s+=q*q;}return s;};
            const int actual=d((*fixed)[token]);
            for(int end:{0,255}) for(int step=0;step<=255;++step) {
                Rgb candidate{};
                for(int k=0;k<3;++k) candidate[k]=static_cast<unsigned char>((int(p[token][k])*(255-step)+end*step+127)/255);
                if(passes(token,candidate,surface)) assert(actual<=d(candidate));
            }
        }
    }
    std::cout<<"PASS: 57 real palettes; 400 custom palettes; impossible mixed surfaces; idempotence, minimum displacement and no input mutation\n";
}
'''.replace('PRESETS',',\n'.join(palettes))
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/'test.cpp'; src.write_text(code)
    for flags in [[],['-O2'],['-fsanitize=address,undefined','-fno-sanitize-recover=undefined','-fno-omit-frame-pointer','-no-pie']]:
        binary=Path(tmp)/'test'
        subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(src),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
