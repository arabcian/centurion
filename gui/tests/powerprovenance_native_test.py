#!/usr/bin/env python3
"""Native, hardware-independent tests of presentation-only firmware and PMU evidence."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]
code = r'''
#include "powerprovenance.h"
#include <cassert>
#include <limits>
#include <random>
#include <string>
using namespace powerprovenance;
int main() {
    assert(integer(" -9223372036854775808\n") == std::numeric_limits<int64_t>::min());
    assert(integer("9223372036854775807") == std::numeric_limits<int64_t>::max());
    for(auto s:{"", "unknown", "1W", "1.2", "0xff", "9223372036854775808", "-9223372036854775809", "1 2"}) assert(!integer(s));
    assert(!range("0","0","0")); assert(!range("5","3","1")); assert(!range("0","8","-1"));
    const auto full=range("-9223372036854775808","9223372036854775807","1");
    assert(full && full->contains(std::numeric_limits<int64_t>::min()) && full->contains(std::numeric_limits<int64_t>::max()));
    const auto p=topologyfields::cpuList("0-3,8"), e=topologyfields::cpuList("4-7,8");
    assert(pmuKind(0,p,e)==PmuKind::Core); assert(pmuKind(4,p,e)==PmuKind::Atom);
    assert(pmuKind(8,p,e)==PmuKind::Conflict); assert(pmuKind(9,p,e)==PmuKind::Unknown);
    assert(pmuKind(0,p,std::nullopt)==PmuKind::Unknown);
    std::mt19937 gen(34);
    for(int i=0;i<30000;++i) {
        int64_t n=static_cast<int64_t>(gen())-static_cast<int64_t>(gen());
        assert(integer(std::to_string(n))==n);
        int64_t a=gen()%1000,b=gen()%1000,c=gen()%20;
        auto r=range(std::to_string(a),std::to_string(b),std::to_string(c));
        assert(bool(r)==(a<=b && c>0));
        if(r) assert(r->contains(a) && r->contains(b) && !r->contains(a-1) && !r->contains(b+1));
    }
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/'test.cpp';src.write_text(code)
    for flags in [[],['-O3','-flto'],['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-no-pie']]:
        exe=Path(tmp)/'test';subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(src),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
        print('PASS:', ' '.join(flags) or 'normal', '30000 fixtures')
