# Centurion Experimental — Tur 33 kısmi doğrulama ve devir

Temel: `Centurion-experimental-tur32-kismi.zip` (SHA-256 `33fea8fefda70b5bd9ae6eb0356a693381cd97d3c28e0922594f8bccfea52180`). Kabul: **0/48 tam, 47 kısmi, D05 engelli.** Özgün kapsam, Tur29 özel istekleri ve build kriteri korunur.

## Gerçek kaynak değişiklikleri

- A10: Experimental Home Hardware & Firmware / Live Sensors alt sekmeleri bağımsız görünürlük tercihleri; ikisi birden gizlenemez, reset ve mevcut panel tercihleri korunur.
- B08: V2 Optimization draft, sabit helper hedefleri ve benzersiz WWID/namespace ile doğrulanmış NVMe queue hedefleri; V1 VM/scheduler okuma uyumluluğu, fresh readback, kimlik değişiminde unchecked/touched temizleme, atomik kayıt, lock ve modal sırasında değişen dosya/live/boot/editor kontrolü.
- X05: Gerçek tune-helper boot çağrısı için root-owned /run receipt: pending/returned, boot UUID, zaman, preset, anahtar bazında gerçek yazma/readback sonucu. Boot Service Inspector salt okunur tablo ve eksik/çelişkili receipt kapsam ayrımı.
- D04: 39 yeni Türkçe katalog girdisi.
- D05: 60 bağımsız runner ilk tam set; 8 son etkilenen rerun. Yeni C++ coverage runner, Qt ve Rust fixture kaynakları.

### Sınırlar ve tasarım kararları

- Optimization v2 dosyası önceki dosya yolunu kullanır; eski v1 kayıtları okunur. V2 kimliği helper tarafından bildirilen hedef tanımının hash'idir. NVMe kimliği eksik veya birden fazla aygıtta aynı ise yeni cihaz taslağı kaydedilmez. Slot değişmişse otomatik remap yapılmaz. Taslak restore sonrası unchecked; doğrudan tuning yazması yok. Kimlik değişiminde mevcut editin düşmesi bilinçli güvenlik kararıdır. Uygulama anındaki hotplug yarışı çözülmüş sayılmaz. Eski helper ile yalnız VM/scheduler fallback; fresh, okunabilir, shadow edilmemiş satırlar. QLockFile + atomik kayıt, 1 MiB/512 key sınırı, inceleme sırasında byte snapshot kontrolü. Kullanıcıya ait dosya kontrolü ayrıcalıklı FD güvenliği değildir.
- Boot receipt yalnız mevcut boot'taki kaydedilmiş son tune-helper boot denemesini gösterir, servis enabled/active bilgisi ayrı kalır. /run/centurion/tune/boot-receipt.json, 1 MiB sınır, atomik root kaydı, ayrı nonblocking lock. Başarısız kayıt mevcut boot/restore güvenlik işlemini engellemez. Helper hiç çalışmazsa receipt yok; sonraki drift/rollback veya fiziksel durum doğrulaması değildir. Per-key readback sayıları, skipped/mismatch/unverified/missing ayrı; istenen değer helper sonuçta döndürürse görüntülenir. Ham helper stdout/stderr saklanmaz. Eski helper ve kayıt yokluğu başarı sayılmaz.
- Home iki detay sekmesi için Experimental tercihi ayrı; Power Profile ve Device zorunlu kalır. Gizli sekme mevcut görünürlük tabanlı sensör davranışını kullanır; EC/device polling tamamıyla duruyor iddiası yok.
- A09, B04, B05, B06, X04 ve diğer açık paketlerde bu tur ayrı kaynak ilerlemesi yok. Gerçek MainWindow simulation ve görsel kabul açık. GitHub push yok.

## Testler

| Test | Sonuç |
|---|---|
| Tur32 kaynak ZIP CRC | PASS |
| Tur32 tam baseline runner | Bu tur ayrı çalıştırılmadı |
| Tur33 ilk tam runner | 60/60 PASS |
| Son etkilenen rerun | 8/8 PASS |
| Boot receipt coverage | 10.000 rastgele fixture; normal, LTO, ASan/UBSan PASS |
| Türkçe katalog | PASS |
| Qt6 Debug/Release configure | FAIL: Qt6Config.cmake yok |
| Paket erişim kontrolü | DNS çözümleme başarısız; kurulum denenmedi |
| Rust build/test/clippy/deny | Cargo yok, çalıştırılmadı |
| Yeni Qt draftidentity CTest ve Rust identity/boot UUID testleri | Çalıştırılmadı |
| Gerçek GUI, Gentoo PGO/LTO, fiziksel donanım, CI | Çalıştırılmadı |

Bağımsız runner'lar statik ve native C++ karışımıdır. Son tam koşu son küçük düzeltmelerden önce, son etkilenen testler daha sonra çalıştırıldı. Qt/Rust derlemesi veya fiziksel güvenlik kabulü değildir.

## Kalan özgün kapsam

- A10: Diğer güvenli panel kombinasyonları, compact capability-aware rows, iki shell DPI ve polling davranış testleri açık.
- B08: F2FS/zram/SATA ve diğer dinamik scope kimlikleri, farklı slota taşınan NVMe taslaklarının güvenli eşleştirilmesi, gerçek hotplug/write-time güvenliği, native Qt/Rust restart fixture. Kimlik yalnızca taslak için kanıt, Apply yetkisi değil.
- X05: Native OpenRC/systemd ve helper crash/guard/no-service fixture; servis çağrılmadan atlanan boot guard receipt yok; geçmiş boot receipt veya daha sonraki rollback/drift doğrulaması yok. GUI-109 diğer birimler ve native roundtrip açık.
- D04: Tüm FIX native kabul, görsel ve dil denetimi açık.
- D05: Qt6/Cargo yok, Debug/Release configure başarısız; Qt/Rust/CTest/CI/Gentoo PGO/LTO/fiziksel test açık.
- Diğer 48 paket / 118 GUI / ek ID / FIX / Wanted kriterleri aynen açık. Özgün hücreler korunmuştur.
