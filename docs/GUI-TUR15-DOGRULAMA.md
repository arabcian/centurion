# Centurion Experimental — Tur 15 kısmi teslimat

**Temel:** Tur 14 ZIP SHA-256 `6d50183ae95f4a72b54fa7cf10ec9b3701147acac7a73ca681930c7f488b3235`. İki rapor aynı SHA-256 değerinde; ZIP CRC PASS.

**Durum:** 48 paket / 118 GUI ID korunmuştur. 0 paket ve 0 ID tam kabul; 47 paket kısmi, D05 engelli. Tur 15 alt davranış ilerlemesi A01/A02/A04/B02/C05/D04; D05 doğrulama denemeleri. Diğer 41 pakette yeni davranış yok; kaynakta tüm 48 paketin çağrı zinciri yeniden denetlenmedi. Bu bir tamamlanmış release veya kapsamın azaltılması değildir. Orijinal kabul sütunları aynen tutulur. GUI-47, GUI-60, GUI-17 gibi 118 dışı ek ID ve FIX-01–21 paket matrisinde tutulmuştur.

## Gerçek değişiklikler ve açık sınırlar

### A01
- Diff: theme.cpp:setTokens / validCustomPalette; 19×7 ASCII token atomik kontrol; memcpy kaldırıldı. 57 palet kontrastı yeniden ölçüldü.
- Test: palette audit: 3990 check, 737 BELOW_TARGET, 57 EXEMPT; theme-runtime test genişletildi, Qt BLOCKED.
- Kalan: 737 düşük kontrast düzeltmesi/istisnası, semantic/density/OLED/accessibility ve rendered testler halen açık.

### A02
- Diff: theme.cpp:applyLivePalette artık setTokens atomik validation sonucunu kontrol ediyor; 20 token adresi geçiş fixture eklendi. Normal Home restart yolu incelendi, değiştirilmedi.
- Test: theme-runtime Qt BLOCKED; 20 geçiş testi normal shell kabulü değildir.
- Kalan: Normal Home canlı tema, local stylesheet/icon/brush/chart cache yenileme, gerçek 20 geçiş no-write kabulü açık.

### A04
- Diff: CO trialGeneration ile launch kernel baseline, watcher, delayed debounce/validation/step ve test process callback ayrımı; blockingReset normal exit ve exit=0 ister; privileged::run crash/nonzero success/oversize sonucu unknown.
- Test: CO native reboot/walk PASS; mevcut guardrail/security runner PASS; yeni Qt callback ve helper acceptance BLOCKED.
- Kalan: toolsPresent ve destructor sync beklemeleri, resume/discard lifecycle, merkezi cancel/restore/timeout, bütün GUI producer ve 60 s fake helper testi açık.

### B02
- Diff: Search::remainingIfPass gerçek next/report kopyasında iki koşullu workload; GUI current planned load, başarılı testlerden ölçülen overhead, 2.5 s debounce, unknown retry/restore; reset doğrulanırsa arama yükü 0.
- Test: co_walk/co_reboot native PASS (mevcut); costrategy_test iki senaryo ve save() eşitliği eklendi, Qt BLOCKED.
- Kalan: Tam branch reason, gerçek event timeline, replay/crash guard Qt acceptance ve öngörülen aralık için gerçek run ölçümü açık; senaryolar min/max garanti değildir.

### C05
- Diff: edidfields::progressiveTimings; base/CTA progressive DTD, trusted capture; explicit VRR bandı mevcut decoder; offset/hex cursor/table sync, 4096 cap fail-closed ve kısa EDID inspector erişimi.
- Test: edidfields_native_test.py PASS: 8192 CTA kombinasyon + 30000 random capture + progressive/invalid/interlaced; Debug/O2/ASan+UBSan. Qt BLOCKED.
- Kalan: Gerçek Qt click/keyboard/connector disconnect/resize fixture, DisplayID/interlaced ayrıntıları, optional raw export/privacy ve compositor kapsamı açık.

### D04
- Diff: LTO memcpy kaynak düzeltmesi; 18 TR mesajı; mevcut çalışma yollarına lokal değişiklikler.
- Test: 42/42 Python runner PASS; TR catalogue PASS; native Gentoo LTO/Qt BLOCKED.
- Kalan: FIX-01–21 gerçek kabul, bütün UI TR/EN/DPI ve warning doğrulaması açık.

### D05
- Diff: SHA/CRC doğrulandı; native ortam kurulum ve başlangıç/son configure denendi; 42 runner ve contrast audit yürütüldü.
- Test: Qt6Config.cmake yok; Cargo/rustc yok; apt DNS / index failure; Qt Debug/Release/Rust BLOCKED; CI/physical NOT RUN.
- Kalan: Tüm native/CI/Gentoo PGO/visual/hardware acceptance ve conditional experimental push açık.

## Test ayrımı
- 42/42 mevcut Python runner son kaynakla PASS, skip/fail yok. Bazıları bağımsız C++ derleyip gerçek matematik/parser fixture çalıştırıyor, diğerleri statik/fixture. EDID yeni fixture üç derleme modunda başarılı. Yeni `theme_runtime_test.cpp` ve `costrategy_test.cpp` Qt gerektirir; çalıştırılmadı.
- A01 token kontrastı: 57 palet × 70 = 3990; 737 eşik altı, 57 exempt. Bunlar düzeltilmiş değildir. Rendered DPI/kontrast testleri değildir.
- Qt 6.4+ Debug sanitizer ve Release: başlangıç ve son `cmake` configure, `Qt6Config.cmake` bulunamadığından BLOCKED. Derleme/CTest yapılmadı. Kullanıcının Qt 6.11.3 Gentoo LTO/PGO warning'i için kaynak düzeltildi fakat warning'in kesin kaybolduğu doğrulanmadı.
- Rust cargo/rustc PATH ve aranan dizinlerde bulunamadı. `cargo test --workspace --locked` ve `cargo clippy ...` komutları command not found; cargo-deny, gerçek CI, visual baseline, donanım kabulü yapılmadı. `apt-get update` DNS; install paketler bulunamadı. Loglar ayrı kanıt ZIP'inde.
- Fiziksel donanıma write, pkexec, stress, fan/SMU/HID değiştirme testi yapılmadı. CO gerçek crash guard/next/report/journal sırası, NVcurve/fan/ABBA/restore kaynakları değiştirilmedi. Merkezi safety davranışlarının tamamı test edilmedi. Yeni root helper exit doğrulamasının gerçek helper entegrasyon testi açık.
- GitHub push yapılmadı; experimental yayına kabul kapısı kapalı, main/old dokunulmadı.

## Kalan iş takibi
- `GUI-TUR15-48-PAKET.tsv`: 48 paket ve özgün kabul kriterleri, Tur 15 diff/test/sınır.
- `GUI-TUR15-118-KABUL.tsv`: 118 özgün ID, kalan iş ve testleri; hiçbiri tam kabul değil.
- `GUI-TUR14-BIRLESIK-DEVIR.md`: bütün özgün kapsam ve güvenlik sözleşmesi.
- `GUI-TUR15-RUNNERS.tsv`: 42 runner sonucu.
- `GUI-TUR15-PALETTE-CONTRAST.csv`: güncel token audit.

## Değişen kaynak/test dosyaları
- `gui/i18n/centurion_tr.ts`
- `gui/src/cosearchdialog.cpp`
- `gui/src/cosearchdialog.h`
- `gui/src/costrategy.cpp`
- `gui/src/costrategy.h`
- `gui/src/diagnostictools.cpp`
- `gui/src/diagreportview.cpp`
- `gui/src/edidfields.h`
- `gui/src/privileged.cpp`
- `gui/src/theme.cpp`
- `gui/tests/costrategy_test.cpp`
- `gui/tests/edidfields_native_test.py`
- `gui/tests/theme_runtime_test.cpp`

13 mevcut dosya değişti, özgün 595 dosyadan hiçbiri silinmedi. Yeni dokümantasyon dışında paket dışı derleme/test cache dosyaları dahil edilmedi.
