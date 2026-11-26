#!/usr/bin/env python3
"""Centurion: read-only dump of /sys (debugfs included) and /proc/sys.

Run as root:  sudo python3 centurion-sysdump.py
Output:       ./centurion-sysdump-<host>.jsonl.gz   (send this file back)

Read-only: it never writes to any sysfs/debugfs/procfs file, never follows
symlinks, skips write-only and binary files, and refuses trees whose read has
side effects (GPU recover, hardware register reads, blocking pipes) or that
hold secrets/serials. Each read has a 2 s timeout. Typical run: under a minute.
"""
import gzip, json, os, platform, re, signal, stat, sys, time

ROOTS = ["/sys", "/proc/sys"]
MAX_VALUE = 4096
MAX_ENTRIES = 400000
BUDGET = 240  # seconds, whole run

# Path prefixes never entered (side effects, secrets, huge or binary).
DENY_PREFIXES = [
    "/sys/kernel/debug/dri", "/sys/kernel/debug/tracing", "/sys/kernel/debug/regmap",
    "/sys/kernel/debug/i2c", "/sys/kernel/debug/kcov", "/sys/kernel/debug/usb",
    "/sys/kernel/debug/gpio", "/sys/kernel/debug/ieee80211", "/sys/kernel/debug/pinctrl", "/sys/kernel/debug/mmc",
    "/sys/kernel/tracing", "/sys/kernel/security", "/sys/kernel/slab",
    "/sys/kernel/config", "/sys/kernel/btf", "/sys/kernel/notes",
    "/sys/kernel/vmcoreinfo", "/sys/fs/cgroup", "/sys/fs/bpf", "/sys/fs/pstore",
    "/sys/fs/selinux", "/sys/firmware/efi/efivars", "/sys/firmware/acpi/tables",
    "/sys/firmware/dmi", "/sys/class/drm", "/sys/devices/virtual/powercap/intel-rapl-mmio",
]
DENY_NAMES = re.compile(r"^(.*serial.*|uuid|address|hostname|boot_id|sections|notes|uevent|modalias|vpd|rom|config|"
                        r"trace_pipe.*|raw_trace|snapshot|kmsg|kcore|mem)$", re.I)
ADDR = re.compile(r"\b(?:0x)?ffff[0-9a-f]{12}\b", re.I)


class Timeout(Exception):
    pass


def _alarm(_s, _f):
    raise Timeout


def read_file(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        signal.setitimer(signal.ITIMER_REAL, 2)
        data = os.read(fd, MAX_VALUE + 1)
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        os.close(fd)
    return data


def main():
    if os.geteuid() != 0:
        print("Run as root (sudo), otherwise debugfs cannot be read.", file=sys.stderr)
        sys.exit(1)
    signal.signal(signal.SIGALRM, _alarm)
    host = platform.node() or "host"
    out = f"centurion-sysdump-{host}.jsonl.gz"
    start = time.time()
    n = 0
    skipped = {"binary": 0, "unreadable": 0, "denied": 0, "writeonly": 0}
    truncated = ""
    with gzip.open(out, "wt", encoding="utf-8") as f:
        meta = {"meta": {"kernel": platform.release(), "version": platform.version(),
                         "machine": platform.machine(), "date": time.strftime("%F %T"),
                         "debugfs_mounted": os.path.isdir("/sys/kernel/debug/sched"),
                         "cmdline": open("/proc/cmdline").read().strip()}}
        f.write(json.dumps(meta) + "\n")
        stack = list(reversed(ROOTS))
        while stack:
            d = stack.pop()
            if time.time() - start > BUDGET:
                truncated = "time budget"
                break
            try:
                it = list(os.scandir(d))
            except OSError:
                skipped["unreadable"] += 1
                continue
            for e in sorted(it, key=lambda x: x.name, reverse=True):
                p = e.path
                try:
                    if e.is_symlink():
                        continue
                    if any(p == q or p.startswith(q + "/") for q in DENY_PREFIXES) \
                            or DENY_NAMES.match(e.name):
                        skipped["denied"] += 1
                        continue
                    st = e.stat(follow_symlinks=False)
                    if stat.S_ISDIR(st.st_mode):
                        stack.append(p)
                        continue
                    if not stat.S_ISREG(st.st_mode):
                        continue
                    if not st.st_mode & 0o444:
                        skipped["writeonly"] += 1
                        continue
                    raw = read_file(p)
                    if b"\0" in raw:
                        skipped["binary"] += 1
                        continue
                    try:
                        v = raw[:MAX_VALUE].decode("utf-8")
                    except UnicodeDecodeError:
                        skipped["binary"] += 1
                        continue
                    v = ADDR.sub("<addr>", v.rstrip("\n"))
                    f.write(json.dumps({"p": p, "v": v, "w": bool(st.st_mode & 0o222)},
                                       ensure_ascii=False) + "\n")
                    n += 1
                    if n >= MAX_ENTRIES:
                        truncated = "entry limit"
                        stack.clear()
                        break
                except (OSError, Timeout):
                    skipped["unreadable"] += 1
        f.write(json.dumps({"end": {"entries": n, "skipped": skipped, "truncated": truncated,
                                    "seconds": round(time.time() - start, 1)}}) + "\n")
    print(f"{n} entries -> {out}  skipped={skipped} truncated={truncated or 'no'}")
    print("Send this file back.")


if __name__ == "__main__":
    main()
