#!/usr/bin/env bash
# lpm-collect — gathers read-only hardware/firmware info from AMD and Intel laptops
# into lpm-<product>.tar.gz. Changes nothing on the system.
#
#   sudo ./lpm-collect.sh            # full dump (ACPI tables, dmidecode, MSRs and debugfs need root)
#   ./lpm-collect.sh                 # partial dump without root
#   sudo ./lpm-collect.sh -o /tmp    # output folder (default: current folder)
#
# Serial numbers, UUIDs and MAC addresses are masked before archiving.
set -u
export LC_ALL=C

OUTDIR=.
[[ ${1:-} == -o && -n ${2:-} ]] && OUTDIR=$2

rd() { timeout 2 cat "$1" 2>/dev/null; }
have() { command -v "$1" >/dev/null 2>&1; }
hdr() { printf '\n===== %s\n' "$*"; }
# run CMD... : prints the command, then its output; notes a missing tool instead of failing
run() {
    hdr "$*"
    if have "$1"; then timeout 20 "$@" 2>&1; else echo "($1 not installed)"; fi
}
# tree DIR [maxdepth] : every readable regular file under DIR as "path: value"
tree() {
    [[ -d $1 ]] || { hdr "$1"; echo "(missing)"; return; }
    hdr "$1"
    find -L "$1" -maxdepth "${2:-3}" -type f -readable 2>/dev/null | sort | while read -r f; do
        case $f in */uevent|*/modalias|*/power/*|*/subsystem/*|*/driver/*|*/firmware_node/*) continue ;; esac
        v=$(rd "$f" | head -c 2000 | tr '\0' ' ')
        printf '%s: %s\n' "$f" "${v//$'\n'/ | }"
    done
}
# msr CPU REG... : reads MSRs with rdmsr (msr module, root)
msr() {
    local c=$1; shift
    have rdmsr || { echo "(rdmsr not installed: msr-tools)"; return; }
    for r in "$@"; do printf 'cpu%s %s = %s\n' "$c" "$r" "$(rdmsr -p "$c" -0 "$r" 2>&1)"; done
}

ROOT=0; [[ $EUID -eq 0 ]] && ROOT=1
PRODUCT=$(rd /sys/class/dmi/id/product_name | tr -c 'A-Za-z0-9._\n-' '_' | tr -d '\n')
VERSION=$(rd /sys/class/dmi/id/product_version | tr -c 'A-Za-z0-9._\n-' '_' | tr -d '\n')
NAME="lpm-${VERSION:-${PRODUCT:-unknown}}"
[[ $VERSION == *[Ss]ystem* || -z $VERSION ]] && NAME="lpm-${PRODUCT:-unknown}"
WORK=$(mktemp -d) || exit 1
D="$WORK/$NAME"
mkdir -p "$D/acpi"
trap 'rm -rf "$WORK"' EXIT

VENDOR=$(awk -F': ' '/^vendor_id/{print $2; exit}' /proc/cpuinfo)
echo "lpm-collect: $NAME  ($VENDOR, root=$ROOT)"
(( ROOT )) || echo "  warning: not root — ACPI tables, dmidecode, MSR and debugfs sections will be empty"

# ── kernel ──────────────────────────────────────────────────────────────────
{
    hdr uname; uname -a
    hdr /proc/cmdline; rd /proc/cmdline
    hdr os-release; rd /etc/os-release
    hdr /proc/sys/kernel/tainted; rd /proc/sys/kernel/tainted
    hdr scheduler
    rd /sys/kernel/debug/sched/features 2>/dev/null
    ls /sys/kernel/sched_ext 2>/dev/null && tree /sys/kernel/sched_ext 1
    for k in /proc/sys/kernel/sched_*; do [[ -f $k ]] && echo "$k: $(rd "$k")"; done
    hdr "kernel config (selected)"
    { zcat /proc/config.gz 2>/dev/null || cat "/boot/config-$(uname -r)" 2>/dev/null; } |
        grep -E 'AMD|INTEL|X86_|PSTATE|CPU_FREQ|CPU_IDLE|HZ|PREEMPT|SCHED|LENOVO|IDEAPAD|WMI|THINKPAD|HWMON|RAPL|POWERCAP|NTSYNC' | grep -v '^#'
    run lsmod
    hdr "dmesg (firmware/cpu/acpi lines)"
    dmesg 2>/dev/null | grep -iE 'acpi|wmi|bios|firmware|microcode|pstate|cppc|rapl|mce|hwmon|lenovo|ideapad|legion|thermal|tcc|x3d|hsmp|nvidia|amdgpu|i915|xe ' | tail -400
} > "$D/kernel.txt"

# ── CPU (common + vendor-specific) ─────────────────────────────────────────
{
    run lscpu
    run lscpu -e
    hdr "/proc/cpuinfo (cpu0)"; awk '/^$/{exit} {print}' /proc/cpuinfo
    hdr microcode; grep -m1 microcode /proc/cpuinfo; tree /sys/devices/system/cpu/cpu0/microcode 1
    tree /sys/devices/system/cpu/cpufreq 2
    tree /sys/devices/system/cpu/cpuidle 1
    tree /sys/devices/system/cpu/cpu0/cpuidle 2
    hdr "per-cpu max freq / acpi_cppc"
    for c in /sys/devices/system/cpu/cpu[0-9]*; do
        printf '%s max=%s base=%s cppc_highest=%s pref_rank=%s\n' "${c##*/}" \
            "$(rd "$c/cpufreq/cpuinfo_max_freq")" "$(rd "$c/cpufreq/base_frequency")" \
            "$(rd "$c/acpi_cppc/highest_perf")" "$(rd "$c/cpufreq/amd_pstate_prefcore_ranking")"
    done
    tree /sys/devices/system/cpu/cpu0/acpi_cppc 1
    tree /sys/devices/system/cpu/smt 1
    tree /sys/devices/system/cpu/vulnerabilities 1
    tree /sys/class/powercap 3
    hdr "cpu topology / hybrid"
    for t in /sys/devices/cpu_core /sys/devices/cpu_atom; do [[ -d $t ]] && echo "$t cpus=$(rd "$t/cpus")"; done
    for c in /sys/devices/system/cpu/cpu[0-9]*; do
        echo "${c##*/} pkg=$(rd "$c/topology/physical_package_id") die=$(rd "$c/topology/die_id") core=$(rd "$c/topology/core_id") cluster=$(rd "$c/topology/cluster_id") l3=$(rd "$c/cache/index3/shared_cpu_list")"
    done
    (( ROOT )) && modprobe msr 2>/dev/null

    case $VENDOR in
    AuthenticAMD)
        hdr "AMD"
        tree /sys/devices/system/cpu/amd_pstate 1
        for p in /sys/bus/platform/drivers/amd_x3d_vcache /sys/bus/platform/drivers/amd_pmf \
                 /sys/bus/platform/drivers/amd_pmc /sys/bus/platform/drivers/amd_hsmp \
                 /sys/bus/platform/drivers/amd-pmc; do
            [[ -d $p ]] && { hdr "$p"; ls -l "$p"; for dev in "$p"/*:*/ "$p"/AMDI*/; do [[ -d $dev ]] && tree "$dev" 1; done; }
        done
        tree /sys/devices/platform/hsmp 2
        tree /sys/kernel/debug/amd_pmc 1
        tree /sys/kernel/ryzen_smu_drv 1
        run ryzenadj --info
        hdr "AMD MSRs (cpu0)"
        # HWCR, P-state current limit/status, P0-P2 defs, RAPL unit / core / package energy
        msr 0 0xc0010015 0xc0010061 0xc0010063 0xc0010064 0xc0010065 0xc0010066 0xc0010299 0xc001029a 0xc001029b
        ;;
    GenuineIntel)
        hdr "Intel"
        tree /sys/devices/system/cpu/intel_pstate 1
        tree /sys/devices/system/cpu/intel_uncore_frequency 2
        tree /sys/bus/pci/drivers/proc_thermal 1
        for d in /sys/bus/pci/devices/*; do
            [[ -d $d/tcc_offset_degree_celsius || -d $d/power_limits || -d $d/workload_request ]] && tree "$d" 2
        done
        tree /sys/class/thermal/cooling_device0 1
        hdr "Intel MSRs (cpu0)"
        # PLATFORM_INFO, TEMPERATURE_TARGET (TjMax/TCC), TURBO_RATIO_LIMIT, POWER_CTL, RAPL unit,
        # PKG_POWER_LIMIT, PKG_POWER_INFO, PP0 limit, PLATFORM_POWER_LIMIT, HWP caps/request, MISC_ENABLE, FIRMWARE lock
        msr 0 0xce 0x1a2 0x1ad 0x1fc 0x606 0x610 0x614 0x638 0x65c 0x771 0x774 0x1a0
        hdr "Intel undervolt mailbox (0x150) — not read: reading needs a write; use intel-undervolt read"
        run intel-undervolt read
        ;;
    *) hdr "unknown CPU vendor: $VENDOR" ;;
    esac
} > "$D/cpu.txt"

# ── platform / firmware ────────────────────────────────────────────────────
{
    hdr "/sys/bus/platform/drivers"; ls /sys/bus/platform/drivers
    hdr "bound platform devices"
    for d in /sys/bus/platform/devices/*; do
        printf '%s -> %s\n' "${d##*/}" "$(basename "$(readlink -f "$d/driver" 2>/dev/null)")"
    done
    for p in /sys/bus/platform/devices/VPC2004:* /sys/bus/platform/devices/PNP0C09:* \
             /sys/devices/platform/legion /sys/devices/platform/thinkpad_acpi \
             /sys/bus/platform/drivers/ideapad_acpi/*:* /sys/bus/platform/drivers/legion/*; do
        [[ -d $p ]] && tree "$p" 1
    done
    tree /sys/module/ideapad_laptop/parameters 1
    tree /sys/module/legion_laptop/parameters 1
} > "$D/platform-drivers.txt"

tree /sys/class/firmware-attributes 4 > "$D/firmware-attributes.txt"
{ tree /sys/firmware/acpi 1; tree /sys/class/platform-profile 2; } > "$D/platform-profile.txt"

{
    for h in /sys/class/hwmon/hwmon*; do
        hdr "$h ($(rd "$h/name")) -> $(readlink -f "$h/device" 2>/dev/null)"
        for f in "$h"/*; do [[ -f $f && -r $f ]] && printf '%s: %s\n' "${f##*/}" "$(rd "$f" | tr '\n' ' ')"; done
    done
    run sensors
    tree /sys/class/thermal 2
} > "$D/hwmon.txt"

{
    run lspci -nnk
    run lspci -vvv -nn
    run lsusb
    run lsusb -t
    hdr hidraw
    for h in /sys/class/hidraw/hidraw*; do echo "${h##*/}: $(grep -E 'HID_ID|HID_NAME|HID_PHYS' "$h/device/uevent" 2>/dev/null | tr '\n' ' ')"; done
} > "$D/lspci.txt"

{
    tree /sys/class/power_supply 2
    run upower -d
} > "$D/charge.txt"

tree /sys/class/dmi/id 1 > "$D/dmi.txt"

{
    tree /sys/bus/wmi/devices 2
    hdr "wmi drivers"; ls /sys/bus/wmi/drivers 2>/dev/null
} > "$D/wmi.txt"

# ── GPU / storage / misc (one file each, only extras) ──────────────────────
{
    for c in /sys/class/drm/card[0-9]; do
        dev=$(readlink -f "$c/device")
        hdr "$c -> $dev driver=$(basename "$(readlink -f "$dev/driver")") runtime=$(rd "$dev/power/runtime_status")"
        for f in power_dpm_force_performance_level pp_power_profile_mode pp_od_clk_voltage pp_dpm_sclk pp_dpm_mclk \
                 gt_min_freq_mhz gt_max_freq_mhz gt_RP0_freq_mhz gt_RPn_freq_mhz; do
            [[ -r $dev/$f || -r $c/$f ]] && printf '%s:\n%s\n' "$f" "$(rd "$dev/$f" || rd "$c/$f")"
        done
    done
    hdr "nvidia (nvidia-smi wakes the dGPU)"
    run nvidia-smi -q
    tree /proc/driver/nvidia 2
} > "$D/gpu.txt"

{
    run lsblk -o NAME,MODEL,SIZE,TRAN,ROTA,SCHED,FSTYPE,MOUNTPOINTS
    for n in /sys/class/nvme/nvme*; do tree "$n" 1; done
    run nvme list
    (( ROOT )) && for n in /dev/nvme[0-9]; do [[ -e $n ]] && run nvme id-ctrl "$n" -H; done
} > "$D/storage.txt"

# ── root-only raw firmware ─────────────────────────────────────────────────
if (( ROOT )); then
    if have acpidump; then
        (cd "$D/acpi" && acpidump -b >/dev/null 2>&1)
    else
        for t in /sys/firmware/acpi/tables/DSDT /sys/firmware/acpi/tables/SSDT*; do
            [[ -r $t ]] && cat "$t" > "$D/acpi/$(basename "$t" | tr 'A-Z' 'a-z').dat"
        done
    fi
    run dmidecode > "$D/dmidecode.txt"
    run efibootmgr -v >> "$D/platform-profile.txt"
else
    echo "(not root)" > "$D/dmidecode.txt"
fi

# ── mask identifiers ───────────────────────────────────────────────────────
find "$D" -name '*.txt' -exec sed -i -E \
    -e 's/((Serial Number|UUID|serial|product_uuid)[^:]*: ).*/\1<masked>/I' \
    -e 's/^(sn +: ).*/\1<masked>/' \
    -e 's/\b([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}\b/xx:xx:xx:xx:xx:xx/g' {} +
# (WMI GUIDs are left as they are: they name firmware interfaces, not this machine)

OUT="$OUTDIR/$NAME.tar.gz"
tar -C "$WORK" -czf "$OUT" "$NAME" && {
    [[ -n ${SUDO_USER:-} ]] && chown "$SUDO_USER": "$OUT"
    echo "done: $OUT ($(du -h "$OUT" | cut -f1))"
}
