# Centurion Experimental — Tur 32 kısmi doğrulama ve devir

Temel: `Centurion-experimental-tur31-kismi.zip` (SHA-256 `535bc27613f3234fa1d5e1ac04ab6154b3e91863b1e019645e9ff2cc23ee0859`). Tur29 özel 11 istek ve build düzeltmesi korunur. Kabul: **0/48 tam, 47 kısmi, D05 engelli.** Özgün kapsam daraltılmadı.

## Gerçek kaynak değişiklikleri

- A06: DayBars visible range zoom/pan and visible/all CSV, keyboard selection within visible range, BarChart table/export, V/F frozen displayed curve table and CSV with matched voltage layout, visible-only hit-test and left-button edit guard.
- A07: Isolated simulation kept; V/F export context menu disabled there to preserve isolated fixture behavior. No MainWindow injection.
- A08: Fixture CTest matrix expanded to five scales including 175%, both shell-style settings per language, three sizes, explicit baseline-required environment flag.
- D02: Opt-in approximate software animation for six effects, stops on hide/busy, existing ordered effect list and undo preserved, reactive/host static, helper structural validation before Apply, draft editor locked during busy, explicit unverified result.
- D03: Unprivileged bounded config_preview streaming GNU/ustar (gzip or plain), member paths/type/size/archive mode and whitelist-scope candidates, duplicate/traversal/special/link-in-restored-scope refusal, fd metadata identity before/after, no extraction. Legacy fallback only explicit for old helper/unsupported compression. System image sidecar chain shown as hierarchy with archive and metadata identity checks. Nonzero/crashed helper cannot report ok.
- X09: Zone4 draft canvas (4 stable zones), click/keyboard selection and color edit, shared viewport zoom/pan/fit, font-aware height, busy editor lock, real Qt fixture added.
- D04: 63 new TR catalogue entries; no earlier requested GUI changes removed.
- D05: Qt Debug/Release configure attempted; 59 independent runners and seven final targeted reruns pass; new Qt and Rust test sources and CI runner.

Lighting software preview is approximate and opt-in, not firmware timing. Existing explicit Apply, controller profile storage, undo and helper safety remain. Host/reactive effects are not fabricated as live animation. V/F display index is not hardware point ID; missing/incompatible reference layouts give blank comparison columns, no Apply authorization. CSV text columns are prefixed with an apostrophe for spreadsheet safety; this is not a byte-for-byte label roundtrip. System-image preview still reads sidecar metadata only. Configuration member preview accepts plain/gzip GNU/ustar, rejects unsupported PAX/sparse and unsafe paths, 4096 members, 512 KiB cumulative path names, 1 MiB manifest, existing MAX_UNPACK_BYTES (1.25 GiB) decompression cap, cooperative 30 s check between reads, not strict wall-clock deadline. Archive modes are not target permissions, and candidate scope does not establish eventual write or authentication. No new restore write path. Old helper fallback explicit only for unknown op/unsupported compression. Native Rust parser/backup integration is uncompiled; test source added, not executed. The common backup process exit gate rejects an ok reply after nonzero/crash. Actual MainWindow simulation stays untouched; do not claim safety acceptance based on fixture workspace.

## Testler

| Test | Sonuç |
|---|---|
| Tur31 kaynak ZIP CRC | PASS |
| Tur31 tam baseline runner | Bu tur ayrı çalıştırılmadı; önceki 58/58 raporu |
| Tur32 ilk tam runner | 59/59 PASS |
| Son etkilenen rerun | 7/7 PASS |
| Yeni pure presentation fixture | O2, LTO -Werror, ASan+UBSan PASS; 100000 random frames and view mappings per run |
| Qt6 Debug/Release configure | FAIL: Qt6Config.cmake yok |
| Apt update | DNS çözümleme başarısız |
| Rust build/test/clippy/deny | Cargo yok, çalıştırılmadı |
| Qt CTest, new Zone4/DayBars layout and visual fixtures | Çalıştırılmadı |
| Rust strict configuration preview parser tests | Çalıştırılmadı |
| Gerçek GUI, Gentoo PGO/LTO, fiziksel donanım, CI | Çalıştırılmadı |

Bağımsız runner'lar statik ve native C++ karışımıdır. Qt/Rust uygulamasının derlendiği veya fiziksel güvenlik kabulü anlamına gelmez.

## Kalan özgün kapsam

- A06: Common time/unit transforms, multi-series/legend/nearest-point parity, 100k actual Qt performance, CSV roundtrip and full native VF selection/drag/regression.
- A07: Full real MainWindow fail-closed backend injection, fake host counters, all production pages, lifecycle and physical isolation tests.
- A08: Reviewed real-shell baselines, production pages, all palette/rendered states/contrast, native execution and intentional regressions; no approved PNGs bundled.
- D02: Full graphical timeline and status/readback parity, exact reactive/rotation/direction model (only approximation now), full device capacity validation and actual Qt/HID tests.
- D03: Per-file content hash and cryptographic authentication, target path/permission/create/replace revalidation, target-safe FD traversal, complete restore receipts/history/registry, PAX/other formats, strict wall-clock timeout and native Rust/Qt tests. Image file-level unsupported; advisory only.
- X09: Native DPI/resize/hit-test/keyboard behavior, visual review, physical no-HID-write verification, richer physical layout and undo parity.
- D04: FIX-01–21 full native acceptance, all production layout/translation/contrast tests.
- D05: Qt6/Cargo unavailable; native Qt/Rust/CTest/CI/Gentoo PGO/LTO/physical validation open.
- Tüm diğer 48 paket / 118 GUI / ek ID / FIX / Wanted kriterleri önceki haliyle açık. Orijinal hücreler aynen korunmuştur.
- GitHub push yok.
