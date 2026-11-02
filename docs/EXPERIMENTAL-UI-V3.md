# Centurion Experimental / Studio UI v3 (source change)

## Direction

This is a **distinct Qt Widgets shell**, not another tab-bar skin. Experimental mode has a 184 px navigation rail (56 px collapsed), a compact contextual title/search header, and the remaining area belongs to the working page. The navigation contains only physically available hardware tabs; hidden pages are neither constructed by search nor offered as dead controls. At widths under 1100 px, the rail collapses automatically unless its position was manually saved. Ctrl+B toggles it; Ctrl+K opens the existing read-only quick navigator. The rail uses purpose-drawn, HiDPI-aware vector line icons instead of theme-dependent icon fonts. The last page is still restored by its stable canonical ID.

Appearance: graphite, minimal flat surfaces, restrained amber accent, tight type hierarchy, 5–7 px control radii, compact groupboxes. This styling applies to all pages, including Health, Intel, AMD GPU, Scenes, Lighting, and Firmware. All existing colour palettes remain available. Classic mode retains the original top tab bar and styling; switch in Home > Preferences > Interface, or use `CENTURION_UI=classic`.

## Actual workspace changes

- **Home:** One compact four-item live telemetry strip; original profile and EC/fan/device controls left, live graph and sensor/hardware details right. Eliminated the *Quick access* button farm now duplicated by the app rail; shortened explanatory material without removing informational pages.
- **Ryzen:** The fixed SMU slot matrix is the editor; removed the redundant second 'selected slot' editor (an experimental-only v2 addition) and its GUI synchronization timer. All-Core control stays full width above the matrix; existing profile/apply/reset/log/trial flows remain intact.
- **NVIDIA:** Original interactive V/F curve gets the full content width; profile actions move to the GPU status band above. Removes the static instructions/right rail, not GPU controls. The original live status, curve editing, cap/clock/PowerMizer, apply/reset and log remain in place.
- **AMD GPU:** The dynamic power, voltage/curve and fan controls are composed in two columns in experimental mode, with existing buttons, trial rollback and live polling unchanged.
- **Optimize:** Preset and Autotune workflows are compact horizontal rows above the full-width settings editor and search. Original calibration, custom weights, boot controls and Apply checked remain connected.
- **Health:** Slightly narrower searchable tool rail; all original Health pages are still reachable.
- **Firmware:** Removed an otherwise empty duplicated explanatory banner; Custom-mode locking and Apply All stay untouched.
- **Scenes, Lighting, Intel, 4-zone fallback and other pages:** Existing operational widgets and previous v2 layout compositions remain, with the new shared shell and compact visual system. These individual forms have **not** been rewritten from scratch in v3.

## Preserved behavior

No Rust crate, helper, polkit rule, IPC protocol, thermal guard, tuning limits, device probe or polling algorithm was modified. The experiment changes QWidget composition only. Hardware-gated tabs, dynamic dGPU reappearance, Health→Home health level, Health cooling→Home fan actions, SceneEngine and profile events remain connected. Search/navigation never write hardware.

## Verification and limitations

Static checks: source-vs-v2.1 audit (11 GUI source files modified and no crates or helpers changed), C++ lexical delimiter balance, canonical page IDs and necessary cross-tab connections. Archive integrity is tested before delivery.

**Not verified:** actual Qt6 compilation, linking, runtime GUI screenshots, responsive display at every DPI, Gentoo-specific runtime interactions or end-to-end hardware feature parity. This environment has no Qt6 >=6.4 development files (`Qt6Config.cmake`), and no Cargo. CMake stops at `find_package(Qt6 ...)`, so this is source delivery and **not a tested binary**.

Build on Gentoo with the normal project dependency set, for example:

```bash
cmake -S gui -B gui/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build gui/build -j$(nproc)
CENTURION_UI=experimental gui/build/centurion --window
```

Test the Classic fallback with `CENTURION_UI=classic`. A full real-device interaction pass is still required before calling the branch production-ready.
