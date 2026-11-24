# Isolated simulation and visual comparison

`centurion --simulate` enters a **separate fixture workspace** before the production shell, hardware whitelist, D-Bus registration, instance lock, tray, or helper code. XDG config, data, state, cache and runtime roots are redirected to a fresh temporary directory. The workspace only exposes the existing LineChart and V/F editor, a bounded in-memory global offset demonstration, and the existing S10/S13 report generators over a second generated temporary tree. No fix actions are exposed. These fixtures are not physical hardware readings or an emulator for the full MainWindow. Normal startup is unchanged.

Live palette switching is enabled in this migrated workspace. Semantic token storage has stable addresses after startup; the revision signal is available to migrate the rest of the application. Since Tur24 the Home selector, gallery and custom palette use the live path; not all cached assets or production pages have been visually accepted. No production helper or firmware action is connected to the new theme API.

Tur26 extended the shared six-slot telemetry service with asynchronous Home, Health CPU and both D-Bus sensor endpoints. Safety and post-write readbacks remain direct. Stable device lifetime/hotplug handling, Stress/SMART consumers and the full adaptive scheduler remain open. The isolated workspace is still not a real MainWindow.

## Native visual tests

After installing the normal build dependencies:

```sh
cmake -S gui -B build-debug -DCMAKE_BUILD_TYPE=Debug -DCENTURION_BUILD_TESTS=ON -DCENTURION_SANITIZE=ON -DCENTURION_LTO=OFF
cmake --build build-debug --parallel
QT_QPA_PLATFORM=offscreen ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-debug --output-on-failure
```

`simulation-visual-*` exercises six fixture choices, a 20-change live-palette sequence with preserved V/F draft, selection and zoom, invalid custom data, a diagnostics tab, representative dark/pastel/OLED palettes, 760×540, 900×600 and 1280×800 logical sizes, 100/125/150/175/200% scale factors, English/Turkish (when LinguistTools is available), and both shell-style choices. These are fixture workspace images, **not screenshots of every actual production page**. The original all-palette appearance smoke test remains in CTest.

The image comparison counts pixels for which any channel differs by more than 12 of 255; a fraction over 0.005 fails. Dimension differences or missing references fail when references are explicitly supplied. It also compares two consecutive renders and checks basic control geometry. Without approved reference images this is a repeat-render smoke/geometry test, not a regression test across commits.

Actual PNGs are saved in `build-debug/visual-artifacts/<mode>-<language>-<scale>/`. CI uploads the same folder from `build`. Review the actual images manually, copy only approved references to an independent directory, and run:

```sh
CENTURION_VISUAL_BASELINES=/absolute/path/to/approved-references \
  QT_QPA_PLATFORM=offscreen ASAN_OPTIONS=detect_leaks=0 \
  ctest --test-dir build-debug -R simulation-visual --output-on-failure
```

The test never changes references. Mismatches produce expected/actual/diff images where dimensions permit. Keep the reference renderer, Qt version, fonts, locale and scale fixed. The baseline path is optional because there are no reviewed reference images in this source archive. This is not a replacement for real production-page visual baselines, clipping checks on all widgets, custom palette coverage, or a real hardware-backed test.

Tur32: set `CENTURION_VISUAL_REQUIRE_BASELINES=1` together with `CENTURION_VISUAL_BASELINES` to make a missing reference directory a failure, not smoke-only success. Both shell-style settings are now enumerated for each scale/language. No reviewed production-shell baselines are bundled; this expanded fixture matrix is not full A07/A08 acceptance.
