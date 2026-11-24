#!/usr/bin/env python3
"""Native tests of the production memory parsers, no host reads or Qt required."""
from pathlib import Path
import subprocess, tempfile, os
root=Path(__file__).resolve().parents[1]
code=r''' 
#include "memoryfields.h"
#include <cassert>
#include <string>
#include <random>
#include <cstdio>
using namespace memoryfields;
int main(){
 auto m=meminfo("MemTotal: 8192 kB\nMemAvailable: 1024 kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\n");
 assert(m.total==8388608 && m.available==1048576 && m.swapTotal==0 && m.swapFree==0);
 m=meminfo("MemTotal: 1 MB\nMemAvailable: 5 kB junk\nSwapFree: -1 kB\n");
 assert(!m.total&&!m.available&&!m.swapTotal&&!m.swapFree);
 m=meminfo("MemTotal: 1 kB\nMemTotal: 2 kB\nMemTotal: 3 kB\n");assert(!m.total);
 assert(!kib("18014398509481984 kB"));assert(kib("18014398509481983 kB")==18446744073709550592ULL);
 assert(!kib("1"));assert(!kib("1 kBfoo"));assert(!kib("1.0 kB"));
 auto z=zramStats("1000 250 300 0 0 0 0\n"); assert(z&&z->stored==1000&&z->compressed==250&&z->resident==300);
 z=zramStats("0 0 0");assert(z&&z->stored==0&&z->compressed==0&&z->resident==0);
 assert(!zramStats("100 0"));assert(!zramStats("100 bad 0"));assert(!zramStats("-1 0 0"));
 assert(zramDevice("/dev/zram0"));assert(zramDevice("/dev/zram123"));
 for(auto bad:{"zram0","/dev/zram","/dev/zram0p1","/dev/zram-1","/swap/zram0"})assert(!zramDevice(bad));
 const std::string header="Filename Type Size Used Priority\n";
 auto empty=swaps(header);assert(empty&&empty->empty());
 const auto active=header+"/dev/zram0 partition 8000 1 100\n/swap\\040file file 100 10 -2\n";
 auto rows=swaps(active);assert(rows&&rows->size()==2&&zramDevice(rows->at(0).path)&&rows->at(1).usedKiB==10);
 for(auto invalid:{"/dev/zram0 partition bad 0 100\n","/dev/zram0 partition 10 11 100\n","/dev/zram0 partition 10 0 bad\n","/dev/zram0 partition 10 0 100 junk\n","/dev/zram0 other 10 0 100\n","/dev/zram0 partition 10 0 100\n/dev/zram0 partition 10 0 100\n"})assert(!swaps(header+invalid));
 assert(!swaps(""));assert(!swaps("not header\n"));
 std::mt19937_64 rng(30);
 for(int i=0;i<50000;++i){std::string data(rng()%128,' ');for(char& c:data)c=char(rng()%256);(void)meminfo(data);(void)zramStats(data);(void)swaps(data);const auto n=rng()>>10;assert(kib(std::to_string(n)+" kB")==n*1024);}
 std::puts("PASS: strict memory, zram, swap evidence; 50000 malformed and roundtrip fixtures");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/'test.cpp';src.write_text(code)
    for flags in [[],['-O2'],['-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']]:
        exe=Path(tmp)/'test'
        subprocess.run([os.environ.get('CXX','g++'),'-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(src),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
