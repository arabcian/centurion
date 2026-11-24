#!/usr/bin/env python3
"""Exercise canonical byte and time parsers, without Qt or hardware."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
code = r'''
#include "integerunits.h"
#include <cassert>
#include <iostream>
int main() {
    using integerunits::bytes;
    const auto max = std::numeric_limits<std::int64_t>::max();
    assert(bytes("9223372036854775807") == max);
    assert(!bytes("9223372036854775808"));
    assert(!bytes("9007199254740992 KiB"));
    assert(bytes("9007199254740991 KiB") == 9223372036854774784LL);
    assert(bytes("16 GiB") == 17179869184LL);
    assert(bytes("0 B") == 0);
    for (auto s : {"", " ", "-1", "1.5 GiB", "1 GB", "1e4", "1 KiB garbage", "1 K", "1 KB", "1 GiB MiB"}) assert(!bytes(s));
    for (std::int64_t n=0; n<100000; ++n) {
        assert(bytes(std::to_string(n)+" MiB") == n*1048576);
        assert(bytes(std::to_string(max-n)) == max-n);
    }
    using U = integerunits::TimeUnit;
    using integerunits::time;
    using integerunits::timeUnitForKey;
    assert(time("1.5 ms", U::Ns) == 1500000);
    assert(time("0.001 s", U::Us) == 1000);
    assert(time("1000 ns", U::Us) == 1);
    assert(time("1000000000 ns", U::Seconds) == 1);
    assert(time("1.0000000000000000000000 ms", U::Ms) == 1);
    assert(time("9223372036854775807 s", U::Seconds) == max);
    assert(time("9223372036854775807000 ns", U::Us) == max);
    assert(time("9223372036.854775807 s", U::Ns) == max);
    assert(time("-9223372036854775808", U::Ms) == std::numeric_limits<std::int64_t>::min());
    assert(time("-1", U::Us) == -1);
    assert(time("0.0000000000000000000 ns", U::Seconds) == 0);
    for (auto s : {"", "-1 ms", "0.1", "1.1 us", "1 ns", "1e3 us", "1,2 ms", "1. ms", ".1 ms", "1..0 ms", "1 ms us", "9223372036854775808", "9223372036854775808 us", "9223372036854775807 ms", "1 MS", "1 s\n", "1 KiB"}) assert(!time(s, U::Us));
    assert(!time("9223372036.854775808 s", U::Ns));
    assert(!time(std::string(129, '0'), U::Ns));
    assert(!time("1", U::None));
    assert(timeUnitForKey("cpu.wake_latency_ccd1") == U::Us);
    assert(timeUnitForKey("sched.base_slice_ns") == U::Ns);
    assert(timeUnitForKey("usb.autosuspend_ms") == U::Ms);
    assert(timeUnitForKey("usb.autosuspend") == U::Seconds);
    assert(timeUnitForKey("bt.conn_min_interval") == U::None);
    assert(timeUnitForKey("sched.llc_epoch_period") == U::None);
    assert(timeUnitForKey("vm.dirty_bytes") == U::None);
    assert(timeUnitForKey("unrecognized_ns") == U::None);
    const U units[] = {U::Ns, U::Us, U::Ms, U::Seconds};
    for (auto from : units) for (auto to : units) {
        const auto factor = [](int n) { std::int64_t f=1; while(n-->0) f*=10; return f; };
        const int diff = static_cast<int>(from)-static_cast<int>(to);
        const auto mult = factor(diff<0?-diff:diff);
        const auto suffix = std::string(integerunits::unitName(from));
        for(std::int64_t n=0;n<10000;++n) {
            auto v = time(std::to_string(n)+" "+suffix,to);
            if(diff>=0) assert(v==n*mult);
            else if(n%mult==0) assert(v==n/mult);
            else assert(!v);
            // Near-boundary values with exact canonical suffix, bare and scaled.
            assert(time(std::to_string(max-n),to)==max-n);
            assert(time(std::to_string(max-n)+" "+std::string(integerunits::unitName(to)),to)==max-n);
        }
    }
    std::cout << "PASS: exact time unit conversions, sentinels, overflow, 480000 checks and malformed inputs\n";
    std::cout << "PASS: exact signed int64 byte conversion, overflow and 200000 round-trip checks\n";
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp)/'test.cpp'; src.write_text(code)
    for flags in [[], ['-O2'], ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer', '-no-pie']]:
        binary = Path(tmp)/'test'
        subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', *flags, '-I', str(root/'src'), str(src), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
