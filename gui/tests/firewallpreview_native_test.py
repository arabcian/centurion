#!/usr/bin/env python3
"""Native saved-chain matcher; no nftables access or helper calls."""
from pathlib import Path
import subprocess, tempfile, os
root=Path(__file__).resolve().parents[1]
code=r'''
#include "firewallpreview.h"
#include <cassert>
#include <random>
#include <cstdio>
using namespace firewallpreview;
int main(){
 assert(ports("80, 81-83, 82, 443")==ports("443,80-83"));
 assert(!ports("0")&&!ports("65536")&&!ports("90-80")&&!ports("1-2-3"));
 assert(remote("192.0.2.87/24,2001:db8::1") == remote("2001:0db8::1,192.0.2.0/24"));
 assert(remote("192.0.2.1-192.0.2.4")==remote("192.0.2.1,192.0.2.2/31,192.0.2.4"));
 assert(!remote("0.0.0.0/0")&&!remote("192.0.2.5-192.0.2.1")&&!remote("::1-192.0.2.1"));
 assert(!address("::ffff:192.0.2.1"));
 auto lan=*remote("lan");assert(matches(lan,*address("192.168.1.3"),true));assert(!matches(lan,*address("100.64.0.1"),false));assert(!matches(lan,*address("224.0.0.1"),true));assert(matches(lan,*address("224.0.0.1"),false));
 Config c{true,Verdict::Block,Verdict::Allow,{{1,true,true,Verdict::Allow,"tcp","wg*",*ports("80-90"),*remote("2001:db8::/32")},{2,true,true,Verdict::Reject,"any","",{},*remote("")}}};
 assert(evaluate(c,Packet{true,"tcp","wg0",Address{},85}).value==Verdict::Unknown);
 Packet p{true,"tcp","wg0",*address("2001:db8::5"),85};assert(evaluate(c,p).rule==1);p.iface="";assert(evaluate(c,p).value==Verdict::Unknown);p.iface="eth0";assert(evaluate(c,p).rule==2);p.iface="wg0";std::swap(c.rules[0],c.rules[1]);assert(evaluate(c,p).rule==2);c.rules[0].enabled=false;assert(evaluate(c,p).rule==1);p.destinationPort=91;assert(evaluate(c,p).value==Verdict::Block);p.inbound=false;assert(evaluate(c,p).value==Verdict::Allow);p.proto="udp";p.destinationPort=67;assert(evaluate(c,p).value==Verdict::Unknown);c.enabled=false;assert(evaluate(c,p).value==Verdict::Disabled);
 std::mt19937 rng(123);for(int i=0;i<30000;++i){unsigned a=1+rng()%65535,b=1+rng()%65535;if(a>b)std::swap(a,b);auto x=ports(std::to_string(a)+"-"+std::to_string(b));assert(x&&x->size()==1&&x->front().first==a&&x->front().second==b);std::string junk(rng()%80,' ');for(auto& ch:junk)ch=char(rng());(void)ports(junk);(void)remote(junk);}
 std::puts("PASS: ordered IPv4/IPv6 saved-chain fixtures + 30000 randomized parser cases");
}'''
with tempfile.TemporaryDirectory() as d:
    src=Path(d)/'test.cpp'; src.write_text(code)
    for flags in [[],['-O2'],['-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']]:
        exe=Path(d)/'test'; subprocess.run([os.environ.get('CXX','g++'),'-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(src),'-o',str(exe)],check=True); subprocess.run([str(exe)],check=True)
