# Health Center refresh

This change is a GUI-only Health redesign. The original helper services, privilege
policies, fault emission, notifications, and hardware tuning/curve reset behavior
are unchanged.

## What changed

- **One-level navigation:** Overview, Problems, Hardware, Maintenance, Advanced;
  with a search box and persistent selected page (`health/page` in gui.ini).
- **System tools directory:** dedicated searchable, categorized catalog, preserving
  all tools from `systools::addPages`. Individual pages are still lazy. SMART
  summary is directly accessible as *Drive health*.
- **Overview:** larger overall verdict, counts of normal/review/attention/unknown
  indicators, last refreshed timestamp, priority-sorted cards, responsive 1/2
  column layout, and read-only, rule-based suggested actions linked to existing
  pages. Acknowledged incidents are not described as repaired.
- **Events & alerts:** meaningful real-world timestamps, short descriptions,
  severity/search filters, optional technical details, inline error explanations,
  kernel-log context actions, and a visible warning when monitoring fails.
- **Historical days:** select a chart day to read available records from the
  current and rotated persistent health logs. Days no longer falsely state that
  no historical events occurred just because this GUI session is recent.
  The display is bounded to 500 recent saved entries and may be incomplete
  if logs were rotated or deleted. Archived rows never enter `eventsSince()`.
- **Drive health:** separate readable SMART cards and an opt-in 11-column
  technical table. Unknown SMART status is not shown as healthy.

## Not changed

No CPU/GPU power, undervolt, curve optimizer, fan control, or helper policies
were modified. No additional periodic hardware polling was introduced.
No network requests were added. The existing diagnostic report workflow and
its privacy preview are retained.

## Validation

Static source/log format checks:

```bash
python3 gui/tests/health_ui_static_test.py
```

On a machine with Qt 6.4+ development packages (Widgets, Network, Svg, DBus):

```bash
cmake -S gui -B build/gui -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/gui -j"$(nproc)"
ctest --test-dir build/gui --output-on-failure
```

Then manually verify Overview actions, searchable navigation, SMART vs details,
event search/severity/technical view, saved-day history and kernel logs, the
tray/Home badge, and the dGPU shortcut. In a restricted-dmesg environment verify
that the root-fallback warning is visible if pkexec fails.

The bundle was prepared in an environment without Qt6 development headers and
without cargo. Consequently the authoring environment cannot certify a full
Qt/Rust binary build; static checks are not a substitute for it.
