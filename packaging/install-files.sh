#!/bin/bash
# Centurion file layout: installs already built binaries and the integration files.
# install.sh sources this after building; distribution packages (AUR, COPR) run it directly
# so every package installs the same list:
#
#   DESTDIR=$pkgdir PREFIX=/usr bash packaging/install-files.sh
#
# Expects to run from the source tree root with target/release (cargo build --release) and
# gui/build (cmake -S gui -B gui/build …) built. Knobs (environment): DESTDIR PREFIX LIBEXEC
# UNITDIR UDEVDIR ELOGIND_SLEEP SYSTEMD_SLEEP (both detected on this machine when unset).
# The polkit exec.path annotations and the GUI's CENTURION_HELPER_DIR must both point at $LIBEXEC.
# Nothing here enables a service or touches files outside $DESTDIR.
if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    set -euo pipefail
    cd "$(dirname "$0")/.."
    PREFIX=${PREFIX:-/usr}
    DESTDIR=${DESTDIR:-}
    LIBEXEC=${LIBEXEC:-$PREFIX/libexec/centurion}
    UNITDIR=${UNITDIR:-$PREFIX/lib/systemd/system}
fi
own=(-o root -g root); [[ $EUID -eq 0 ]] || own=()
T=target/release
install -d "${own[@]}" -m 0755 "$DESTDIR$LIBEXEC" "$DESTDIR$PREFIX/bin"
install "${own[@]}" -m 0755 "$T/legion-profile-helper" "$T/fwattr-helper" "$T/ryzen-co-helper" "$T/tune-helper" "$T/tune-profile-helper" "$T/intel-uv-helper" "$T/legion-gpu-helper" "$T/legion-firmware-helper" "$T/lighting-helper" "$T/amdgpu-helper" "$T/nvcurve-sensors" "$T/legion-ec-sensors" "$T/health-reader" "$T/centurion-boot-guard" "$T/netguard-helper" "$T/backup-helper" \
    "$DESTDIR$LIBEXEC/"
install "${own[@]}" -m 0700 "$T/nvcurve-root-helper" "$DESTDIR$LIBEXEC/"
install "${own[@]}" -m 0755 "$T/nvcurve" "$T/centurion-gamemode" "$T/centurion-intel-uv" "$T/centurion-autotune" "$T/centurion-calibrate" "$T/centurion-netguard" "$T/centurion-display" "$T/centurion-cooltest" "$T/centurion-gpustress" "$T/centurion-cpustress" "$DESTDIR$PREFIX/bin/"
DESTDIR="$DESTDIR" cmake --install gui/build --strip

# Packages put the rules under /usr/share (POLKIT_RULESDIR): /etc/polkit-1/rules.d belongs to polkit
# (0750 on Arch), and a copy there with the same name overrides the packaged one (local level change).
POLKIT_RULESDIR=${POLKIT_RULESDIR:-/etc/polkit-1/rules.d}
install -d "${own[@]}" -m 0755 "$DESTDIR$PREFIX/share/polkit-1/actions" "$DESTDIR$POLKIT_RULESDIR" \
    "$DESTDIR/etc/init.d" "$DESTDIR/etc/nvcurve/profiles"
# Every action id in the policy must be known to the rules file (allowlist, not a namespace prefix).
for id in $(grep -o 'action id="com\.centurion\.[^"]*"' packaging/polkit/com.centurion.policy | cut -d'"' -f2); do
    grep -qF "\"$id\"" <(grep -v '^ *\*' packaging/polkit/49-centurion.rules) || [[ $id == com.centurion.firmware.write || $id == com.centurion.backup || $id == com.centurion.netguard ]] \
        || { echo "polkit action $id is in com.centurion.policy but not in the rules allowlist (49-centurion.rules)" >&2; exit 2; }
done
sed "s|@LIBEXEC@|$LIBEXEC|g" packaging/polkit/com.centurion.policy > "$DESTDIR$PREFIX/share/polkit-1/actions/com.centurion.policy"
[[ $EUID -eq 0 ]] && chown root:root "$DESTDIR$PREFIX/share/polkit-1/actions/com.centurion.policy"; chmod 0644 "$DESTDIR$PREFIX/share/polkit-1/actions/com.centurion.policy"
sed -e "s|@LIBEXEC@|$LIBEXEC|g" packaging/polkit/49-centurion.rules > "$DESTDIR$POLKIT_RULESDIR/49-centurion.rules"
[[ $EUID -eq 0 ]] && chown root:root "$DESTDIR$POLKIT_RULESDIR/49-centurion.rules"; chmod 0644 "$DESTDIR$POLKIT_RULESDIR/49-centurion.rules"
# Keyboard lighting: uaccess on the Spectrum controller's hidraw node.
UDEVDIR=${UDEVDIR:-$PREFIX/lib/udev/rules.d}
install -d "${own[@]}" -m 0755 "$DESTDIR$UDEVDIR"
install "${own[@]}" -m 0644 packaging/udev/70-centurion-lighting.rules "$DESTDIR$UDEVDIR/"
sed -e "s|@LIBEXEC@|$LIBEXEC|g" packaging/udev/90-centurion-dgpu.rules > "$DESTDIR$UDEVDIR/90-centurion-dgpu.rules"
chmod 0644 "$DESTDIR$UDEVDIR/90-centurion-dgpu.rules"
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
    # Sleep log for Health → Sleep (duration, battery used, wake source, dGPU state).
    install "${own[@]}" -m 0755 packaging/sleep/centurion-sleep-log "$DESTDIR$ELOGIND_SLEEP/40-centurion-sleep-log"
fi
SYSTEMD_SLEEP=${SYSTEMD_SLEEP:-}
if [[ -z $SYSTEMD_SLEEP ]]; then
    for d in /usr/lib/systemd /lib/systemd; do [[ -d $d/system ]] && { SYSTEMD_SLEEP=$d/system-sleep; break; }; done
fi
if [[ -n $SYSTEMD_SLEEP ]]; then
    install -d "${own[@]}" -m 0755 "$DESTDIR$SYSTEMD_SLEEP"
    install "${own[@]}" -m 0755 packaging/sleep/centurion-sleep-log "$DESTDIR$SYSTEMD_SLEEP/centurion-sleep-log"
fi
