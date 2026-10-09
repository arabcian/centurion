#!/bin/bash
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "run as root" >&2; exit 1; }
PREFIX=${PREFIX:-/usr}
UNITDIR=${UNITDIR:-$PREFIX/lib/systemd/system}
if [[ -d /run/systemd/system ]]; then
    systemctl disable nvcurve-autoload.service centurion-tune.service centurion-intel-uv.service centurion-intel-uv-daemon.service centurion-boot-guard.service 2>/dev/null || true
    systemctl disable --now centurion-netguard.service 2>/dev/null || true
    systemctl disable centurion-calibrate-boot.service 2>/dev/null || true
else
    rc-update del nvcurve-autoload default 2>/dev/null || true
    rc-update del centurion-tune boot 2>/dev/null || true
    rc-update del centurion-intel-uv boot 2>/dev/null || true
    rc-update del centurion-intel-uv-daemon default 2>/dev/null || true
    rc-update del centurion-boot-guard 2>/dev/null || true
    rc-service centurion-netguard stop 2>/dev/null || true
    rc-update del centurion-netguard default 2>/dev/null || true
    rc-update del centurion-calibrate-boot default 2>/dev/null || true
fi
# Put every tuned value back before the helper disappears.
if [[ -x "$PREFIX/libexec/centurion/tune-helper" ]]; then
    th="$PREFIX/libexec/centurion/tune-helper"
    if [[ ! -L $th && $(stat -c '%u' "$th") == 0 && $(( 0$(stat -c '%a' "$th") & 022 )) == 0 ]]; then
        printf '%s' '{"op":"restore"}' | "$th" >/dev/null || echo "warning: tune restore failed; some knobs may stay changed until reboot" >&2
    else
        echo "warning: $th is not root-owned/safe — skipping restore" >&2
    fi
fi
rm -rf "$PREFIX/libexec/centurion"
rm -f "$PREFIX/bin/centurion" "$PREFIX/bin/nvcurve" "$PREFIX/bin/centurion-gamemode" "$PREFIX/bin/centurion-intel-uv" "$PREFIX/bin/centurion-autotune" "$PREFIX/bin/centurion-calibrate" "$PREFIX/bin/centurion-netguard" /etc/init.d/centurion-netguard "$UNITDIR/centurion-netguard.service" /etc/init.d/centurion-calibrate-boot "$UNITDIR/centurion-calibrate-boot.service" \
      /etc/init.d/centurion-tune /etc/init.d/centurion-intel-uv /etc/init.d/centurion-intel-uv-daemon /etc/init.d/centurion-boot-guard {/lib64,/usr/lib64,/lib,/usr/lib}/elogind/system-sleep/50-centurion-intel-uv \
      "$PREFIX/share/applications/centurion.desktop" \
      "$PREFIX/share/icons/hicolor/scalable/apps/centurion.svg" \
      "$PREFIX/share/polkit-1/actions/com.centurion.policy" \
      /etc/xdg/autostart/centurion.desktop \
      /etc/polkit-1/rules.d/49-centurion.rules /etc/init.d/nvcurve-autoload
rm -f "${UDEVDIR:-$PREFIX/lib/udev/rules.d}/70-centurion-lighting.rules" "${UDEVDIR:-$PREFIX/lib/udev/rules.d}/90-centurion-dgpu.rules"
command -v udevadm >/dev/null && udevadm control --reload 2>/dev/null || true
rm -f "$UNITDIR/nvcurve-autoload.service" "$UNITDIR/centurion-tune.service" "$UNITDIR/centurion-intel-uv.service" "$UNITDIR/centurion-intel-uv-daemon.service" "$UNITDIR/centurion-boot-guard.service"
# Boot-guard state goes; the BIOS memory-timing backups (AodSetupRpl-*) stay:
# they are the only copy of the variable from before an edit.
rm -f /var/lib/centurion/boot-guard.json /var/lib/centurion/boot-guard.json.tmp \
      /var/lib/centurion/gpu-mode-pending.json /var/lib/centurion/gpu-mode-pending.json.tmp
rm -rf /var/cache/centurion   # probed values only, rebuilt on demand
rmdir /var/lib/centurion 2>/dev/null || true
[[ -d /run/systemd/system ]] && systemctl daemon-reload 2>/dev/null || true
[[ -L /run/centurion ]] && rm -f /run/centurion || rm -rf /run/centurion
# Everything that is left on purpose, with what it is, so nothing stays behind unnoticed.
echo "Removed. Left in place (delete by hand if you do not want them):"
left() { [[ -e $1 ]] && printf '  %-34s %s\n' "$1" "$2"; return 0; }
left /etc/nvcurve                  "NVIDIA curve profiles and the default-profile setting"
left /etc/centurion     "boot presets, approved Optimizations presets, network guard rules"
left /var/lib/centurion "BIOS memory-timing backups (AodSetupRpl-*), calibration signature, boot defaults"
left /var/log/centurion "writes.log, network guard and connection logs"
for b in /backup-*.tar.*; do left "$b" "system backup made from Health > Backup (others may be where you saved them)"; done
echo "  per user: ~/.config/centurion, ~/.config/ryzen-curve-optimizer (scenes, presets, profiles)"
echo "            ~/.local/state/centurion (Health and kernel-log history)"
