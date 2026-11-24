#!/usr/bin/env python3
"""Pure C++ checks of the actual presentation protocols (no Qt or host devices)."""
from pathlib import Path
import tempfile,subprocess
root=Path(__file__).resolve().parents[1]
code=r'''
#include "diagnosticprotocols.h"
#include "acpinetlink.h"
#include <cassert>
#include <cstdio>
#include <random>
using namespace diagnosticprotocols;
int main(){
 auto fixed=pdo("fixed_supply","20000mV","","","2250mA","","");assert(fixed.watts==45);
 auto battery=pdo("battery","","9000mV","20000mV","","60000mW","");assert(battery.watts==60&&!battery.current);
 auto pps=pdo("programmable_supply","","3300mV","21000mV","3000mA","","0");assert(pps.watts==63);
 for(auto limit:{"1","","yes","unknown"})assert(!pdo("programmable_supply","","3300mV","21000mV","3000mA","",limit).watts);
 assert(!pdo("battery","","20000mV","9000mV","","60000mW","").watts);
 assert(!pdo("battery","","9000mV","20000mV","","60000mA","").watts);
 assert(!pdo("spr_adjustable_voltage_supply","20000mV","","","2250mA","","").watts);
 assert(!pdo("fixed_supply","20V","","","2250mA","","").watts);
 Cache a{0,"3","Unified","98304K",{0,1}},b{1,"3","Unified","98304K",{0,1}};
 std::map<unsigned,std::vector<Cache>> all{{0,{a}},{1,{b}}};assert(verifiedCache(a,all));
 all[1][0].size="32768K";assert(!verifiedCache(a,all));all[1][0]=b;all[1].push_back(b);assert(!verifiedCache(a,all));all[1].pop_back();
 all.erase(1);assert(!verifiedCache(a,all));all[1]={b};a.members={0,0,1};assert(!verifiedCache(a,all));
 a.members={0,1};a.cpu=3;assert(!verifiedCache(a,all));
 const std::string header="total used free shared buff/cache available";
 auto mem=freeBytes({header,"Mem: 18446744073709551615 10 20 30 40 50","Swap: 0 0 0"});assert(mem&&mem->at(0).bytes.at(0)==UINT64_MAX&&mem->at(1).bytes.size()==3);
 assert(!freeBytes({header,"Mem: 18446744073709551616 10 20 30 40 50","Swap: 0 0 0"}));
 assert(!freeBytes({header,"Mem: 100 -1 20 30 40 50","Swap: 0 0 0"}));
 assert(!freeBytes({header,"Mem: 100 10 20 30 40 101","Swap: 0 0 0"}));
 assert(!freeBytes({header,"Mem: 100 10 20 30 40 50","Swap: 0 1 0"}));
 assert(!freeBytes({"total used free","Mem: 100 10 20 30 40 50","Swap: 0 0 0"}));
 auto params=parameters({"NVreg_DynamicPowerManagement: mode: value (int)","NVreg_X: (charp)"});assert(params&&params->size()==2);
 assert(!parameters({"NVreg_X: (int)","NVreg_X: (int)"}));assert(!parameters({"$ injected: bad"}));assert(!parameters({"x: "}));
 assert(!parameters({"x: \x01"}));assert(!parameters({"x"}));
 // Construct a real aligned netlink/genl attribute buffer, no live socket.
 constexpr uint16_t family=42; constexpr std::size_t msgSize=NLMSG_HDRLEN+GENL_HDRLEN+NLA_HDRLEN+sizeof(acpievents::wire::Payload);
 std::vector<char> msg(msgSize);nlmsghdr nh{};nh.nlmsg_len=msgSize;nh.nlmsg_type=family;nh.nlmsg_seq=19;
 genlmsghdr gh{};gh.cmd=1;gh.version=1;nlattr at{};at.nla_type=1;at.nla_len=NLA_HDRLEN+sizeof(acpievents::wire::Payload);
 acpievents::wire::Payload payload{};std::strcpy(payload.deviceClass,"wmi");std::strcpy(payload.busId,"PNP0C14:00");payload.type=0xe8;payload.data=0;
 std::memcpy(msg.data(),&nh,sizeof(nh));std::memcpy(msg.data()+NLMSG_HDRLEN,&gh,sizeof(gh));std::memcpy(msg.data()+NLMSG_HDRLEN+GENL_HDRLEN,&at,sizeof(at));std::memcpy(msg.data()+NLMSG_HDRLEN+GENL_HDRLEN+NLA_HDRLEN,&payload,sizeof(payload));
 auto events=acpievents::parseChecked(msg.data(),msg.size(),family);assert(events&&events->size()==1&&events->front().sequence==19&&events->front().busId=="PNP0C14:00");
 for(std::size_t i=0;i<msg.size();++i)assert(!acpievents::parseChecked(msg.data(),i,family));
 auto broken=msg;broken.push_back(0);assert(!acpievents::parseChecked(broken.data(),broken.size(),family));
 broken=msg;broken[NLMSG_HDRLEN]=2;assert(!acpievents::parseChecked(broken.data(),broken.size(),family));
 broken=msg;broken[NLMSG_HDRLEN+GENL_HDRLEN]=0;assert(!acpievents::parseChecked(broken.data(),broken.size(),family));
 std::mt19937 rng(31);for(int i=0;i<30000;++i){std::string x(rng()%100,' ');for(auto&c:x)c=char(rng());(void)parameters({x});(void)freeBytes({header,x,x});(void)pdo(x,x,x,x,x,x,x);(void)acpievents::parseChecked(x.data(),x.size(),family);}
 std::puts("PASS: fixed/battery/PPS PDO, reciprocal cache declarations, byte-accurate memory and modinfo, ACPI wire validation, 30000 malformed inputs");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp)/'test.cpp';p.write_text(code);exe=Path(tmp)/'test'
    for flags in [['-O2'],['-O2','-flto'],['-O1','-fsanitize=address,undefined','-fno-sanitize-recover=undefined','-fno-omit-frame-pointer','-no-pie']]:
        subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(p),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
