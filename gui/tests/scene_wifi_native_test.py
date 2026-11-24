#!/usr/bin/env python3
"""Native behavior tests for the exact scene reporting and scan-age arithmetic."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]
code = r'''
#include "sceneoutcome.h"
#include "wifiage.h"
#include <cassert>
#include <random>
#include <iostream>
int main() {
    using sceneoutcome::terminal;
    for (unsigned bits=0; bits<16; ++bits) {
        bool ok=bits&1, game=bits&2, incomplete=bits&4, deferred=bits&8;
        auto state=terminal(ok,game,incomplete,deferred);
        if (game) assert(state=="interrupted");
        else if (!ok||incomplete) assert(state=="partial");
        else if (deferred) assert(state=="deferred_pending");
        else assert(state=="returned_unverified");
    }
    using namespace wifiage;
    assert(!integer(NAN)); assert(!integer(INFINITY)); assert(!integer(-1)); assert(!integer(1.5));
    assert(!integer(9007199254740992.0)); assert(integer(0)==0);
    assert(!elapsed(20,10)); assert(!elapsed({},20)); assert(!age({},20));
    assert(age(300,60000)==60300); assert(!age(UINT64_MAX,1));
    assert(validSignal(-127)); assert(!validSignal(-128)); assert(!validSignal(NAN));
    assert(band(2412)==0); assert(band(5180)==1); assert(band(5955)==2); assert(band(5955.5)==-1);
    assert(band(5900)==-1); assert(band(NAN)==-1);
    std::mt19937_64 r(12);
    for (int i=0;i<100000;++i) {
        auto a=r(),b=r(); auto sum=static_cast<__uint128_t>(a)+b;
        auto got=age(a,b);
        assert(bool(got)==(sum<=UINT64_MAX)); if(got) assert(*got==sum);
        auto diff=elapsed(a,b); assert(bool(diff)==(b>=a)); if(diff) assert(*diff==b-a);
        const double x=double(r()%20000)-10000;
        assert(validSignal(x)==(x>=-127&&x<=0));
    }
    std::cout << "PASS: 16 scene outcome combinations + 100000 age/clock/signal cases\n";
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/'test.cpp'; src.write_text(code)
    for flags in [[],['-O2'],['-fsanitize=address,undefined','-fno-sanitize-recover=undefined','-fno-omit-frame-pointer','-no-pie']]:
        binary=Path(tmp)/'test'
        subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(src),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
