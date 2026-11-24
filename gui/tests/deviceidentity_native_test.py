#!/usr/bin/env python3
"""Run production directory-incarnation and runtime gate on a real fake sysfs tree."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
code=r'''
#include "deviceidentity.h"
#include <cassert>
#include <iostream>
#include <fstream>
using namespace deviceidentity;
namespace fs=std::filesystem;
int main(int argc,char **argv) {
    assert(argc==2);
    fs::path root=argv[1], devices=root/"devices", aliases=root/"aliases";
    fs::create_directories(devices/"bridge"/"endpoint");fs::create_directories(aliases);
    fs::create_directories(root/"devices-other");
    auto alias=aliases/"0000:01:00.0", node=devices/"bridge"/"endpoint";
    fs::create_directory_symlink(node,alias);
    auto before=capture(devices,alias);assert(before && before->path=="bridge/endpoint");
    std::ofstream(node/"vendor")<<"0x10de";assert(capture(devices,alias)==before);
    fs::rename(node,devices/"detached");assert(!capture(devices,alias));
    fs::create_directory(node);auto after=capture(devices,alias);assert(after && after!=before);
    fs::create_directory_symlink(node,aliases/"duplicate");assert(capture(devices,aliases/"duplicate")==after);
    fs::create_directory_symlink(root/"devices-other",aliases/"escape");assert(!capture(devices,aliases/"escape"));
    fs::create_directory_symlink(aliases/"cycle2",aliases/"cycle1");fs::create_directory_symlink(aliases/"cycle1",aliases/"cycle2");
    assert(!capture(devices,aliases/"cycle1"));assert(!capture(devices,devices));assert(!capture(devices,node/"absent"));
    assert(mayReadLink("active"));for(auto value:{"", "suspended", "suspending", "resuming", "unsupported", "Active", "active\n"}) assert(!mayReadLink(value));
    std::cout<<"PASS: sysfs incarnation, detached/replaced/duplicate aliases, boundary escape, cycles, runtime gate\n";
}
'''
with tempfile.TemporaryDirectory() as tmp:
    t=Path(tmp);src=t/'test.cpp';src.write_text(code)
    for i,flags in enumerate([[],['-O2'],['-fsanitize=address,undefined','-fno-sanitize-recover=undefined','-fno-omit-frame-pointer','-no-pie']]):
        binary=t/'test';subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(src),'-o',str(binary)],check=True)
        subprocess.run([str(binary),str(t/f'fixture{i}')],check=True)
