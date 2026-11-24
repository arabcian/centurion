#!/usr/bin/env python3
"""Exercise the actual Open Ports address classifier without Qt or network access."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]
code = r'''
#include "socketbind.h"
#include <cassert>
#include <random>
#include <iostream>
int main() {
 using namespace socketbind;
 auto check=[](const std::string& a, int family, Scope scope, bool mapped=false) {
   auto b=classify(a); assert(b.family==family && b.scope==scope && b.mapped==mapped);
   assert(potentiallyNonlocal(b)==(scope!=Scope::Unknown && scope!=Scope::Loopback));
 };
 check("127.0.0.1",4,Scope::Loopback); check("127.255.1.2",4,Scope::Loopback);
 check("0.0.0.0",4,Scope::Wildcard); check("0.0.0.1",4,Scope::Specific);
 check("169.254.1.1",4,Scope::LinkLocal); check("10.0.0.0",4,Scope::Private);
 check("172.15.255.255",4,Scope::Specific); check("172.16.0.0",4,Scope::Private);
 check("172.31.255.255",4,Scope::Private); check("172.32.0.0",4,Scope::Specific);
 check("192.168.0.1",4,Scope::Private); check("100.63.255.255",4,Scope::Specific);
 check("100.64.0.0",4,Scope::Shared); check("100.127.255.255",4,Scope::Shared);
 check("100.128.0.0",4,Scope::Specific); check("224.0.0.1",4,Scope::Multicast);
 check("239.255.255.255",4,Scope::Multicast); check("240.0.0.0",4,Scope::Specific);
 check("::",6,Scope::Wildcard); check("0:0:0:0:0:0:0:1",6,Scope::Loopback);
 check("::ffff:127.0.0.1",6,Scope::Loopback,true); check("::ffff:0.0.0.0",6,Scope::Specific,true);
 check("::ffff:192.168.1.1",6,Scope::Private,true); check("fe80::1%eth0",6,Scope::LinkLocal);
 check("febf::1",6,Scope::LinkLocal); check("fec0::1",6,Scope::Specific);
 check("fc00::1",6,Scope::Private); check("fdff::1",6,Scope::Private);
 check("ff02::1",6,Scope::Multicast); check("2001:db8::1",6,Scope::Specific);
 for (const auto &bad: {"", "localhost", "127.0.0.1:80", "256.1.1.1", "1.2.3", "[::1]", "::%", "::%bad%zone", "127.0.0.1%eth0", " ::1"}) check(bad,0,Scope::Unknown);
 check(std::string("::1\0junk",8),0,Scope::Unknown); check(std::string(129,'1'),0,Scope::Unknown);
 std::mt19937 rng(21);
 for (int i=0;i<100000;++i) {
   std::array<unsigned char,4> bytes{}; for(auto &b:bytes) b=rng()%256;
   char out[INET_ADDRSTRLEN]{}; assert(inet_ntop(AF_INET,bytes.data(),out,sizeof(out)));
   const auto plain=classify(out), mapped=classify(std::string("::ffff:")+out);
   assert(plain.family==4 && mapped.family==6 && mapped.mapped);
   assert(mapped.scope==(plain.scope==Scope::Wildcard?Scope::Specific:plain.scope));
   std::string bad=out; bad.push_back('\0'); assert(classify(bad).scope==Scope::Unknown);
 }
 std::cout << "PASS: explicit scope boundaries, malformed data, IPv4-mapped IPv6, 100000 randomized roundtrips\n";
}
'''
with tempfile.TemporaryDirectory() as td:
    src=Path(td)/'test.cpp'; src.write_text(code)
    for flags in [[], ['-O2'], ['-fsanitize=address,undefined','-fno-sanitize-recover=undefined','-fno-omit-frame-pointer','-no-pie']]:
        exe=Path(td)/'test'
        subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(src),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
