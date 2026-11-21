# Centurion 36 Themes + 25 Diagnostics — Derin Hata ve Mantık Denetimi

**Kaynak:** `centurion-main-36-themes-25-diagnostics.zip` (önceki teslimat)
**Tarih:** 21 Kasım 2026  
**Amaç:** Bu oturumdaki 36 tema + H02/H03/H04/H05/H09/S01/S03/S06 + H01/H06/H10/H12/H13/H15/H16/H17/H18/S04/S07/S08/S10/S11/S13/S15/S18.

## Doğrulama sınırı

Kaynak ağacı ve veri yolları/GUI yönlendirmeleri incelendi; gerçek sysfs/log formatlarından oluşturulan regresyon fixture'ları eklendi. Bu çalışma ortamında **Qt6 6.4+ geliştirme bileşenleri** ve **Rust/Cargo** bulunmadığı için C++/Rust binary'si üretilemedi; yeni C++/Rust fixture'ları burada çalıştırılamadı. Statik testlerin geçmesi çalışma zamanı ve gerçek donanım güvenliğinin kanıtı değildir. CMake denemesi `find_package(Qt6 6.4 REQUIRED ...)` aşamasında Qt6Config.cmake bulunamadığı için durdu. Hiçbir donanım üzerinde stres testi, CO ayar değişikliği, GPU wake-up veya EFI/ACPI yazma işlemi yapılmadı.

## Düzeltilen kusurlar (öncelik sırasıyla)

| Önem | Kod | Hata / etkisi | Yapılan düzeltme / konum |
|---|---|---|---|
| **Yüksek** | H03 | `TOTAL_ERR_COR: 14` satırı Rust `strip_prefix(...).trim().parse()` ile sayı olamıyor; AER sayaçları Unknown kalıyordu. | `crates/centurion-helpers/src/health.rs`: `aer_total()` etiket + `:` + `u64` ayrıştırması; 0, bozuk ve overflow fixture'ı. |
| **Yüksek** | H03 | Aynı PCI BDF'ye başka cihaz geldiğinde sayılar yanlış karşılaştırılabiliyordu. | AER örneklerine `vendor/device` eklendi; `gui/src/healthevidence.cpp` deltalardan önce ikisini doğruluyor. Eski şema örneklerinde delta `Unknown`. |
| **Yüksek** | H17 | Olay değiştirilince kayıtlı durum/not editöre yüklenmiyor; sonraki Save notu siliyordu. | `gui/src/advancedhealth.cpp::selectIncident()` mevcut özel kaydı yükler; `advanceddiagnostics::incidentNote()` eklendi. |
| **Yüksek** | H06 | En çok 8 NVMe denetleyicisinin iki ayrı 4 saniyelik sorgusu arayüzün 30s timeout'unu aşabiliyordu. | Yalnızca H06 için GUI helper timeout'u 90 saniyeye çıkarıldı. `health_nvme.rs` stdout'u child çalışırken okuyup 48KiB ile sınırlar; kök komutlar read-only. |
| **Yüksek** | S04 | Çok satırlı Linux AER sysfs dökümü düz tamsayı gibi okunuyordu. | `gui/src/advanceddiagnostics.cpp::aerTotal()` ile `TOTAL_ERR_*` satırları ayrıştırıldı; vendor/device eşleştirme ve boot sınırı. |
| **Orta** | H15 | Flight Recorder 30/60s aralıkla örnek alırken `>15s` normal aralıklar kayıp sayılıyordu. | `boothist.rs` flight interval gönderir; `flightCoverage()` kaydedilmiş aralık ve gözlenen medyandan konservatif eşik üretir, değişken cadence belirsizliğini açıklar. |
| **Orta** | H12 | Kayıtları sadece yan yana sıralıyor ve aynı olay çift günlükte sayılabiliyordu. | `sleepMatrix()` boot/seq bazlı deduplikasyon ve boot başına uyku/D3cold/USB reset/failure-hint özetleri sağlar; uyanma nedeni kesin ilan edilmez. |
| **Orta** | H13 | Bozuk/erişilemeyen CO crash guard veya recovery session dosyası sıfır aday gibi okunabiliyordu. | `coCrossLink()` koruma durumunu `UNKNOWN` uyarısıyla gösterir; pending trial ve boot sınırını raporlar; **crash guard değerlerine dokunmaz**. |
| **Orta** | H16 | NVIDIA ECC sorgusunda timeout/başarısız çalışma kısmi çıktıyla karışabiliyordu. | Qt işlemindeki başarısız sonlandırma ayrı işaretlenir; sadece explicit butonla, en fazla 5s aktif sorgu; başarısızlık `Unknown`. |
| **Orta** | S18 | `zcat` alt süreç stdout'u çalışırken boşaltılmadığından büyük config çıktısında pipe tıkanabiliyordu. | `kernelDependencies()` stdout'u bekleme esnasında boşaltır, 2MiB/2.5s sınırı getirir. |
| **Orta** | S15 | `wpctl/pactl` ana GUI thread'inde eşzamanlı bekleniyor, takılmaya yol açıyordu. | Komutlar `ToolPage`'in var olan asenkron QProcess sıralamasına taşındı. |
| **Orta** | S07 | Bluetooth aygıtının yalnızca USB device class'ına bakılması composite BT aygıtını kaçırabiliyordu. | `radioDoctor()` ilgili USB arayüzlerinin `bInterfaceClass=e0` metadata'sını da inceler. |
| **Orta** | H01 | İki tamamlanmış temiz boot yeni-eski yönü net ifade edilmiyordu. | `compareBoots()` karşılaştırma başlıkları açıklığa kavuşturuldu. |
| **Orta** | H17/H03/S01 | Gizli tanı verilerinin dosya izinleri bazı yerlerde commit *sonrasında* kısıtlanıyordu. | `QSaveFile::setPermissions(ReadOwner|WriteOwner)` commit öncesinde yapılır; kayıt atomik. |
| **Orta** | Health UI | Tarama sırasında aktif tanı değiştirilirse geç gelen yanıt başka tanı altına yazılabiliyordu. | Başlatılan tarama tamamlanana kadar feature chooser devre dışı. |

## Özellik bazında mevcut kapsam ve kalan sınır

| Özellik | İnceleme sonucu | Kalan sınır / test ihtiyacı |
|---|---|---|
| **36 tema** | Kimlikler, önizleme ve kontrast statik testte korunuyor. | Gerçek Qt6 ekran görüntüsü, farklı ölçeklerde kontrast ve çeviri kontrolü gerekli. |
| H02 Ryzen MCE Atlas | CPU/APIC, MCA bank, L3 alanlarını tahminsiz eşliyor. | Logda CPU yoksa core/CCD kesinleşmez. Fiziksel AMD CCD dizini yalnızca L3'ten ispatlanamaz. |
| H03 AER Delta | **İki doğrulanmış hata düzeltildi**; 0/Unknown/reset ve aynı-boot kuralı. | Gerçek sysfs ve hotplug üzerinde Rust/Qt bütünleştirme testi gerekli. |
| H04 Xid Cases | Aynı-GPU Xid ve zaman penceresi kullanılıyor. | AER/GSP/d3cold olaylarının GPU kimliği doğrulanmamış olabilir; nedensellik çıkartılmaz. |
| H05 F2FS Guard | Normal mount/GC mesajları yerine explicit hata sınıflandırması var; yazma yok. | Tarama aralığında log yoksa bütünlüğe garanti verilemez. |
| H09 Wi-Fi/BT Ledger | Firmware timeout/reload/HCI mesajları sınıflandırılıyor. | Sadece tutulan loglar; tüm sürücülerin bütün hata kodları kapsanmaz. |
| S01 Hardware Diff | User opt-in baseline, allowlist komut satırı, PCI/sürücü/firmware anlık farkı. | Boot parametre allowlist'i ve sysfs erişimi Gentoo üzerinde gözden geçirilmeli. |
| S03 EDID/VRR | Blok sınırları/checksum/timing ve DRM export okunuyor. | EDID min/max Hz VRR garantisi değildir; Nvidia/Wayland VRR etkinliği ayrıca doğrulanmalı. |
| S06 NVIDIA Module Diff | Konfig ve runtime ayrımı; yorum satırını option saymıyor. | Alias, config dosyası önceliği ve parametre normalizasyonu her sürücüde bire bir değildir. |
| H01 Boot Regression | Temiz boot ve yeterli süre koşulu; eski context yoksa Unknown. | Bir kernel sürümünün sorun nedeni olduğu tek başına kanıtlanamaz. |
| H06 NVMe Deep | Bounded root helper, SMART ve error log, explicit on-demand. | NVMe admin komutu bazı cihazlarda gecikebilir; kök helper dış işlem yaşam döngüsü gerçek cihazda sınanmalı. |
| H10 USB Stability | Olay/port metin eşleşmesi, boot/seq dedupe. | İstekle gerçekleşmiş bağlantı kesilmesi donanım hatası olmayabilir. |
| H12 Sleep Matrix | **Güncellendi:** boot bazlı örnekler, tekrarlar ayıklanıyor. | Normal başarılı suspend logları mevcut olmayabilir; başarısızlık oranı çıkartılamaz. |
| H13 CO Cross-Link | **Güncellendi:** bozuk guard/session uyarıları, pending durum, boot sınırı. | Belirli offset'in hatanın sebebi olduğu istatistiksel olarak ispatlanamaz; guard'a yazmaz. |
| H15 Precrash | **Güncellendi:** 5/10/30/60s ve değişen cadence. | Son kalıcı kayıt ≠ gerçek çökme anı; bozuk saat veya az örnek halinde belirsizdir. |
| H16 GPU Memory | PCI runtime pasif, ECC aktif sorgu opt-in; hata Unknown. | ECC consumer GPU'da mevcut olmayabilir, opt-in query dGPU uyandırır. |
| H17 Incident Lifecycle | **Güncellendi:** var olan notları yükler, owner-only atomik yazım. | Eski olay satırları log rotasyonu sonrası görünmeyebilir, not kaydı kalabilir. |
| H18 Guided Wizard | Kategoriye göre salt okunur adımlar. | Tam otomatik karar motoru değil; veri kalitesi/diagnostik yorum uzman kontrolü ister. |
| S04 PCIe Link Lab | **Güncellendi:** multiline AER, vendor/device, same boot. | Örneklerin arasında reset olmuşsa tam olay zamanı tespit edilemez. |
| S07 Radio Firmware | **Güncellendi:** USB interface class, firmware/driver ve bounded loglar. | Aygıtı uyandırma/reload yok; bulunan firmware dosyası yüklenmiş olduğunu kanıtlamaz. |
| S08 Power Map | Sysfs powercap/profil ve GPU runtime verisi; GPU'yu uyandırmaz. | SMU PPT/EC kaynağı yoksa göstermez; firmware değişikliği nedenini çıkaramaz. |
| S10 Memory Explorer | NUMA/L3/THP/zram/PSI bilgisi passively okunur. | L3 alanı firmware CCD numarası değildir. |
| S11 ACPI/WMI Browser | Yalnızca WMI GUID ve firmware attribute metadata okunur. | ACPI method çalıştırmaz; current_value özellikle okunmuyor. |
| S13 Charger Inspector | Kernel PD/typec ve power_supply alanları okunuyor. | Ölçülen W değeri bir PD sözleşmesi veya adaptörün garantili watt'ı değildir. |
| S15 PipeWire | **Güncellendi:** harici komutlar asenkron. | Ses sunucusu hiç yanıt vermezse timeout gösterilmeli; xrun sayacı varsayılamaz. |
| S18 Kernel/Firmware | **Güncellendi:** `zcat` pipe drain ve sınır; firmware adayları raporlanır. | Bilinmeyen CONFIG desteklenmiyor demek değildir; uyumluluk liste kapsamıyla sınırlı. |

## Eklenen fixture / regresyon kontrolleri

- `crates/centurion-helpers/src/health.rs`: `aer_sysfs_colon_format_and_unknowns` (normal/0/missing/overflow).
- `crates/centurion-helpers/src/boothist.rs`: Flight interval'in yalnızca doğru boot'a eklenmesi.
- `gui/tests/advanceddiagnostics_test.cpp`: multiline AER, BDF kimliği, H17 not kaydı, 30s örnekleme ve değişen cadence, bozuk CO journal/guard ve H12 aynı olayın iki logdaki kopyası.
- `tests/test_session_audit_static.py`: 13 yeni kaynak tabanlı regression guard.
- Önceden bulunan theme/evidence/advanced/studio/health testleri tekrar çalıştırılır.

## Gentoo üzerinde gerçek doğrulama — zorunlu son adım

Kaynağın kökünden:

```bash
# Rust modülleri ve unit testler
cargo test -p centurion-helpers --lib

# Qt6/C++: dev-qt/qtbase:6, qtdeclarative/svg vb. paketler kurulu olmalı
cmake -S gui -B build-audit -DCENTURION_BUILD_TESTS=ON -DCENTURION_LTO=OFF
cmake --build build-audit -j"$(nproc)"
ctest --test-dir build-audit --output-on-failure

# Kaynak bazlı (gerçek runtime/compile yerine geçmez)
python3 tests/test_session_audit_static.py
python3 tests/test_evidence_lab_static.py
python3 gui/tests/advanced_17_static_test.py
python3 gui/tests/theme_palettes_static_test.py
```

**Kabul kararı:** Kaynak ve statik kontroller bakımından düzeltmeler hazırlanmıştır. Qt6 ve Rust derlemesi + gerçek donanımda salt okunur tanı akışlarının doğrulaması geçmeden **üretim kararlılığı onaylanmış sayılamaz**. İnceleme kasıtlı davranış/arka kapı kanıtı bulmadı, fakat statik denetim tek başına böyle bir yokluğu garanti edemez.
