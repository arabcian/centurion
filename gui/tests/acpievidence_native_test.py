#!/usr/bin/env python3
"""Native lexical matching: exact instances, duplicate GUIDs, driver candidates, boot isolation."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]
code = r'''
#include "acpievidence.h"
#include <cassert>
#include <iostream>
#include <random>
using namespace acpievidence;
int main() {
 const std::string g="887B54E3-DDDC-4B2C-8B88-68A26A8835D0";
 const std::string h="DC2A8805-3A8C-41BA-A6F7-092E0089CD3B";
 assert(wmiGuid(g)==lower(g)); assert(wmiGuid(g+"-1")==lower(g));
 for(auto tail:{"-","-x",".1","-1x","-1-2","junk"}) assert(wmiGuid(g+tail).empty());
 assert(!uuid("887B54E3")); assert(!uuid(std::string(36,'a')));
 assert(sameBoot(g,lower(g))); assert(!sameBoot(g,g.substr(0,8))); assert(!sameBoot(g,h));
 Index index({{g,lower(g),"lenovo-wmi",true},{g+"-1",lower(g),"lenovo-wmi",true},
              {"PNP0C14:00",{},"acpi-driver",false},{h,lower(h),{},true}});
 auto m=index.match("wmi "+g+": failed"); assert(m.kind==Kind::Guid && m.count==2);
 m=index.match("wmi "+g+"-1: failed"); assert(m.kind==Kind::Instance && m.count==1 && m.objects[0]==1);
 m=index.match("ACPI: PNP0C14:00: status"); assert(m.kind==Kind::Instance && m.count==1 && m.objects[0]==2);
 m=index.match("ACPI: PNP0C14:00 "+h); assert(m.kind==Kind::Guid && m.count==2);
 m=index.match("wmi "+g+" "+g+"-1 "+g); assert(m.count==2);
 m=index.match("lenovo-wmi: event"); assert(m.kind==Kind::Driver && m.count==2);
 assert(index.match("other: lenovo-wmi: event").kind==Kind::None);
 assert(index.match("ACPI: PNP0C14:001").kind==Kind::ClassOnly);
 assert(index.match("other X"+g).kind==Kind::None);
 assert(index.match("other "+g+"-123").kind==Kind::None);
 assert(index.match("other "+g+".abc").kind==Kind::None);
 assert(index.match("WMI: general warning").kind==Kind::ClassOnly);
 assert(index.match(std::string(8193,'x')).kind==Kind::None);
 assert(index.match(std::string(129,'x')+" lenovo-wmi").kind==Kind::None);
 std::vector<Object> many(40,{g,lower(g),"lenovo-wmi",true});
 m=Index(many).match(g); assert(m.count==40 && m.objects.size()==16);
 std::mt19937 rng(22);
 for(int i=0;i<30000;++i){
   std::string s(rng()%160,' '); for(auto &c:s)c=char(rng()%256);
   auto r=index.match(s); assert(r.count<=4 && r.objects.size()<=r.count);
   for(auto n:r.objects) assert(n<4);
   if(s.size()!=36)assert(!uuid(s));
 }
 // Real kernel prefixes: the WMI bus device name, KBUILD_MODNAME underscores, "ACPI: <driver>:" pr_fmt.
 {
  Index real({{"PNP0C14:00",{},{},false},{"PNP0C0A:00",{},"battery",false},{g,lower(g),"lenovo-wmi-gamezone",true}});
  m=real.match("wmi_bus wmi_bus-PNP0C14:00: [Firmware Bug]: WQ00 data block query control method not found");
  assert(m.kind==Kind::Instance && m.count==1 && m.objects[0]==0);
  m=real.match("lenovo_wmi_gamezone: failed to get profile"); assert(m.kind==Kind::Driver && m.objects[0]==2);
  m=real.match("ACPI: battery: Slot [BAT0] (battery present)"); assert(m.kind==Kind::Driver && m.objects[0]==1);
  assert(real.match("other: battery: x").kind==Kind::None);
  assert(real.match("wmi_bus-PNP0C14:01: x").kind==Kind::None);
 }
 std::cout<<"PASS: strict GUID, full boot identity, exact token links, duplicate/mixed mentions, bounded candidates, 30000 malformed/random fixtures\n";
}
'''
with tempfile.TemporaryDirectory() as td:
    src=Path(td)/'test.cpp'; src.write_text(code)
    for flags in [[], ['-O2'], ['-fsanitize=address,undefined','-fno-sanitize-recover=undefined','-fno-omit-frame-pointer','-no-pie']]:
        exe=Path(td)/'test'
        subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(src),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
