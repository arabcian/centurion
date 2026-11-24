#!/usr/bin/env python3
"""Real preview math shared by Zone4 and Spectrum; no device calls."""
from pathlib import Path
import tempfile, subprocess
root=Path(__file__).resolve().parents[1]
code=r'''
#include "lightingpreview.h"
#include <cassert>
#include <random>
#include <limits>
#include <cstdio>
int main(){
 using namespace lightingpreview;
 canvasviewport::View view;
 for(int i=0;i<4;++i)assert(zoneAt(view,{(i+.5)/4.,.5})==i);
 assert(!zoneAt(view,{1,.5})&&!zoneAt(view,{-.01,.5})&&!zoneAt(view,{.5,1})&&!zoneAt(view,{NAN,.5}));
 assert(animated(1)&&animated(6)&&!animated(7)&&!animated(11)&&!animated(13));
 auto fixed=frame(11,2,0,0,0,.25,3);auto fixedLater=frame(11,2,0,0,25,.25,3);assert(fixed.hue==fixedLater.hue && fixed.brightness==1);
 assert(frame(4,2,0,0,0,.25,1).brightness<frame(4,2,0,0,1,.25,1).brightness);
 assert(frame(2,2,4,0,0,.2,0).hue!=frame(2,2,3,0,0,.2,0).hue);
 std::mt19937_64 rng(32);std::uniform_real_distribution<double> d(0,1);
 for(int n=0;n<100000;++n){
   view.zoomAt(.5+d(rng)*1.5,{d(rng),d(rng)});view.pan({d(rng)-.5,d(rng)-.5});
   const canvasviewport::Point p{d(rng),d(rng)},m=view.map(p);
   if(m.x>=0&&m.x<1&&m.y>=0&&m.y<1)assert(zoneAt(view,m)==int(p.x*4));
   const auto count=std::size_t(rng()%33);const auto f=frame(int(rng()%16),int(rng()%6),int(rng()%6),int(rng()%3),d(rng)*1e9,d(rng),count);
   assert(f.hue>=0&&f.hue<=1&&f.brightness>=0&&f.brightness<=1&&(!count||f.color<count));
 }
 const auto f=frame(4,0,0,0,std::numeric_limits<double>::infinity(),NAN,0);assert(std::isfinite(f.hue)&&std::isfinite(f.brightness));
 std::puts("PASS: 100000 preview frames and zoom/pan inverse mappings, static/pulse/direction/nonfinite guards");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)/'test.cpp';p.write_text(code);exe=Path(tmp)/'test'
 for flags in [['-O2'],['-O2','-flto'],['-O1','-fsanitize=address,undefined','-fno-sanitize-recover=undefined','-fno-omit-frame-pointer','-no-pie']]:
  subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(p),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
