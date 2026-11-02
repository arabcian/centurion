# Centurion Experimental UI — first implementation

Branch: `experimental/ui-overhaul` (the original source is available on `master`).

This is a **real Qt Widgets code change**, not a picture or an HTML mockup. It is
an initial cohesive UI pass based on the compact Graphite/amber concept. It does
**not** claim to reproduce every pixel of the concept art, nor to have passed a
hardware integration test.

## User-facing changes

- Flat, consistently styled surfaces across every existing Qt tab, including
  the Home, Health, CPU/GPU curves, Optimize, Scenes, Lighting, and Firmware pages.
  The existing Centurion theme palettes (Graphite, Gruvbox, etc.) remain usable.
- Top-level page order: Home, Health Center, AMD/Intel CPU controls (if present),
  NVIDIA/AMD GPU controls (if present), Optimize, Scenes, Lighting (if present),
  Firmware (if present). No page is deleted or eagerly instantiated to do this.
- `All sections` header menu lists every page available on the current device,
  even where a narrow window cannot fit its tab label.
- `Ctrl+K` opens the read-only navigation palette. It searches top-level tabs,
  the currently visible page's sections/controls, its nested subtabs, and visible
  one-level navigation trees. Choosing an item **navigates or focuses**; it does
  **not** click the control, apply a curve, write sysfs, or run a helper.
- Current main page is restored on next launch by stable page name, not unstable
  numerical index; optional hardware pages may appear/disappear.
- Optimize has a live autocomplete search for all available tuning rows; selecting
  a match brings its original editor into view without changing its value.
- Home has three quick navigation links; the CPU CCD cards and Health overview
  are less ornamental; Scenes has a slightly wider preset list.
- Choose **Experimental** or **Classic** in Home > Interface, then restart.
  This selection is saved to `~/.config/centurion/gui.ini` (`[ui] mode=`), and
  changes only the interface. `CENTURION_UI=classic` and
  `CENTURION_UI=experimental` override the saved mode for one launch.

## Intentionally unchanged

- The Rust helpers, their privilege levels and IPC, hardware probe addresses,
  fan safety guard, power / undervolt / PBO / OC algorithms, stress/validation
  programs, SMART and historical log parsing, kernel monitoring, notification
  routing, tray behavior and backup/import/export format.
- All hardware-conditional tabs and the original late NVIDIA tab creation.
  No new GPU polling or stress testing takes place to populate navigation.
- Health Center's existing local navigation and system tools directory.
- Special input protections: mouse wheel over a combo box never changes its
  value, and tuning buttons retain their existing confirmation flows.

The mockup sometimes illustrates unavailable controls (e.g. unlocked laptop GPU
power limits, firmware downloads). **No unimplemented or unsafe device control
was added simply because it appeared in a generated concept image.**

## Build / validation

On Gentoo with Qt 6.4+ development packages, C++20 compiler, CMake and Ninja:

```sh
cmake -S gui -B build/gui -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/gui -j"$(nproc)"
python3 gui/tests/health_ui_static_test.py
python3 gui/tests/experimental_ui_static_test.py
# if you enable the existing Qt tests during configuration:
ctest --test-dir build/gui --output-on-failure
./build/gui/centurion --window
```

UI mode is not a different daemon or set of helpers: installing this branch
normally retains the previous backend. Switching to the original Git branch
(or to Classic in this build) is a voluntary user choice.

## Mandatory on-device regression checklist

1. With AMD Zen 5 / Intel systems, verify that only supported CPU controls
   appear; switch GPU from iGPU-only to hybrid and check late NVIDIA creation.
2. Select all available top tabs and every Health navigation item; confirm
   `Ctrl+K` opens existing nested subtabs without triggering any write.
3. Resize to 620×420 and 1366×768; ensure All sections and the tab scroller
   remain operable, and check for horizontal overlap in Home settings.
4. Exercise CPU/GPU profile saving/loading, trial expiry, cancel, rollback,
   scene switching, fan auto/max and device toggles. Check that they perform
   exactly the same commands as the original branch.
5. Check SMART, old Health event days, report privacy preview, history,
   boot events and tray notifications. Verify no surprise GPU wake on Home.
6. Switch to Classic, restart, and switch back; check persistence across
   reboot and command-line environment override.

### Verification boundary

In this authoring environment, Qt6 development headers, Qt6 CMake package files
and Rust Cargo are missing. Full C++/Rust compilation and real EC/SMU/GPU
integration verification **have not been performed**. The static checks validate
source-level integration only. The implementation should be treated as an
experimental/test build until the above tests succeed on a supported machine.
