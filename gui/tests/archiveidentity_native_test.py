#!/usr/bin/env python3
"""Test metadata identity without opening or extracting archives."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
code=r'''
#include "archiveidentity.h"
#include <cassert>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
using archiveidentity::read;
int main(int argc,char**argv){
    assert(argc==2); std::string p=std::string(argv[1])+"/a", q=p+".new", link=p+".link";
    assert(!read(p.c_str())); std::ofstream(p)<<"one"; auto a=read(p.c_str()); assert(a && a==read(p.c_str()));
    struct stat s{}; assert(stat(p.c_str(),&s)==0);
    std::ofstream(q)<<"two"; timespec t[2]={s.st_atim,s.st_mtim}; assert(utimensat(AT_FDCWD,q.c_str(),t,0)==0);
    assert(rename(q.c_str(),p.c_str())==0); auto b=read(p.c_str()); assert(b && a!=b);
    assert(symlink(p.c_str(),link.c_str())==0); assert(!read(link.c_str())); assert(!read(argv[1]));
    assert(chmod(p.c_str(),0600)==0); auto c=read(p.c_str()); assert(c && c!=b);
    std::filesystem::remove(p); std::filesystem::remove(link); assert(!read(p.c_str()));
    std::cout<<"PASS: missing, symlink, directory, same-size replacement with preserved mtime, changed metadata\n";
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/'test.cpp';src.write_text(code)
    for flags in [[],['-O2'],['-fsanitize=address,undefined','-fno-sanitize-recover=undefined','-fno-omit-frame-pointer','-no-pie']]:
        binary=Path(tmp)/'test'
        subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(src),'-o',str(binary)],check=True)
        subprocess.run([str(binary),tmp],check=True)
