#!/usr/bin/env python3
"""Exercise the real canonical byte parser, without Qt or hardware."""
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
    std::cout << "PASS: exact signed int64 byte conversion, overflow and 200000 round-trip checks\n";
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp)/'test.cpp'; src.write_text(code)
    for flags in [[], ['-O2'], ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer', '-no-pie']]:
        binary = Path(tmp)/'test'
        subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', *flags, '-I', str(root/'src'), str(src), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
