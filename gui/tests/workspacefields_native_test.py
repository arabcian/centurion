#!/usr/bin/env python3
"""Real workspace validators and dashboard hysteresis, independent of Qt."""
from pathlib import Path
import tempfile, subprocess
root=Path(__file__).resolve().parents[1]
code=r'''
#include "workspacefields.h"
#include <cassert>
#include <climits>
#include <iostream>
#include <numeric>
#include <random>
using namespace workspacefields;
int main() {
    assert(permutation({2,0,1},3)); assert(!permutation({2,2,1},3)); assert(!permutation({0,1,3},3));
    assert(!permutation({},0)); assert(!permutation({},INT_MAX));
    assert(widths({100,200},2,20)); assert(!widths({19,200},2,20)); assert(!widths({100,4001},2,20));
    assert(split({100,100},2)); assert(!split({0,100},2)); assert(!split({INT_MAX,100},2));
    assert(panelColumns(612,300,2,2)==2); assert(panelColumns(612,300,1,2)==1);
    assert(panelColumns(636,300,1,2)==2); assert(panelColumns(611,300,2,2)==1);
    assert(panelColumns(INT_MAX,INT_MAX,2,2)==1); assert(panelColumns(INT_MAX,0,2,1)==1);
    std::mt19937 rng(1234);
    for(int i=0;i<100000;++i) {
        int n=2+int(rng()%127); std::vector<int> order(n); std::iota(order.begin(),order.end(),0);
        std::shuffle(order.begin(),order.end(),rng); assert(permutation(order,n));
        order[0]=order[1]; assert(!permutation(order,n));
        int r=int(rng()%2000), w=int(rng()%5000), prev=1+int(rng()%2);
        int expected=w>=2*r+12+(prev==1?24:0)?2:1;
        assert(panelColumns(w,r,prev,n)==expected);
    }
    std::cout<<"PASS: 100000 valid/duplicate permutations and dashboard thresholds plus bounds fixtures\n";
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/'test.cpp'; src.write_text(code)
    for flags in [[],['-O2'],['-fsanitize=address,undefined','-fno-sanitize-recover=undefined','-fno-omit-frame-pointer','-no-pie']]:
        binary=Path(tmp)/'test'
        subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(src),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
