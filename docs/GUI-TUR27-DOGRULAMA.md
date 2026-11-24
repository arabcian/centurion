# Centurion Experimental — Tur 27 kısmi teslimat

## Baseline ve kapsam
Tur26 ZIP SHA-256 `31c7ed1dbec8dea63a7210901d156367215d63272ab3ed80a5a889a2e1a2a869`, 681 dosya, CRC PASS. Opus düzeltilmiş Tur22 soyunu korur. 6 mevcut kaynak/konfigürasyon dosyası değiştirildi, 5 yeni kaynak/test dosyası eklendi, önceki hiçbir dosya silinmedi. Önceki bütün 48 paket, 118 GUI ID ve 21 FIX satır ve sütunları korunarak Tur27 sütunları eklendi. Ek ID'ler özgün paket satırlarında korunur. 0/48 tam kabul, 47 kısmi, D05 engelli. Tur26 baseline bağımsız runner sonucu önceki teslimata aittir; bu tur ayrıca tam baseline native derlemesi yapılmadı.

## B11 — Autotune Run History
- Optimize > Inspect > Autotune Run History: gerçek GUI run ID, boot ID, UTC zamanları, dispatch edilen request/weights/profile/mode, terminal exit, elapsed, gerçek stage timeline, helper JSON ve staging sonucu. Pending kayıt dispatch öncesinde. Timeout, failed start, nonzero/crash, invalid output, Explore ve değişen draft ayrılır. Başarı için existing normal exit + exit=0 + ok true ve preset/values object gerekir. Staging outcome donanım apply başarısı değildir. Eksik GUI row anahtarları ayrı metadata olarak saklanır.
- `autotunehistory.h` mevcut tunejournal atomic owner-only QSaveFile/QLockFile storage mekanizmasını ayrı user-local `autotune-runs-v1.json` dosyasında kullanır. 100 run / 4 MiB toplam üst sınır; 2 MiB üzeri tek kayıtta full report omitted işaretlenir ve sınırlı özet tutulur. SHA-256, parse edilmiş raporun compact JSON serileştirmesini gösterir, raw wire bytes hash'i değildir. Bozuk journal üzerine yazılmaz; başarısız disk kaydı timeline'da belirtilir. PGO train sırasında journal yazılmaz. Solver'ın kendi JSONL history, model/weight/ABBA algoritması ve mevcut apply/restore guard zincirleri korunur.
- Filtreli history table ve frozen JSON detail. Yalnız tamamlanmış, full report içeren başarılı solver run'ları read-only comparison için listelenir; request/weights/preset/profile/prefit evidence/constraints/decisions karşılaştırması; geçmiş raporlardan apply/stage/weight mutation yok. Report diff, otomatik benchmark veya fiziksel güvenlik onayı değildir.
- `runAutotune` stdout streamed accumulation 16 MiB cap, excess fail-closed. Qt process buffers transient size ve full native GUI responsiveness doğrulanmadı.

## B11 — Compare Frametime Logs
- Optimize > Inspect > Compare Frametime Logs. İki kullanıcı seçimi raw log, mevcut FLM/MangoHud/ms parser ve aynı mean/P50/P95/P99/FPS tanımları. Her log 16 MiB, minimum 300 accepted samples, en fazla %5 rejected (accepted/(accepted+rejected) >= 95%); SHA-256 ve accepted sample dizisi aynıysa karşılaştırma yok. Bağlam ve A/B config label zorunlu, label veya dosya değişince confirmation sıfırlanır.
- Gerçek A/B mean/percentile/FPS değerleri ve yüzde delta; malformed/short/duplicate/missing confirmation durumunda delta yok. Kullanıcının aynı oyun/workload/resolution/graphics/kernel/firmware/power koşullarını beyan etmesi gerekir; bu, backend tarafından doğrulanmış controlled experiment değildir. FPS = 1000 / mean ms. Significance/CI veya prediction utility'den FPS türetilmez. No calibration import, no hardware write; mevcut field pairing ve ABBA akışına müdahale yok.
- Kalan: gerçek kontrollü deney yürütme (repeated blocks/restore), otomatik game/session/context identity, persist/export/graph of measured pair, uncertainty and recommendation dependency/revalidation, gerçek native GUI fixture. Mevcut ABBA/field history korunur.

## B07 — Semantic Live / Boot / Preset Diff
- Optimization Sources & Conflicts > Live / Boot / Preset Diff. Frozen last helper rows, saved boot values, preset values ve checked editor subset; union by stable setting key, no hardware refresh/write. Integer canonicalization `std::from_chars` exact signed 64-bit, Qt JSON integer conversion without double rounding; bool exact 0/1, choice exact strings, unknown/missing/stale/invalid ayrı; role alias mapping captured topology only, unresolved role unknown. Boot saved ≠ applied, missing ≠ reset. Unsupported/unavailable and coverage status separate; current actual options/range validity not full validated.
- Text, checked-draft-only, hide verified equal combined filters; critical key/relation columns cannot be hidden, optional columns, width/order/visibility persisted per shell with existing validated workspace state and reset. Existing generic preset comparator, journal, ABBA, boot guard, apply/restore unchanged. No merge/replace action. Remaining: stable device lifetime/UUID, per-device capability and semantic checked draft relation, real provenance and all tuning table preferences, per-key restore/ABBA receipts, merge/replace workflow and full acceptance tests.

## B09 — Calibration session evidence
- Existing v10 viewer Measurement Sessions now has frozen group selection/filter and details of actual rows, configuration, context descriptors, same-session baseline candidates clearly separated from exact match. File read errors rejected. Existing coverage/compatibility/predcheck/ABBA/field tabs retained.
- Rust `autotune_req` reports the exact descriptor passed to `prefit` and loaded signature fingerprint/kernel/row count/newest. `prefit_context` describes prefit, not every solver phase or a verified controlled run. Loaded=false is no loaded signature, not a diagnosis of why. Solver weight calculation and measurement/validation decisions unchanged. Full schema/validity criteria and safe root signature import/rollback with mutation guards not implemented. GUI does not write signatures.

## Test results
| Test | Result |
|---|---|
| First expanded independent runner set | 53/53 PASS |
| Final independent runner set | 53/53 PASS (target 53; missing results not run/completed) |
| Semantic and measured-pair independent C++ fixture | PASS; 100,000 exact integer comparisons plus INT64/invalid/missing/quality/delta cases |
| ASan + UBSan for comparison fixture | PASS (exit 0) |
| GNU LTO + -Werror for comparison fixture | PASS (exit 0) |
| Turkish catalogue | PASS after fixing literal percent placeholder issue |
| Source added-line whitespace check | PASS |
| Qt6 Debug/Release configure | FAIL — Qt6Config.cmake unavailable |
| apt update | FAIL / timed out; DNS temporary failure |
| apt package installation simulation | FAIL — packages not available |
| Cargo workspace locked test | FAIL — cargo unavailable (exit 127) |
| New Qt autotunehistory CTest | Added; not run |
| Full Qt build/CTest, Rust build/Clippy/deny, Gentoo LTO/PGO, GUI sanitizer/visual/physical and CI | Not run |

62 new TR catalogue entries; original FIX-01–21 acceptance remains open. No GitHub push. New Qt and Rust source is not natively verified; not production acceptance. No physical test/hardware writes were performed. Only the stated source changes are new; other packages, including A07/A08, remain at Tur26 state. Original scope unchanged.
