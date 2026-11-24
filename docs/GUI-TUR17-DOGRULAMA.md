# Centurion Experimental — Tur 17 kısmi geliştirme ve devir raporu

**Temel:** `Centurion-experimental-tur16-kismi.zip` — SHA-256 `249734edf068fe98b378acaf54f25e31f72a7e5ad55eefc64c97374168c3bcb7`. Input CRC PASS. Orijinal Tur 14 devir raporu ve Tur 15–16 matrisleri değişmeden korunmuştur.

**Kabul durumu:** 48 özgün paket / 118 özgün GUI ID; **0 tam kabul, 47 kısmi, D05 doğrulama engelli**. Tur 17 A01, A02 ve D04 alt davranışları ilerletildi, D05 doğrulaması tekrar denendi. 48 paketin tamamı için çağrı zinciri incelemesi henüz tamamlanmadı. 118 ID listesi dışındaki paket ID ve FIX kapsamı 48 paket matrisinde aynen korunur. Yeni ID oluşturulmadı. ID satırlarında yalnız açıkça ilgili yeni davranış yazılmıştır; paket düzeyindeki ilerleme bütün ID'lerin ilerlediği anlamına gelmez.

## A01 — Kontrast ve özel palet düzeltme
- Önce: 57 palet, 3990 opak token kontrolü, 737 below-target (56 palette), 57 disabled muafiyeti. Sonra: 3933 PASS, 57 muaf, 0 below-target. Bağımsız Python audit gerçek theme.cpp paletlerini okur.
- 287 sorunlu ön plan token'ı en küçük RGB kanal kare mesafesiyle siyah/beyaz doğrusu boyunca iyileştirildi. Surface BG0–BG4/WELL, BORDER/BORDER_SOFT ve DANGER_SOFT, palette ID/name/order değişmedi; başarısı olan token değişmedi. Ön plan görünümünde değişiklik vardır; bu değerler görsel kimliği korumak için hue yönünü koruyan karışımdır, algısal optimum veya onaylı görsel baseline değildir. Graphite varsayılan header token'ları senkronize edildi.
- `palettecontrast.h`: Qt'den bağımsız deterministik algoritma, siyah/beyaz rayında 8-bit adaylar; 4.5:1 normal text, 3:1 soft accent chart; opak sRGB, mümkün değilse tüm draft reddedilir. `theme::repairPaletteContrast` geçersiz inputu reddeder ve çıkışı mevcut audit ile doğrular. Özel palet editöründe explicit Improve Token Contrast düğmesi; yalnız taslak değiştirilir, Save olmadan tercih veya aktif tema değişmez. Karışık yüzeylerde düzeltme bulunamazsa yüzeylere müdahale edilmez. Bu algoritma tüm RGB uzayında optimal arama değildir.
- Kalan: gerçek rendered hover/focus/disabled/error/chart/custom, alpha ve gradient kontrast, palette bağımsız OLED seçenekleri, colorblind/desen/ikon, ortak semantic typography/spacing/density/table API, tüm DPI ve erişilebilirlik. 737 opak çift düzeltildi; A01 tam kabul edilmedi.

## A02 — Canlı tema ve gerçek shell cache migration
- Kaynak doğrulaması: `profileAccent()` önceki statik `QHash<QString, QString>` ile ilk çağrıdaki renkleri sonraki revizyonlarda da döndürüyordu. Artık geçerli token ve sabit profil renkleri yeniden hesaplanır.
- `theme::bindPresentation` receiver-lifetime bağlı bir kez kaydedilen sunum callback'i; gerçek Home profil butonları (icon/gradient/QSS), Health badge, guard/pause banner, experimental MainWindow rail/scene/search icons ile bağlandı. MainWindow profile chip ve gradient yalnız önceden saklanmış `profileId` kullanılarak yenilenir; tema sinyalinde pp watcher veya scene ayarı yeniden sorgulanmaz. Sayfa/selection/işlem sıfırlanması amaçlanmadı, ancak gerçek shell fixture çalıştırılmadı.
- `applyLivePalette`: mevcut atomik doğrulama korunur; nested apply engellenir, senkron palette/sheet/callback/update scheduling süresi nanos cinsinden tutulur (deferred repaint süresi dahil değil). Mevcut izole simulation çağrısı korunur.
- Normal Home tema seçici, gallery ve custom save **restart kullanmaya devam ediyor**; diğer sayfalarda eski QSS/model brush/rich text/pixmap kalabildiği için normal live seçiciyi etkinleştirmek yanlış güvenlik rengi riski taşır. Bu karar tamamlanma iddiası değildir. Kalan cache migration, lazy lifecycle, gerçek shell 20 geçiş selection/scroll/undo/aktif işlem/no-write ve visual regression açık.

## D04/D05 — Test ve derleme
- 3 yeni Türkçe çeviri. Önceki theme.cpp memcpy warning düzeltmesi korunuyor.
- 44/44 bağımsız Python runner PASS (önceki 43 + palettecontrast native). Yeni native test Debug, O2, ASan/UBSan altında 57 gerçek palet, her modda 400 özel palet, imkansız karışık yüzey, determinism/idempotence, input mutation yok, minimal aday displacement. CI tanımına yeni runner eklendi; uzaktaki CI çalıştırılmadı.
- Qt offscreen theme-runtime testine üretim palette repair, gerçek 57 palette audit, 20 live apply, draft/selection, nested apply reject, settings byte eşitliği, receiver deletion eklendi. Qt6 geliştirme paketleri bulunmadığı için bu yeni test **çalıştırılmadı**. Qt6 Debug/Release configure BLOCKED; apt DNS/indeks hatası, cargo/rustc yok. Native GUI/Rust/CTest/LTO/PGO/CI/fiziksel donanım kabulü yapılmadı.
- Bu tur CO/NVIDIA/firmware/helper/Rust donanım güvenlik kaynakları değiştirilmedi; fiziksel güvenlik doğrulaması yapılmadı. GitHub push yok; main/old dokunulmadı.

## Değişiklik envanteri
9 mevcut kaynak/test/CI dosyası değişti, 2 yeni kaynak/test dosyası; hiçbir kaynak silinmedi.
- `.github/workflows/ci.yml`
- `gui/CMakeLists.txt`
- `gui/i18n/centurion_tr.ts`
- `gui/src/hometab.cpp`
- `gui/src/mainwindow.cpp`
- `gui/src/mainwindow.h`
- `gui/src/theme.cpp`
- `gui/src/theme.h`
- `gui/tests/theme_runtime_test.cpp`
- `gui/src/palettecontrast.h`
- `gui/tests/palettecontrast_native_test.py`

## Tam kapsam takibi
- `GUI-TUR17-48-PAKET.tsv`: 48 paket, orijinal kapsam/kabul sütunları ve bütün önceki kayıtlar, Tur17 farkları ve kalanlar.
- `GUI-TUR17-118-KABUL.tsv`: 118 özgün GUI ID, eski sütunlar değiştirilmedi; 25/26/30 ilgili yeni ilerleme, diğer satırlarda ayrı ID davranışı doğrulanmadı.
- `GUI-TUR17-RUNNERS.tsv`, `GUI-TUR17-PALETTE-BEFORE.csv`, `GUI-TUR17-PALETTE-AFTER.csv`, `GUI-TUR17-PALETTE-CHANGES.csv`: gerçek ölçümler.
- Kanıt ZIP: matrisler, rapor, patch, ham test/configure/apt logları, kontrast CSV'leri.

**Sonuç:** Kısmi kaynak teslimatı. Tüm 48 paket kabul edilmedi, normal live theme henüz etkin değil. 737 tarihsel opak token hatası düzeltildi; rendered kontrast ve Qt davranış kabulü yapılmadı.
