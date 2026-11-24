# Centurion Experimental — Tur 28 kısmi teslimat

## Baseline ve kapsam
Tur27 ZIP SHA-256 `f8bf9bddc1cfac16968884514b8d0cac1cd43a043f80dda79e343bbd0401c226`, 691 dosya, CRC PASS. 12 mevcut kaynak/konfigürasyon dosyası değiştirildi, 7 yeni kaynak/test dosyası eklendi. Önceki bütün dosyalar ve 48 paket, 118 GUI ID, 21 FIX matris hücreleri korundu; ek ID özgün paket satırlarında. 0/48 tam kabul, 47 kısmi, D05 engelli. Tur27 bağımsız sonuç önceki teslimata ait; bu tur baseline full runner seti ayrıca çalıştırılmadı. Baseline Qt Debug/Release configure başarısız.

## A05 — Scoped Operation Registry
`operationclaims.h` explicit audited helper/op allowlist; CPU curve, GPU curve/profile, tuning kaynakları. `operationregistry.h` GUI thread tryBegin bütün claims'i tek adımda kontrol eder. Read/read serbest, write çakışması reddedilir; no QProcess start. Timeout/owner gone sonrasında process gerçekten çıkmadan rezervasyon kalkmaz. `privileged.cpp` Conflict sonucu history'de görünür. Acil reset ve release/prune mevcut root guard/lock yolunu engellememek için conflict override; bunlar da aktif rezervasyon taşır. Registry sistem-geneli kilit değildir; tek GUI süreç kapsamı, yalnız listelenen helper operasyonları. CO stress süresi boyunca, Scene tüm restore zincirinde ve firmware/lighting/backup dahil diğer kaynaklarda ortak lease eksik. Dış süreç/daemon sahipliği ve whole-operation restore ownership kabulü açık.

## B01/B02 — CO evidence and decision timeline
- `cohistoryfields.h` exact CCD/slot/candidate P/F/I sayıları, invalid/duplicate exclusion, contradictory evidence. Qt adapter `cohistory.h` canonical boot/event UUID, isolated scope, per-core mode ve frozen topology membership; eski satırların izolasyonunu varsaymaz. Boş hücre stock stable değildir; excluded core, duplicate IDs ve yanlış topology yok sayılır.
- CO actual Search next/replay metadata: event UUID, selection UTC ms, boot, decision enum, previous pass/failure boundary, candidate offset, test süresi ve scope. Optional v3 dispatch metadata journal'a eklenir; crash guard signature/report ve write-before-dispatch sırası korunur. Reboot recovery interruption ayrı, verification/all-core evidence isolated sayılmaz. Çözülmemiş selection arşivde unresolved olarak gösterilir, gerçek pass sayılmaz. Compact journal 1 MiB reader cap üstü commit reddedilir; önceki güvenli disk kaydı korunur.
- Active snapshot ve archive üzerinden CCD Evidence and Decision Timeline: exact offset heatmap, metin+renk P/F/I, çelişki, 4096 newest event timeline, filter, frozen JSON detail. Live incremental monitor değil. Eski archive/session compare korunur. Ryzen profile diff recorded CCD/CCX/slot normalizasyonu, all-core ayrı, malformed/duplicate reddi; matching slot physical CPU proof değildir.
- Kalan: live SMT/V-Cache/online/fingerprint, cross-session evidence comparison, complete branch replay test, ETA acceptance, real Qt recovery/crash fixtures. Fiziksel CPU denemesi yapılmadı.

## D01 — Scene plan and trigger dry-run
- `sceneplanfields.h` ordered step selection shared by actual SceneEngine start and orderedPreview. Fan boost, power profile, firmware, CPU/GPU, optimization, lighting, keyboard, display, command, brightness steps retain previous callbacks/order and restore paths. Preview table current/requested/prerequisite/rollback; unknown when not known. Existing current dependency probe text preserved. No hardware write, no user command in preview. GUI preview not a visual node builder.
- Shared sourceAction for actual AC/battery decision and dry-run using saved auto configuration; pause/calibration/game ownership/busy/coalesce/queue/target cases. Hypothetical guards are not actual device state; no time/game trigger simulation, daemon persistence or full dependency graph. A05 scene-wide restore ownership still open.

## Test results
| Test | Result |
|---|---|
| Expanded independent runner set | 54/54 PASS |
| Final affected reruns (TR, scene static, new independent fixture) | 3/3 PASS |
| New pure C++ evidence/scene/registry | PASS: 49,152 scene plans and 64 trigger cases, evidence and claims fixtures |
| ASan + UBSan new fixture | PASS, exit 0 |
| GNU LTO + -Werror new fixture | PASS, exit 0 |
| Turkish catalogue | PASS, 89 new entries |
| Source added-line whitespace | PASS |
| Baseline Qt6 Debug/Release configure | FAIL — Qt6Config.cmake missing |
| apt update/install | DNS temporary failure / packages unavailable |
| Cargo | Unavailable, exit 127 |
| New Qt cohistory CTest | Added, not run |
| Full Qt build/CTest, Rust/Clippy/deny, Gentoo LTO/PGO, CI, real GUI sanitizer/visual, physical device tests | Not run |

Independent tests are not full native Qt/Rust acceptance. New Qt code not compiled here. No GitHub push, no physical tuning write. No original acceptance criteria removed. Other packages, including real MainWindow simulation A07/A08, remain open as in prior rounds.
