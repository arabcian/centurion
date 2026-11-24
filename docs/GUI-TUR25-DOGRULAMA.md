# Centurion Experimental — Tur 25 kısmi teslimat

## Baseline ve kapsam
Tur24 kısmi ZIP SHA-256 `84e3f84ff56badd48e89a5e00b88af9fd030b3cff3e5886de4d107de76b3eb8a`; 659 dosya; CRC PASS. Opus düzeltilmiş Tur22 soyunu korur. Bu tur 11 mevcut kaynak dosyası değişti, 5 yeni kaynak/test dosyası eklendi; kaynak silinmedi. Eski matrislerin tüm hücreleri korunarak 48 paket / 118 GUI ID / 21 FIX için Tur25 sütunları eklendi; ek ID'ler önceki paket satırlarında korunur. 0/48 tam kabul; 47 kısmi, D05 engelli. Tur24 baseline native derlemesi bu tur yeniden tamamlanamadı.

## A04 — CO Search ve helper yaşam döngüsü
- `start`, `verifyProfile`, `offerResume` CPU stress `info` kontrolü QProcess sinyallerine bağlandı. 10 s timeout, 64 KiB stdout, exit + JSON boolean power capability doğrulaması, Cancel ve kapatma koruması. Normal probe sırasındaki eski `waitForFinished(10000)` kaldırıldı. Recovery snapshot/guard/boot/topology probe sonrası tekrar doğrulanır. Recovery/reset/discard sırasında busy; stock throughput sırasında close ve cancel koruması, duplicate callback reddi, terminate→kill fallback. Launch öncesinde AC/game/recorded stock tekrar kontrol edilir. Crash guard algoritması, trial signature ve durable journal sırası değiştirilmedi.
- `privileged::run` stdout 4 MiB, stderr 4 KiB saklama sınırı ve timeout/oversize sonucu unknown; root kill reddedilse bile stream drain devam eder. GUI callback ile gerçek süreç sonlanması ayrıdır.
- Kalan: zorunlu destructor emergency reset/kill/wait/sleep korunuyor; tüm sync file/sysfs/IPC çağrıları ve post-reset scene restoration yaşam döngüsü, gerçek Qt 60 s fake helper/cancel/timeout/close testleri açık. Bu tur CO native GUI test çalıştırılmadı.

## A05 — gerçek süreç olayları ve kısmi kaynak koruması
- Yeni `operationregistry.h`: unique process ID, UTC event time, resource (helper basename), running/timeout/owner gone/exited, reported ok/failure/unknown/not started, 200 event retention. Timeout veya widget kaybında active kayıt gerçek process exit'e kadar korunur. MainWindow işlem merkezi ve mevcut quit guard gerçek helper durumlarını da kullanır. Optimization Apply/restore/boot/ABBA öncesi ve CO probe/resume için ilgili aktif helper kontrolü. Bu kontrol GUI süreç kapsamındadır; root helper'ların kendi kilitleri değişmedi.
- Kalan: bütün scene/restore/trial/backup/cooling/stress owner/resource graph, gerçek atomik read/write reservation, dış süreç root lock kapsamı ve restore tamamlanmasına kadar birleşik kaynak sahipliği; registry tüm QProcess akışlarını kapsamaz. Gerçek helper event testinin native Qt katmanı çalıştırılmadı.

## B07 — Optimization Transaction Journal
- `tunejournal.h` gerçek GUI `runOp` Apply, restore, restore_keys, restore_defaults, set_boot istekleri için dispatch öncesi UUID/boot/time/requested, gerçek callback sonrası structured per-key receipts ve reply durumu; crash halinde pending kayıt. 100 transaction / 4 MiB; QLockFile/QSaveFile, owner-only, corrupt/duplicate reddi; evidence dosyası tuning veya restore yetkilendirmesi değildir. Kaydedilemeyen günlük açık uyarı verir, güvenli restore işlemini engellemez. PGO training sırasında journal yok. Sources & Conflicts → Transaction Journal filtreli salt okunur tablo.
- Rust `tune-helper.rs` mevcut settle readback döngüsündeki beklenen/okunabilen eşleşen/okunamayan/uyuşmayan sayaçları bildirir. Yeni `tunereceipt.h` sınıflandırıcı, eski helper ya da eksik sayaçta doğrulanmış sonucu üretmez; skip/refused/mismatch/partial/failure ayrılır. Başarısız Apply veya restore artık tümü başarılı diye gösterilmez; boot-default receipt aggregate restore ile per-key apply ayrı. Root yazma/rollback/ABBA/boot guard algoritması değiştirilmedi.
- Kalan: ABBA ile gerçek transaction correlation, per-key restore receipt (mevcut helper aggregate), scene/external/boot history, semantic live/boot/preset diff, stable per-device identity ve column preferences. Kalıcı history native Qt fixture eklendi ama çalıştırılmadı. Rust yeni readback sayaçları derlenmedi.

## D04 ve testler
46 yeni TR katalog girdisi. Önceki FIX-01–21 açık, genel çeviri ilerlemesi tam kabul yerine geçmez.

| Test | Sonuç |
|---|---|
| Önceki bağımsız runner seti | 50/50 PASS (ilk regresyon) |
| Genişletilmiş runner seti, önceki final aday | 51/51 PASS |
| Son dondurulmuş kaynak runner | 51/51 PASS; hedef 51, eksik sonuçlar çalıştırılmamış/bitmemiş kabul edilir |
| Registry/receipt bağımsız ASan + UBSan | PASS (EXIT=0) |
| Registry/receipt GNU LTO + -Werror | PASS (EXIT=0; Gentoo tam uygulama değil) |
| Türkçe katalog / stage1 / algorithm guardrails | PASS (son seçili rerun) |
| Kaynak diff whitespace | PASS |
| Qt6 Debug / Release configure | FAIL — Qt6Config.cmake yok |
| apt update | DNS başarısız; apt exit 0 eski/boş indeksleri kullandığı için başarı değil |
| apt install simulation | FAIL — paket bulunamadı |
| Cargo test --locked | FAIL — cargo bulunamadı (EXIT=127) |
| Qt CTest (tunejournal dahil), gerçek GUI/sanitizer | Çalıştırılmadı |
| Rust build/test/clippy/deny, GitHub CI | Çalıştırılmadı |
| Gentoo uygulama LTO/PGO, fiziksel donanım | Çalıştırılmadı |

## Öncelikli kalanlar
A02 gerçek iki shell/cache/rendered contrast; A03 tam typed telemetry subscription ve Health/stress/SMART; A04 destructor/restore lifecycle; A05 tam atomic resource registry; A07/A08 gerçek MainWindow injected simulation ve reviewed baselines; B03 GPU UUID/overlay/history ve Rust doğrulaması; B07 yukarıdaki kalanlar; B09 calibration session/validity/rollback; B11 same-context ölçümlü game A/B ve run history. Diğer tüm açık işler özgün matrislerde. Bu teslimat tam kabul değildir. GitHub push yapılmadı. Native Qt/Rust derlemesi ve güvenli fixture doğrulaması olmadan üretim için önerilmez.
