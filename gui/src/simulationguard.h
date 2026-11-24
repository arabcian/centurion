#pragma once
// Linux-only guard for the opt-in real-shell fixture launcher. The mount
// namespace is the primary filesystem boundary; this filter is installed
// before QApplication (and therefore before Qt starts threads). It is NOT
// a replacement for the namespace, nor a general-purpose security sandbox.
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/magic.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace simulationguard {
inline std::atomic<bool> active{false};
inline std::atomic<unsigned> blockedRoot{0}, blockedUser{0};
inline bool readOnlyFixture(const char *path) {
    struct stat st{}; struct statfs fs{}; struct statvfs v{};
    return ::lstat(path, &st) == 0 && S_ISDIR(st.st_mode) &&
        ::statfs(path, &fs) == 0 && fs.f_type != PROC_SUPER_MAGIC && fs.f_type != SYSFS_MAGIC &&
        ::statvfs(path, &v) == 0 && (v.f_flag & ST_RDONLY);
}
inline bool isolatedFilesystem() {
    struct statfs root{}; struct stat marker{}; struct statvfs rootMount{};
    return ::statfs("/", &root) == 0 && root.f_type == TMPFS_MAGIC &&
        ::statvfs("/", &rootMount) == 0 && (rootMount.f_flag & ST_RDONLY) &&
        ::lstat("/centurion-shell-fixture-v1", &marker) == 0 && S_ISREG(marker.st_mode) &&
        readOnlyFixture("/proc") && readOnlyFixture("/sys") &&
        ::access("/dev/nvidiactl", F_OK) != 0 && ::access("/dev/dri", F_OK) != 0 &&
        ::access("/dev/mem", F_OK) != 0 && ::access("/dev/bus/usb", F_OK) != 0;
}
inline bool installFilter() {
#if defined(__x86_64__) && !defined(__ILP32__)
    // Block the x32 ABI as well: its syscall numbers have an extra high bit.
    // The process may create threads and internal socketpairs, but cannot
    // execute another program or open/connect external sockets.
#define CENTURION_DENY(n) BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_##n, 0, 1), BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM)
    struct sock_filter code[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000U, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        CENTURION_DENY(execve), CENTURION_DENY(execveat),
        CENTURION_DENY(socket), CENTURION_DENY(connect),
        CENTURION_DENY(mount), CENTURION_DENY(umount2), CENTURION_DENY(pivot_root),
        CENTURION_DENY(setns), CENTURION_DENY(unshare), CENTURION_DENY(ptrace),
        CENTURION_DENY(process_vm_writev), CENTURION_DENY(bpf), CENTURION_DENY(reboot),
        CENTURION_DENY(kexec_load),
#ifdef __NR_kexec_file_load
        CENTURION_DENY(kexec_file_load),
#endif
        CENTURION_DENY(init_module), CENTURION_DENY(finit_module), CENTURION_DENY(delete_module),
#ifdef __NR_io_uring_setup
        CENTURION_DENY(io_uring_setup), CENTURION_DENY(io_uring_enter), CENTURION_DENY(io_uring_register),
#endif
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
#undef CENTURION_DENY
    struct sock_fprog program{static_cast<unsigned short>(sizeof(code)/sizeof(code[0])), code};
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return false;
    return ::syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &program) == 0;
#else
    return false; // Unsupported ABI: never silently weaken isolation.
#endif
}
inline bool enter() {
    if (::geteuid()==0 || !isolatedFilesystem() || !installFilter()) return false;
    active.store(true);
    return true;
}
} // namespace simulationguard
