# Centurion Experimental — Tur 38 doğrulama raporu

**KISMİ teslimat.** T37 SHA-256 `f7e5c6fb30d03874e7cf87626d26c8673ab84cf1b10b42e096b486ba0d0a46ba`; 10 mevcut dosya değişti, 1 yeni test dosyası, 0 silinen (rapor/matris hariç). Temiz T37 üzerinde patch uygulaması ve değişen dosya byte eşitliği PASS. 0/48 tam kabul, 47 kısmi, D05 engelli; 118 GUI ID, 21 FIX, WANTED 1–11 + BUILD açık. T37 SMART değişiklikleri ve build düzeltmeleri korunuyor.

## Gerçek kaynak ilerlemesi

- A04/D01: Scene ana işleminde doğrudan root çağrılarının ve ortak helper yolunun `runUntilExit` kullanması, raw JSON array için overload, timeout sonrası gerçek çıkışa kadar callback/rollback gecikmesi. Timeout sonrası helper OK dönse bile Unknown. `prune` game cleanup eski recovery override yolunda bırakıldı; tüm Scene/game akışları terminal-safe değildir.
- A04/D02: Direct lighting process stdout 4 MiB sınırı, stderr drain, gerçek exit öncesi timeout callback yok, crash/invalid reply/nonzero success ret; yetki fallback'i de terminal exit bekler. Kullanıcı tabı dahil aynı helper yolunda geç callback davranışı değişti. D-state süreç çıkmazsa işlem meşgul kalabilir; UI thread beklemez, native doğrulama yok. HID yazma test edilmedi.
- A05/D01: CO finalizing flag, sonuç düzenleme ve pencere kapatma/yeniden başlatma engeli; önceki Scene restore için `applyTracked` (idle-only, pending/game/missing ret) ve doğrudan instance callback. CPU lease Scene'in sadece CPU adımlarına parametre olarak iletilir; tray çağrıları ödünç almaz. Ana transaction terminal signal sırasında yeni apply reentrant overwrite yapmaz, normal pending queue korunur. Scene ana outcome (returned_unverified, partial, rollback, interrupted, deferred) ayrı raporlanır; **deferred GPU işi lease kapsamına dahil değildir**. CO başlatma esnasında Scene aktifse reddedilir; initial Scene name-based callback ve bütün oturumu kapsayan ortak kaynak kilidi açık. Restore başlayamazsa uyarı verip oturumu serbest bırakır; kullanıcı Scene'i manuel kontrol etmeli. Game owner görülürse tracked restore kalan adımları/rollback'i keser; ayrı process race guard değildir.
- D01: Soft no-write failure rollback listesine kalıcı eklenmez. Firmware bilinmeyen alan ve kapalı GPU WMI, missing keyboard için incomplete/skipped; power-profile boş effective değerinde istenen değer uydurulmaz, mevcut readback aranır, undo eksik/mismatch failure. Fan boost missing bool ret. Display/brightness subprocess exit status/code kontrolü. Mevcut green active Scene UI politikasına dokunulmadı. Power/fan/firmware readback ve rollback bütün kapsamı tamamlanmadı.
- D04: Üç yeni TR metin, CO status PlainText. Tüm FIX metinleri bireysel mevcut değil; kabul uydurulmadı.

## Testler

| Kontrol | Sonuç |
|---|---|
| T37 SHA/CRC | PASS |
| Patch temiz T37 uygulama + byte eşitliği | PASS |
| Final bağımsız Python runner seti | 67/67 PASS |
| operationlease test, GNU LTO -Werror | PASS |
| operationlease test, ASan + UBSan | PASS |
| Yeni static source contract | PASS (Qt execution değil) |
| Qt privileged lifecycle raw-array fixture | YAZILDI; Qt yok, çalıştırılmadı |
| apt-get update | exit 0 ancak deb.debian.org DNS failed |
| apt-get install qt6-base-dev qt6-svg-dev qt6-tools-dev cargo rustc | exit 100; paketler bulunamadı |
| Qt Debug ve Release configure | exit 1; Qt6Config.cmake bulunamadı |
| cargo build --locked | exit 127; cargo yok |
| Gerçek Qt build/CTest, Rust test/clippy/deny, Gentoo PGO/LTO, CI/visual/physical | ÇALIŞTIRILMADI |
| GitHub experimental push | YAPILMADI |

Runner başarısızlıkları: yok. Bağımsız runner sayısı gerçek Qt CTest sayısı değildir. Test logları kanıt ZIP içindedir. Gerçek donanım yazma, stres, reboot veya restore çalıştırılmadı. T37 öncesi raporlar tarihsel kayıt, bu tur test kanıtı değil.

## Açık güvenlik sınırları

CO blocking destructor acil stock reset fallback aynen korunuyor; async destructor dönüşümü henüz yapılmadı. CPU session lease yalnız GUI process; helper lock ve emergency reset override değişmedi. Scene tüm kaynaklarını atomik reserve etmiyor; deferred GPU/initial Scene/ABBA/Trial ve cross-process restore ownership açık. D05/A07/A08, B03/B07/B09/B11 ve diğer tüm özgün kriterler açık. Tam kabul iddiası yok.
