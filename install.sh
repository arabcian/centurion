#!/bin/bash
# Centurion 2 — build and install (Rust helpers + Qt6 GUI).
#
#   sudo ./install.sh                  build + install to /usr
#   sudo ./install.sh --remove-legacy  also remove the old Python install
#   DESTDIR=/tmp/stage ./install.sh --no-build   stage only (packaging)
#
# Build optimizations (local build → tuned for this machine by default):
#   --no-native   portable binaries (no -march=native / -C target-cpu=native)
#   --no-lto      disable link-time optimization for the GUI (Rust always uses fat LTO)
#   --no-pgo      skip the profile-guided GUI build (default: instrumented build,
#                 offscreen training
#                 run over every tab, then the final build with the profile)
#   --no-harden   drop stack protector / FORTIFY=3 / CET / full RELRO / PIE on the GUI
#   --security-level=N   1 (default): wheel+local+active session is silent for the helpers;
#                 2: every helper asks for the admin password (polkit keeps it for its cache
#                 window); firmware-persistent writes always ask;
#                 3: as 2, except applying an approved Optimizations preset by name (tune-profile-helper). Asked interactively when
#                 not given (CENTURION_SECURITY_LEVEL works too); DESTDIR/non-tty installs use 1.
#   --clang       build the GUI with clang++ (+ lld when present) and link the Rust
#                 helpers with clang; same as ./install-clang.sh
#
# Layout:
#   /usr/bin/centurion                   GUI (Qt6)
#   /usr/bin/nvcurve                                nvcurve CLI (Rust)
#   /usr/bin/centurion-gamemode                           Lutris/Steam game-mode hook (runs as the user)
#   /usr/bin/centurion-netguard                           network guard daemon (IP blacklist, Wine/.exe guard, connection log)
#   /usr/libexec/centurion/*-helper      pkexec targets (root:root)
#   $PREFIX/lib/udev/rules.d/70-centurion-lighting.rules   keyboard lighting (uaccess)
#   /usr/share/polkit-1/actions/com.centurion.policy
#   /etc/polkit-1/rules.d/49-centurion.rules
#   /etc/init.d/nvcurve-autoload                    OpenRC boot-time GPU profile
#   /etc/init.d/centurion-tune                            OpenRC boot-time tuning preset
#   /etc/init.d/centurion-calibrate-boot                  OpenRC: calibration scheduled for the next boot
#   $PREFIX/lib/systemd/system/{nvcurve-autoload,centurion-tune}.service   systemd equivalents
# Both init flavours are installed; only the running init uses its files.
set -euo pipefail
cd "$(dirname "$0")"

PREFIX=${PREFIX:-/usr}
DESTDIR=${DESTDIR:-}
LIBEXEC="$PREFIX/libexec/centurion"
UNITDIR=${UNITDIR:-$PREFIX/lib/systemd/system}
SECLEVEL=${CENTURION_SECURITY_LEVEL:-}
BUILD=1 LEGACY=0 NATIVE=1 LTO=1 PGO=1 HARDEN=1 CLANG=0
for a in "$@"; do
    case "$a" in
        --no-build) BUILD=0 ;;
        --remove-legacy) LEGACY=1 ;;
        --no-native) NATIVE=0 ;;
        --no-lto) LTO=0 ;;
        --pgo) PGO=1 ;;
        --no-pgo) PGO=0 ;;
        --no-harden) HARDEN=0 ;;
        --clang) CLANG=1 ;;
        --security-level=*) SECLEVEL=${a#*=} ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done
[[ -n "$DESTDIR" || $EUID -eq 0 ]] || { echo "run as root (or set DESTDIR)" >&2; exit 1; }

if [[ -z $SECLEVEL ]]; then
    SECLEVEL=1
    if [[ -z $DESTDIR && -t 0 ]]; then
        echo "Polkit security level for the root helpers:"
        echo "  1) password-free for an administrator (wheel) at the machine   [default]"
        echo "  2) every helper asks for the administrator password (polkit caches it briefly)"
        echo "  3) like 2, but applying an already approved Optimizations preset (scenes, game hooks) stays password-free;"
        echo "     saving/approving presets, typed values and every other helper ask"
        ans=""; read -r -p "Level [1]: " ans || true
        [[ -n $ans ]] && SECLEVEL=$ans
    fi
fi
[[ $SECLEVEL == [123] ]] || { echo "invalid security level '$SECLEVEL' (use 1, 2 or 3)" >&2; exit 2; }

as_user() { if [[ $EUID -eq 0 && -n "${SUDO_USER:-}" ]]; then sudo -u "$SUDO_USER" "$@"; else "$@"; fi; }

onoff() { (( $1 )) && echo ON || echo OFF; }

if (( BUILD )); then
    # ── Toolchain ──
    cmake_cc=() LLD=0 PROFDATA=llvm-profdata
    if (( CLANG )); then
        # sudo's secure_path drops the user's PATH: on Gentoo clang lives only in
        # /usr/lib/llvm/<N>/bin, so look there too and use absolute paths.
        llvm_find() {  # llvm_find <tool>
            local p; p=$(command -v "$1" 2>/dev/null) && { echo "$p"; return; }
            p=$(compgen -G "/usr/lib/llvm/*/bin/$1" | sort -V | tail -1) && [[ -n $p ]] && { echo "$p"; return; }
            p=$(compgen -c "$1-" | grep -E "^$1-[0-9]+$" | sort -V | tail -1) && [[ -n $p ]] && command -v "$p"
        }
        CLANGXX=$(llvm_find clang++ || true); CLANGC=$(llvm_find clang || true)
        [[ -n $CLANGXX && -n $CLANGC ]] || { echo "!! --clang: clang/clang++ not found (PATH or /usr/lib/llvm/*/bin)" >&2; exit 1; }
        echo ">> clang: $CLANGXX"
        cmake_cc=(-DCMAKE_CXX_COMPILER="$CLANGXX")  # the GUI is CXX-only
        # lld handles clang's LTO objects without the LLVMgold plugin.
        if LLDBIN=$(llvm_find ld.lld) && [[ -n $LLDBIN ]]; then
            LLD=1; cmake_cc+=(-DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld --ld-path=$LLDBIN")
        fi
        PROFDATA=$(llvm_find llvm-profdata || true)
        if [[ -z $PROFDATA ]] && (( PGO )); then echo "!! llvm-profdata not found — building without PGO" >&2; PGO=0; fi
    fi
    # A build dir configured with the other compiler cannot be reused.
    for d in gui/build gui/build-pgo; do
        c=$(grep -s '^CMAKE_CXX_COMPILER:' "$d/CMakeCache.txt" | cut -d= -f2 || true)
        [[ -z $c ]] && continue
        if { (( CLANG )) && [[ $c != *clang* ]]; } || { (( !CLANG )) && [[ $c == *clang* ]]; }; then rm -rf "$d"; fi
    done

    # ── Rust: fat LTO + codegen-units=1 + panic=abort come from Cargo.toml ──
    rustflags=${RUSTFLAGS:-}
    (( NATIVE )) && rustflags+=" -C target-cpu=native"
    if (( CLANG )); then
        rustflags+=" -C linker=$CLANGC"
        (( LLD )) && rustflags+=" -C link-arg=-fuse-ld=lld -C link-arg=--ld-path=$LLDBIN"
    fi
    as_user env RUSTFLAGS="$rustflags" cargo build --release --locked

    # ── GUI ──
    gui_cmake() {  # gui_cmake <builddir> <pgo-mode>
        as_user cmake -S gui -B "$1" "${cmake_cc[@]}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
            -DCENTURION_HELPER_DIR="$LIBEXEC" -DCENTURION_LTO="$(onoff $LTO)" -DCENTURION_NATIVE="$(onoff $NATIVE)" \
            -DCENTURION_HARDEN="$(onoff $HARDEN)" -DCENTURION_PGO="$2" -DCENTURION_PGO_DIR="$PWD/gui/build-pgo/profile"
        as_user cmake --build "$1" -j"$(nproc)"
    }
    if (( PGO )); then
        rm -rf gui/build-pgo gui/build
        gui_cmake gui/build-pgo generate
        echo ">> PGO training run (offscreen, every tab)…"
        # Private runtime dir: the single-instance lock must not see a running GUI.
        # Private HOME/config too: the training run must not read (or write)
        # the user's real scenes, presets and guards. The GUI itself also keeps
        # every root helper and automatic scene off while CENTURION_PGO_TRAIN is set.
        rt=$(as_user mktemp -d)
        as_user mkdir -p "$rt/home" "$rt/run" && as_user chmod 700 "$rt/run"
        as_user env QT_QPA_PLATFORM=offscreen XDG_RUNTIME_DIR="$rt/run" HOME="$rt/home" \
            XDG_CONFIG_HOME="$rt/home/.config" XDG_CACHE_HOME="$rt/home/.cache" XDG_DATA_HOME="$rt/home/.local/share" \
            CENTURION_PGO_TRAIN=5 timeout 180 gui/build-pgo/centurion >/dev/null 2>&1 || true
        rm -rf "$rt"
        prof=gui/build-pgo/profile
        if compgen -G "$prof/*.profraw" >/dev/null; then    # clang
            as_user "$PROFDATA" merge -o "$prof/default.profdata" "$prof"/*.profraw
        fi
        if [[ -z $(find "$prof" -type f 2>/dev/null | head -1) ]]; then
            echo "!! training produced no profile — building without PGO" >&2
            gui_cmake gui/build ""
        else
            gui_cmake gui/build use
        fi
    else
        gui_cmake gui/build ""
    fi
fi

own=(-o root -g root); [[ $EUID -eq 0 ]] || own=()
T=target/release
install -d "${own[@]}" -m 0755 "$DESTDIR$LIBEXEC" "$DESTDIR$PREFIX/bin"
install "${own[@]}" -m 0755 "$T/legion-profile-helper" "$T/fwattr-helper" "$T/ryzen-co-helper" "$T/tune-helper" "$T/tune-profile-helper" "$T/intel-uv-helper" "$T/legion-gpu-helper" "$T/legion-firmware-helper" "$T/lighting-helper" "$T/amdgpu-helper" "$T/nvcurve-sensors" "$T/legion-ec-sensors" "$T/health-reader" "$T/centurion-boot-guard" "$T/netguard-helper" "$T/backup-helper" \
    "$DESTDIR$LIBEXEC/"
install "${own[@]}" -m 0700 "$T/nvcurve-root-helper" "$DESTDIR$LIBEXEC/"
install "${own[@]}" -m 0755 "$T/nvcurve" "$T/centurion-gamemode" "$T/centurion-intel-uv" "$T/centurion-autotune" "$T/centurion-calibrate" "$T/centurion-netguard" "$DESTDIR$PREFIX/bin/"
DESTDIR="$DESTDIR" cmake --install gui/build --strip

install -d "${own[@]}" -m 0755 "$DESTDIR$PREFIX/share/polkit-1/actions" "$DESTDIR/etc/polkit-1/rules.d" \
    "$DESTDIR/etc/init.d" "$DESTDIR/etc/nvcurve/profiles"
sed "s|@LIBEXEC@|$LIBEXEC|g" packaging/polkit/com.centurion.policy > "$DESTDIR$PREFIX/share/polkit-1/actions/com.centurion.policy"
[[ $EUID -eq 0 ]] && chown root:root "$DESTDIR$PREFIX/share/polkit-1/actions/com.centurion.policy"; chmod 0644 "$DESTDIR$PREFIX/share/polkit-1/actions/com.centurion.policy"
sed -e "s|@LIBEXEC@|$LIBEXEC|g" -e "s|@SECLEVEL@|$SECLEVEL|g" packaging/polkit/49-centurion.rules > "$DESTDIR/etc/polkit-1/rules.d/49-centurion.rules"
[[ $EUID -eq 0 ]] && chown root:root "$DESTDIR/etc/polkit-1/rules.d/49-centurion.rules"; chmod 0644 "$DESTDIR/etc/polkit-1/rules.d/49-centurion.rules"
# Keyboard lighting: uaccess on the Spectrum controller's hidraw node.
UDEVDIR=${UDEVDIR:-$PREFIX/lib/udev/rules.d}
install -d "${own[@]}" -m 0755 "$DESTDIR$UDEVDIR"
install "${own[@]}" -m 0644 packaging/udev/70-centurion-lighting.rules "$DESTDIR$UDEVDIR/"
if [[ -z "$DESTDIR" ]] && command -v udevadm >/dev/null; then
    udevadm control --reload 2>/dev/null || true
    udevadm trigger --subsystem-match=hidraw --action=change 2>/dev/null || true
fi
# Service files carry @BINDIR@/@LIBEXEC@ so a non-/usr PREFIX points at the right binaries.
subst() { sed -e "s|@BINDIR@|$PREFIX/bin|g" -e "s|@LIBEXEC@|$LIBEXEC|g" "$1"; }
for s in nvcurve-autoload centurion-tune centurion-intel-uv centurion-intel-uv-daemon centurion-boot-guard centurion-netguard centurion-calibrate-boot; do
    subst "packaging/openrc/$s" > "$DESTDIR/etc/init.d/$s"
    chmod 0755 "$DESTDIR/etc/init.d/$s"
done
install -d "${own[@]}" -m 0755 "$DESTDIR$UNITDIR"
for u in nvcurve-autoload centurion-tune centurion-intel-uv centurion-intel-uv-daemon centurion-boot-guard centurion-netguard centurion-calibrate-boot; do
    subst "packaging/systemd/$u.service" > "$DESTDIR$UNITDIR/$u.service"
    chmod 0644 "$DESTDIR$UNITDIR/$u.service"
done
[[ $EUID -eq 0 ]] && chown root:root "$DESTDIR"/etc/init.d/{nvcurve-autoload,centurion-tune,centurion-intel-uv,centurion-intel-uv-daemon,centurion-boot-guard,centurion-netguard,centurion-calibrate-boot} "$DESTDIR$UNITDIR"/{nvcurve-autoload,centurion-tune,centurion-intel-uv,centurion-intel-uv-daemon,centurion-boot-guard,centurion-netguard,centurion-calibrate-boot}.service
# elogind resume hook (re-applies the Intel undervolt boot profile; systemd uses the unit's sleep targets).
ELOGIND_SLEEP=${ELOGIND_SLEEP:-}
if [[ -z $ELOGIND_SLEEP ]]; then
    for d in /lib64/elogind /usr/lib64/elogind /lib/elogind /usr/lib/elogind; do
        [[ -d $d ]] && { ELOGIND_SLEEP=$d/system-sleep; break; }
    done
fi
if [[ -n $ELOGIND_SLEEP ]]; then
    install -d "${own[@]}" -m 0755 "$DESTDIR$ELOGIND_SLEEP"
    subst packaging/sleep/centurion-intel-uv > "$DESTDIR$ELOGIND_SLEEP/50-centurion-intel-uv"
    chmod 0755 "$DESTDIR$ELOGIND_SLEEP/50-centurion-intel-uv"
    [[ $EUID -eq 0 ]] && chown root:root "$DESTDIR$ELOGIND_SLEEP/50-centurion-intel-uv"
fi

if (( LEGACY )) && [[ -z "$DESTDIR" ]]; then
    # Old Python layout (and the step-1/2 drop-in binaries that replaced its .py helpers).
    rm -rf /usr/lib/legion-power-manager
    rm -f /usr/local/bin/nvcurve /etc/xdg/autostart/legion-power-manager-autostart.desktop
    rm -f /etc/polkit-1/localauthority/50-local.d/49-legion-power-manager.pkla
    echo "Removed the legacy Python install."
fi

# ── Migration: Legion Power Manager (lpm-*) → Centurion ──────────────────────
# Runs on every real install; does nothing once no old path is left.
OLD=legion-power-manager
OLD_SVCS=(lpm-tune lpm-intel-uv lpm-intel-uv-daemon lpm-boot-guard lpm-netguard lpm-calibrate-boot)
# Move $1 → $2. Directories merge (files already at the new place win), files only move if the target is absent.
mig_move() {
    local src=$1 dst=$2
    [[ -e $src || -L $src ]] || return 0
    if [[ -d $src && ! -L $src ]]; then
        if [[ -e $dst ]]; then
            { cp -a --update=none "$src/." "$dst/" 2>/dev/null || cp -a -n "$src/." "$dst/"; } && rm -rf "$src"
        else
            install -d "$(dirname "$dst")" && mv "$src" "$dst"
        fi
    elif [[ -e $dst ]]; then
        rm -f "$src"
    else
        mv "$src" "$dst"
    fi
    echo "  migrated $src → $dst"
}
migrate_legacy() {
    local found=0 p s rl u home user
    for p in /etc/$OLD /var/lib/$OLD /var/log/$OLD /var/cache/$OLD /usr/libexec/$OLD /etc/init.d/lpm-tune \
             /etc/modprobe.d/nvidia-lpm.conf /etc/modprobe.d/zz-$OLD-nvidia.conf "$UNITDIR/lpm-tune.service"; do
        [[ -e $p ]] && found=1
    done
    for home in /root /home/*; do [[ -d $home/.config/$OLD || -d $home/.local/state/$OLD ]] && found=1; done
    (( found )) || return 0
    echo "Migrating the Legion Power Manager install to Centurion…"

    # 1. Services: remember where each old one was enabled, stop it, enable the new one the same way.
    declare -A RL=() SD=()
    for s in "${OLD_SVCS[@]}"; do
        if [[ -d /run/systemd/system ]]; then
            systemctl is-enabled -q "$s.service" 2>/dev/null && SD[$s]=1
            systemctl disable --now "$s.service" 2>/dev/null || true
        elif command -v rc-update >/dev/null; then
            for rl in /etc/runlevels/*/; do
                [[ -e $rl$s ]] && RL[$s]+="$(basename "$rl") "
            done
            rc-service "$s" --ifstarted stop 2>/dev/null || true
            for rl in ${RL[$s]:-}; do rc-update del "$s" "$rl" >/dev/null 2>&1 || true; done
        fi
        rm -f "/etc/init.d/$s" "$UNITDIR/$s.service" "/etc/systemd/system/$s.service"
    done
    command -v nft >/dev/null && nft delete table inet lpm_netguard 2>/dev/null || true
    rmdir /sys/fs/cgroup/lpm-game 2>/dev/null || true

    # 2. System state, config, logs.
    mig_move /etc/$OLD /etc/centurion
    mig_move /var/lib/$OLD /var/lib/centurion
    mig_move /var/log/$OLD /var/log/centurion
    mig_move /var/cache/$OLD /var/cache/centurion
    mig_move /run/$OLD /run/centurion
    mig_move /etc/modprobe.d/nvidia-lpm.conf /etc/modprobe.d/nvidia-centurion.conf
    mig_move /etc/modprobe.d/zz-$OLD-nvidia.conf /etc/modprobe.d/zz-centurion-nvidia.conf
    mig_move /etc/NetworkManager/conf.d/90-$OLD-wifi-powersave.conf /etc/NetworkManager/conf.d/90-centurion-wifi-powersave.conf

    # 3. Old binaries, helpers and integration files.
    rm -rf "/usr/libexec/$OLD" "$PREFIX/libexec/$OLD" "/usr/share/doc/$OLD"
    rm -f "$PREFIX/bin/$OLD" "$PREFIX"/bin/lpm-{gamemode,intel-uv,autotune,calibrate,netguard}
    rm -f "$PREFIX/share/applications/$OLD.desktop" "/etc/xdg/autostart/$OLD.desktop" \
          "$PREFIX/share/icons/hicolor/scalable/apps/$OLD.svg"
    rm -f "$PREFIX/share/polkit-1/actions/com.$OLD.policy" "/etc/polkit-1/rules.d/49-$OLD.rules"
    rm -f "$UDEVDIR/70-$OLD-lighting.rules" /etc/udev/rules.d/70-$OLD-lighting.rules
    [[ -n ${ELOGIND_SLEEP:-} ]] && rm -f "$ELOGIND_SLEEP/50-lpm-intel-uv"

    # 4. Per-user data, plus game-launch hooks that call the old binary.
    for home in /root /home/*; do
        [[ -d $home ]] || continue
        user=$(stat -c %U "$home")
        mig_move "$home/.config/$OLD" "$home/.config/centurion"
        mig_move "$home/.cache/$OLD" "$home/.cache/centurion"
        mig_move "$home/.local/state/$OLD" "$home/.local/state/centurion"
        mig_move "$home/Backups/$OLD" "$home/Backups/centurion"
        mig_move "$home/.config/plasma-workspace/env/$OLD-igpu.sh" "$home/.config/plasma-workspace/env/centurion-igpu.sh"
        rm -f "$home/.config/autostart/$OLD.desktop" "$home/.config/autostart/$OLD-autostart.desktop"
        # Lutris (per-game + global system options) and Steam launch options.
        while IFS= read -r -d '' u; do
            grep -q 'lpm-gamemode' "$u" || continue
            cp -p "$u" "$u.pre-centurion" && sed -i 's/lpm-gamemode/centurion-gamemode/g' "$u" && echo "  updated hook in $u"
        done < <(find "$home/.config/lutris" "$home/.local/share/lutris" -name '*.yml' -print0 2>/dev/null)
        if pgrep -u "$user" -x steam >/dev/null 2>&1; then
            grep -rlq 'lpm-gamemode' "$home/.steam/steam/userdata" "$home/.local/share/Steam/userdata" 2>/dev/null &&
                echo "  !! Steam is running — close it and re-run the installer to update launch options (lpm-gamemode → centurion-gamemode)"
        else
            while IFS= read -r -d '' u; do
                grep -q 'lpm-gamemode' "$u" || continue
                cp -p "$u" "$u.pre-centurion" && sed -i 's/lpm-gamemode/centurion-gamemode/g' "$u" && echo "  updated launch options in $u"
            done < <(find -L "$home/.local/share/Steam/userdata" "$home/.steam/steam/userdata" -name localconfig.vdf -print0 2>/dev/null | sort -zu)
        fi
        chown -R "$user:" "$home/.config/centurion" "$home/.cache/centurion" "$home/.local/state/centurion" 2>/dev/null || true
    done

    # 5. Re-enable the new services exactly where the old ones were.
    for s in "${!SD[@]}"; do
        systemctl daemon-reload 2>/dev/null || true
        systemctl enable "centurion-${s#lpm-}.service" && echo "  enabled centurion-${s#lpm-}.service"
    done
    for s in "${!RL[@]}"; do
        for rl in ${RL[$s]}; do
            rc-update add "centurion-${s#lpm-}" "$rl" >/dev/null && echo "  rc-update add centurion-${s#lpm-} $rl"
        done
    done
    echo "Migration done. Restart (or re-login) so the services and the tray app run under the new names."
}
[[ -z "$DESTDIR" ]] && migrate_legacy

if [[ -z "$DESTDIR" ]]; then
    command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q "$PREFIX/share/icons/hicolor" || true
    echo
    echo "Installed. Optional boot services (each pulls in centurion-boot-guard, which pauses them"
    echo "after a boot that crashed right after applying them):"
    if [[ -d /run/systemd/system ]]; then
        systemctl daemon-reload || true
        echo "  systemctl enable centurion-boot-guard.service     # records clean shutdowns (login-scene guard)"
        echo "  systemctl enable nvcurve-autoload.service   # GPU V/F profile"
        echo "  systemctl enable centurion-tune.service           # Optimizations boot preset"
        echo "  systemctl enable --now centurion-netguard.service # network guard (Health → Network; needs nftables)"
        echo "  systemctl enable centurion-calibrate-boot.service # runs a calibration scheduled for the next boot (idle otherwise)"
        grep -q GenuineIntel /proc/cpuinfo && echo "  systemctl enable centurion-intel-uv.service       # Intel undervolt boot/resume profile (or centurion-intel-uv-daemon)"
    else
        echo "  rc-update add centurion-boot-guard default     # records clean shutdowns (login-scene guard)"
        echo "  rc-update add nvcurve-autoload default   # GPU V/F profile"
        echo "  rc-update add centurion-tune boot              # Optimizations boot preset"
        echo "  rc-update add centurion-netguard default       # network guard (Health → Network; needs nftables)"
        echo "  rc-update add centurion-calibrate-boot default # runs a calibration scheduled for the next boot (idle otherwise)"
        grep -q GenuineIntel /proc/cpuinfo && echo "  rc-update add centurion-intel-uv boot          # Intel undervolt boot profile (or centurion-intel-uv-daemon default)"
    fi
    # Runtime dependencies: [ok] found, [--] missing (only the tab/row that needs it is disabled).
    echo
    echo "Runtime dependencies:"
    dep() { printf '  [%s] %-34s %s\n' "$( ( eval "$2" ) >/dev/null 2>&1 && echo ok || echo -- )" "$1" "$3"; }
    # grep -c reads all of ldconfig's output: grep -q exits on the first match, ldconfig then dies of
    # SIGPIPE and pipefail reports a library that is present as missing.
    lib() { ldconfig -p | grep -c "$1" >/dev/null; }
    kmod() { [[ -d /sys/module/$1 ]] || modinfo "$1" >/dev/null 2>&1; }
    kcfg() { local c; c=$( { zcat /proc/config.gz 2>/dev/null || cat "/boot/config-$(uname -r)" 2>/dev/null; } ); for o in "$@"; do grep -Eq "^CONFIG_$o=(y|m)" <<<"$c" || return 1; done; }
    dep "polkit (pkexec)"                  "command -v pkexec"                         "required — every root helper"
    dep "platform_profile"                 "[[ -e /sys/firmware/acpi/platform_profile ]]" "required — power profiles"
    dep "Qt6 Widgets/Network/Svg"          "lib libQt6Widgets.so.6 && lib libQt6Svg.so.6" "required — GUI"
    dep "libxkbcommon"                     "lib libxkbcommon.so"     "keyboard layout in Lighting"
    dep "lenovo-wmi-gamezone"              "kmod lenovo_wmi_gamezone"                  "firmware limits, fans"
    dep "lenovo-wmi-other"                 "kmod lenovo_wmi_other"                     "firmware attributes"
    dep "ideapad_laptop"                   "kmod ideapad_laptop"                       "battery, device toggles"
    dep "acpi_call"                        "kmod acpi_call"                            "GPU limits, fan curve, GPU mode, EC temps"
    dep "NVIDIA proprietary driver (NVML)" "lib libnvidia-ml.so"     "NVIDIA tab"
    dep "amdgpu (ppfeaturemask 0x4000)"    "(( \$(cat /sys/module/amdgpu/parameters/ppfeaturemask 2>/dev/null || echo 0) & 0x4000 ))" "AMD GPU tab Overdrive"
    dep "ryzenadj (root-owned)"            "for p in /usr/bin /usr/sbin /usr/local/bin /usr/local/sbin /opt/ryzenadj; do [[ -x \$p/ryzenadj && \$(stat -c %u \$p/ryzenadj) == 0 ]] && exit 0; done; exit 1" "Ryzen tab"
    dep "ryzen_smu"                        "kmod ryzen_smu"                            "optional — live memory timings"
    dep "zenpower / zenergy"               "kmod zenpower || kmod zenergy"             "optional — extra CPU sensors"
    dep "efivarfs"                         "[[ -d /sys/firmware/efi/efivars ]]"        "BIOS memory timings"
    dep "logind/elogind (uaccess)"         "command -v loginctl"                       "Lighting without password"
    dep "GNU tar"                          "tar --version | grep -q GNU"               "Backup → system image"
    dep "pigz (or gzip)"                   "command -v pigz || command -v gzip"        "Backup compression"
    dep "zstd / xz"                        "command -v zstd || command -v xz"          "optional — Backup compression"
    dep "nftables (nft)"                   "command -v nft"                            "Health → Network"
    dep "kernel netfilter/diag options"    "kcfg NETFILTER_NETLINK_QUEUE NF_TABLES NF_TABLES_INET NFT_QUEUE NF_CONNTRACK NFT_CT INET_DIAG INET_TCP_DIAG INET_UDP_DIAG INET_DIAG_DESTROY" "Health → Network"
    if [[ -x /usr/local/bin/lutris-game-tune-wrapper ]]; then
        echo
        echo "Note: lutris-game-tune is still installed (setuid wrapper). centurion-gamemode replaces it;"
        echo "switch the Lutris hooks (Optimizations → Game launch) before running its uninstall.sh."
    fi
fi
