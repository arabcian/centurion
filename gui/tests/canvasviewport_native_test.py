#!/usr/bin/env python3
"""View-only pan/zoom mapping, no Qt or hardware."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
code=r'''
#include "canvasviewport.h"
#include <cassert>
#include <iostream>
#include <random>
using namespace canvasviewport;
int main(){
 View view; assert(view.zoom()==1); view.pan({5,5}); assert(view.offset().x==0);
 const Point anchor{0.2,0.3}; view.zoomAt(2,anchor); auto p=view.map(anchor); assert(std::abs(p.x-anchor.x)<1e-12 && std::abs(p.y-anchor.y)<1e-12);
 std::mt19937_64 rng(23); std::uniform_real_distribution<double> d(0,1);
 for(int i=0;i<100000;++i){
   view.zoomAt(0.5+d(rng)*1.5,{d(rng),d(rng)}); view.pan({d(rng)-0.5,d(rng)-0.5});
   assert(view.zoom()>=1 && view.zoom()<=4); assert(std::abs(view.offset().x)<=(view.zoom()-1)/2); assert(std::abs(view.offset().y)<=(view.zoom()-1)/2);
   Point a{d(rng),d(rng)}; const auto b=view.inverse(view.map(a)); assert(std::abs(a.x-b.x)<1e-12 && std::abs(a.y-b.y)<1e-12);
 }
 view.zoomAt(NAN,anchor); view.pan({INFINITY,0}); assert(std::isfinite(view.zoom())); view.reset(); assert(view.zoom()==1 && view.offset().x==0 && view.offset().y==0);
 std::cout<<"PASS: 100000 zoom/pan/inverse hit-test mappings\n";
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/'test.cpp';src.write_text(code)
    for flags in [[],['-O2'],['-fsanitize=address,undefined','-fno-sanitize-recover=undefined','-fno-omit-frame-pointer','-no-pie']]:
        binary=Path(tmp)/'test'
        subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(src),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
