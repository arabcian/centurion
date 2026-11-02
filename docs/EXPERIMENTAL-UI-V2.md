# Centurion Experimental UI v2 — source implementation

This revision **changes Qt widget composition**, not just QSS or tab captions.
It is built on the same underlying operational controls, signals, tuning helpers,
permission policy and hardware-detection gates as the Classic UI. The prior
experimental revision only rearranged the top-level navigation; this one also
recomposes the main workspaces.

## Implemented layouts

- **Home** — a four-reading strip (CPU/GPU state, CPU power, fans), left-side
  profile and real device controls, right-side painted five-minute CPU/EC GPU
  temperature plot and switchable Live sensors / Hardware & firmware details.
  Theme, language and UI mode are under the *Preferences* menu. Reads use the
  existing 2-second worker sweep, and GPU samples come solely from the existing
  EC feed when the GPU is awake. No NVML access is introduced on Home.
- **Ryzen Curve** — original CCD/SMU-slot grid retained and moved to the left.
  A *Selected SMU slot* editor on the right writes into the original per-slot
  QLineEdit and checkbox. Offsets are correctly labeled **CO steps**, not mV.
  All-core, reset, profiles, live monitor and trial guard remain in their
  original paths. The existing Apply buttons still control hardware writes.
- **NVIDIA Curve** — original interactive VfCurveWidget is the main canvas,
  with the real saved-profile actions in a secondary panel. Original curve
  controls, locks, compensation, PowerMizer and reset actions are below the
  graph. No fake clock/power-limit sliders are introduced.
- **Optimize** — original full-width, eight-column preset/autotune strip becomes
  a task-focused autotune workspace beside the saved preset and boot/game
  actions. The entire tuning-tab catalog, setting search and Apply checked
  operation remain.
- **Health Overview** — real saved-log/health-helper findings are presented as
  a single-column investigation queue next to the real status, recommendation
  action and deep links to existing subsystems. No invented health score.
- **Scenes** — resizable scene picker and scrollable original editor, preserving
  all automatic triggers, per-component choices, import/export and shortcuts.
- **Lighting** — real interactive keyboard layout and effect-list editor
  arranged in a resizable split workspace. Original profile writes are intact.
- **Firmware Attributes** — task/safety header above the original discovered
  firmware attribute editors. It does not claim generic BIOS/EC update support.

The common Experimental stylesheet now includes shared splitters, metric strip,
labels, action links and nested detail tabs. The existing Classic styling and
widget-composition paths are retained. The Intel undervolt, AMD GPU, zone RGB,
backup, cooling, reports and advanced subtools retain their existing widgets;
Experimental styling applies to them as well, but those specialist editors have
**not** been separately rebuilt to match a mockup.

## Safety / compatibility scope

- No Rust code, privileged helper, sysfs path or firmware/SMU/MSR/NVML write
  operation was changed in this revision.
- No sensor values are hardcoded. The plot stays empty without measurements,
  missing GPU data is never interpreted as zero, and no extra GPU wakeup is made.
- Read-only UI mirrors do not apply tuning on their own.
- No tab is introduced for hardware that Centurion did not originally detect.
- No claims of feature-parity certification or an end-to-end binary build are
  made without testing on real Linux hardware.

## Validation and getting a build

Run these in the project root:

```sh
python3 gui/tests/health_ui_static_test.py
python3 gui/tests/experimental_ui_static_test.py
python3 gui/tests/experimental_layout_static_test.py
cmake -S gui -B build/gui -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/gui -j"$(nproc)"
ctest --test-dir build/gui --output-on-failure
```

Qt6 >= 6.4 development headers with Widgets, Network, Svg and DBus are required.
The authoring environment lacks Qt6 development packages and cargo: only source
parity/static checks could be completed. The screenshots shown previously were
concept illustrations and **not screenshots of this compiled implementation**.
A real Qt build and screenshot pass is still needed before calling this release
feature-parity verified.

To run original UI, set `CENTURION_UI=classic` or use the Home > Preferences
selector and restart. `CENTURION_UI=experimental` forces the new layout.
