# System Tools: Boot preset visibility and OpenRC diagnosis

**Scope:** Health Center → System Tools → Network Tuning, Storage Tuning and Kernel Tuning. The source changes only the GUI. They do not alter kernel tuning values, helper APIs, boot service policy or the boot guard.

## Why checked rows look empty after reopening Centurion

The checkboxes select which rows to include in the **next** Apply Checked / Apply at Boot action. They are not indicators of the effective kernel setting and they are not persisted across launches. The **Live Value** column shows the last readback; the **New Value** editor defaults to that readback for untouched rows. Before this change the UI provided no comparison against the separately persisted boot preset.

- **Apply Checked** applies selected rows to the current running system. It does not save them for the next boot.
- **Apply at Boot** writes selected rows into `/etc/centurion/tune-boot.json` through tune-helper's `set_boot` operation. The page-level action merges the selected rows into any previously saved boot preset, keeping settings from other pages.
- The OpenRC **centurion-tune** service reads this file at boot. Merely adding `centurion-tune` to the boot runlevel does not create or populate it.
- `centurion-boot-guard` can intentionally block presets after an unclean boot. In that case the OpenRC service may return success despite having skipped applying settings. Other services (sysctl, TLP, NetworkManager, user sessions) can also override live values later. The current screenshot cannot distinguish those causes.

## Changes

1. New read-only status line: **Saved for boot / Live matches / Different / Unavailable** counts, computed from the latest tune-helper `describe` response. A row's live-value tooltip includes the stored boot value and highlights a mismatch.
2. **Load Boot Values** copies saved values into checkboxes and editors on the current manual page only. It explicitly **does not change the system** until a separate Apply Checked action. If edits are pending, the user is asked before they are overwritten. Missing or unsupported values are skipped.
3. Manual System Tools pages refresh their live values while visible, even when the main Optimize tab is hidden. Unused pages remain inactive to avoid unnecessary work.
4. The existing OpenRC boot guard and the explicit Apply at Boot workflow remain unchanged; no automatic reapply or reboot is introduced.

## Safe read-only checks on Gentoo / OpenRC

```bash
rc-update show boot | grep -E 'centurion-(tune|boot-guard)'
rc-service centurion-tune status
sudo test -s /etc/centurion/tune-boot.json && echo 'Boot preset file exists' || echo 'Boot preset missing or empty'
sudo /usr/libexec/centurion/centurion-boot-guard status
```

To inspect the saved preset **without writing any settings**:

```bash
sudo python3 -c 'import json; p="/etc/centurion/tune-boot.json"; d=json.load(open(p)); print("Preset:",d.get("preset")); print("Saved settings:",len(d.get("values",{}))); print(*sorted(d.get("values",{}).items()), sep="\n")'
```

**Do not** manually run `rc-service centurion-tune stop` just to troubleshoot: its `stop()` restores the original values and would itself reset live tuning. Do not reset a tripped boot guard until the unstable preset has been reviewed.

### What the results mean

- Missing preset file: the GUI's Apply at Boot step did not persist a valid preset (or it was cleared); enabling the service alone is not enough.
- Preset exists and the guard reports paused/tripped: investigate what caused the unstable boot before re-enabling presets.
- Preset exists and the guard permits it, but live values differ: examine service startup result and other services that may overwrite the same sysctl/debugfs settings.
- All saved values match live: the preset worked, and unchecked GUI rows are just transient selection state.

## Verification limitations

Source-level checks are included in `gui/tests/boot_preset_home_ui_static_test.py`. A real Qt6 build, OpenRC reboot and hardware readback need to be tested on the target Gentoo host. No changes to the low-level Rust helper are included.
