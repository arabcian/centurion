#!/usr/bin/env python3
"""Test the actual seccomp filter in a child, not a live GUI or physical device."""
from pathlib import Path
import os, subprocess, tempfile, platform
if platform.machine() != 'x86_64':
    raise SystemExit('x86_64 fixture required')
root=Path(__file__).resolve().parents[1]
code=r'''
#include "simulationguard.h"
#include <cassert>
#include <sys/socket.h>
#include <fcntl.h>
#include <thread>
int main() {
    assert(!simulationguard::isolatedFilesystem());
    assert(!simulationguard::enter());
    assert(!simulationguard::active.load());
    assert(!simulationguard::readOnlyFixture("/proc"));
    assert(!simulationguard::readOnlyFixture("/sys"));
    assert(simulationguard::installFilter());
    auto check=[] {
        errno=0; assert(::socket(AF_INET,SOCK_STREAM,0)==-1 && errno==EPERM);
        errno=0; assert(::socket(AF_UNIX,SOCK_STREAM,0)==-1 && errno==EPERM);
        char *const argv[]={nullptr};
        errno=0; assert(::syscall(SYS_execve,"/nonexistent-centurion",argv,argv)==-1 && errno==EPERM);
        errno=0; assert(::syscall(SYS_execveat,AT_FDCWD,"/nonexistent-centurion",argv,argv,0)==-1 && errno==EPERM);
        int pair[2]; assert(::socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
        ::close(pair[0]); ::close(pair[1]);
    };
    check(); std::thread worker(check); worker.join();
    assert(::prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==1);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp)/'guard.cpp'; p.write_text(code); exe=Path(tmp)/'guard'
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++20','-Wall','-Wextra','-Werror','-pthread',*os.environ.get('CENTURION_TEST_CXXFLAGS','-O2').split(),'-I',str(root/'src'),str(p),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=10)
print('PASS: normal host refused; exec/socket denied on main and new thread, socketpair preserved')
