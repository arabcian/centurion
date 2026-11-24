# Isolated simulation and visual comparison

`centurion --simulate` enters a **separate fixture workspace** before the production shell, hardware whitelist, D-Bus registration, instance lock, tray, or helper code. XDG config, data, state, cache and runtime roots are redirected to a fresh temporary directory. The workspace only exposes the existing LineChart and V/F editor, a bounded in-memory global offset demonstration, and the existing S10/S13 report generators over a second generated temporary tree. No fix actions are exposed. These fixtures are not physical hardware readings or an emulator for the full MainWindow. Normal startup is unchanged.

Live palette switching is enabled in this migrated workspace. Semantic token storage has stable addresses after startup; the revision signal is available to migrate the rest of the application. **The normal Home theme selector still requires restart** because local QSS, cached item brushes/icons and other production widgets have not all been migrated. No production helper or firmware action is connected to the new theme API.

The shared telemetry cache currently covers six textual display readouts (Home and textual D-Bus Sensors). CPU safety reads, battery history, numeric D-Bus readings, dGPU policy and transaction readbacks remain direct. A miss on the winning caller is synchronous; Home's existing periodic sweep is a worker, but the legacy D-Bus method and initial probes are not yet fully asynchronous. Device identity/source metadata and application-wide subscriptions remain follow-up work. Samples older than the slot's TTL and superseded generations cannot be returned as fresh; concurrent callers receive unavailable/pending instead of blocking or starting another read. The cache does not make a stuck sysfs read cancellable.

## Native visual tests

After installing the normal build dependencies:

```sh
cmake -S gui -B build-debug -DCMAKE_BUILD_TYPE=Debug -DCENTURION_BUILD_TESTS=ON -DCENTURION_SANITIZE=ON -DCENTURION_LTO=OFF
cmake --build build-debug --parallel
QT_QPA_PLATFORM=offscreen ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-debug --output-on-failure
```

`simulation-visual-*` exercises six fixture choices, a 20-change live-palette sequence with preserved V/F draft, selection and zoom, invalid custom data, a diagnostics tab, representative dark/pastel/OLED palettes, 900×600 and 1280×800 logical sizes, 100/125/150/200% scale factors, English/Turkish (when LinguistTools is available), and both shell-style choices. These are fixture workspace images, **not screenshots of every actual production page**. The original all-palette appearance smoke test remains in CTest.

The image comparison counts pixels for which any channel differs by more than 12 of 255; a fraction over 0.005 fails. Dimension differences or missing references fail when references are explicitly supplied. It also compares two consecutive renders and checks basic control geometry. Without approved reference images this is a repeat-render smoke/geometry test, not a regression test across commits.

Actual PNGs are saved in `build-debug/visual-artifacts/<mode>-<language>-<scale>/`. CI uploads the same folder from `build`. Review the actual images manually, copy only approved references to an independent directory, and run:

```sh
CENTURION_VISUAL_BASELINES=/absolute/path/to/approved-references \
  QT_QPA_PLATFORM=offscreen ASAN_OPTIONS=detect_leaks=0 \
  ctest --test-dir build-debug -R simulation-visual --output-on-failure
```

The test never changes references. Mismatches produce expected/actual/diff images where dimensions permit. Keep the reference renderer, Qt version, fonts, locale and scale fixed. The baseline path is optional because there are no reviewed reference images in this source archive. This is not a replacement for real production-page visual baselines, clipping checks on all widgets, custom palette coverage, or a real hardware-backed test.
