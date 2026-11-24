# Centurion Experimental — Tur 16 kısmi geliştirme ve devir raporu

**Temel:** `Centurion-experimental-tur15-kismi.zip` — SHA-256 `2e1e4eda3eb53bbaff0b4096eef3aea7694b592a2e51b353c25d464802759d34`. Input CRC PASS. Tur 14 birleşik devir ve Tur 15 kayıtları kaynakta aynen korunmuştur.

**Kabul durumu:** 48 özgün paket / 118 özgün GUI ID; **0 tam kabul, 47 kısmi, D05 doğrulama engelli**. Tur 16 C03, C05, C08, C09 alt davranışları ve D04 çevirileri ilerledi; D05 doğrulama denemeleri tekrarlandı. Diğer paketlerde yeni davranış kaydı yoktur; bu durum onların kaynakta tamamen bulunmadığı anlamına gelmez. Tur 16 48 paketin tamamı için çağrı zinciri denetimi değildir. Önceki orijinal kapsam ve kabul sütunları değişmeden yeni sütunlar eklenmiştir. C08 / GUI-156 gibi 118 ID listesi dışındaki ek ID'ler 48 paket matrisinde korunur; yeni ID oluşturulmadı.

## Uygulama ve kalan işler

### C03 — GUI-142/143: Rapor seçimi ve gizlilik
- `diagreportview.cpp:previewExport`: bulgu başlığı (ayrıntı/eylem için zorunlu bağlam), ayrıntı/eylem, tablo notu, açıklama, ayrı ayrı kaynak sınırlaması, yakalama zamanı ve boot kimliği seçimi. Private-binary tabloların seçili olmasına ek olarak açık izin gerekir; varsayılan kapalı. Raw/binary/redaction-off uyarıları. Eski frozen preview, SHA, yeniden üretim zorunluluğu ve orijinal verdict korunur.
- `diagreport.cpp:redactPrivateText/Fields`: etiketli PARTUUID, GPU UUID (prefix dahil), 32 hex machine ID, kısa filesystem ID, udev tanımlayıcıları, SSID/BSSID, media kullanıcı yolu ve `$ uname -a` hostname; bilinen full capture UUID etiket olmadan da maskelenir. Mevcut PCI BDF ve WMI GUID koruması devam eder. Yeni fixture bunları kontrol ediyor.
- Kalan: genel source schema/provenance ve bütün hassas alanların testleri; gerçek Qt selection/clipboard/save byte eşitliği, yenileme yarışı ve manuel review fixture. Maskeleme tam anonimleştirme değildir.

### C05 — GUI-152: EDID binary export
- `diagreportview.cpp:inspectEdid`: seçilen connector'ın önceden yakalanmış baytlarını açık seri numarası/eksik veya bozuk veri uyarısıyla `.bin` kaydetme. QSaveFile, owner-only permission, doğrudan yazma fallback kapalı. Connector tekrar okunmaz, helper/override işlemi başlatılmaz. Bozuk/eksik capture tanılama kopyası olabilir; geçerli override olarak sunulmaz.
- Kalan: Qt gerçek dosya byte/permission, click/keyboard/connector disconnect/resize fixture, DisplayID/interlaced ve compositor kapsamı. Önceki progressive parser bağımsız testleri PASS; yeni Qt kaydetme yolu çalıştırılmadı.

### C08 — GUI-156 (ek paket ID): USB-C PD
- `systemdiagnostics.cpp:charging`: port/partner/PD canonical path ve inode/device kimliği, power/data role ve PD revision yeniden kontrolü; değişen snapshot PDO'ları atılır. Eksik/bozuk veya sınırlanmış profil seti maksimum kapasite ya da düşük kapasite uyarısı üretmez. Raporlanan negatif tüketim korunur.
- `diagreportview.cpp:inspectPd`: yakalanmış port seçimi, partner/rol ve kaynak kapasitesi grafiği (en çok 32 doğrulanmış profil), bütün yakalanan profillerin tablosu, bağımsız supply ölçümleri. Yeni probe yok. Müzakere edilmiş kontrat bilinmiyor; kapasite/kontrat/ölçüm birbirine eşit sayılmıyor.
- Kalan: gerçek negotiated contract ancak güvenilir kaynak sağlanırsa; port-supply eşleme kanıtı, yarış/role swap/replacement/hardware ve Qt görsel fixture. Qt testlerine eksik PDO, source-role ve disconnect eklendi ama çalıştırılmadı.

### C09 — GUI-146/147: Raw / Structured Tool Output
- `structuredpci.h` ve `structuredtools.h`: mevcut `lspci -nnk` kaydından BDF, class/vendor/device kodları, driver ve module listesi. Aynı domain adresinin iki gösterimiyle duplicate, malformed, unknown ve truncated veri reddedilir. Serbest donanım açıklaması/seri verisi structured export'a kopyalanmaz. Mevcut lsmod, battery, lsusb, lscpu adaptörleri korunur; battery sayısal alanlar strict signed integer kontrolü alır.
- `systools.cpp:ToolPage`: aynı frozen snapshot içinde raw (mevcut görünen/maskeleme davranışı) ve structured geçişi; QStandardItemModel + QSortFilterProxyModel, tüm kolonlarda arama, sayısal kolonlarda exact unsigned sort, görünen/seçili/tüm satırları kopyala, raw copy; başarısız parser raw fallback. Görünüm değişikliği yeni komut çalıştırmaz.
- Gerçek komut capture yalnız başarılı normal exit ve limit altında olduğunda adapter'a aktarılır; 1 MiB tampon sınırı, C locale yalnız ilgili protokoller, sahte `$ ` section delimiter reddi; raw output ayrı tutulur. Battery için gerçek uevent dosyalarından ayrı bounded capture.
- Native PCI: Debug, O2, ASan/UBSan PASS; duplicate/missing driver/error/bounds ve her modda 12.000 rastgele satır. Qt adapter testleri genişletildi; GUI/proxy/selection/process/clipboard testleri BLOCKED. Diğer uygun tool adapter'ları/helper JSON açık.

### D04/D05 — çeviri, build ve test
- 41 yeni Türkçe mesaj, mevcut katalog korunur. CI'ya yeni native PCI runner eklendi; uzaktaki CI çalıştırılmadı.
- 43/43 Python runner PASS; önceki 42 + yeni PCI. Native bağımsız C++ parser/algoritma testleri ile statik/fixture runner ayrıdır. Qt GUI kabulü yerine geçmez.
- APT DNS: `Temporary failure resolving 'deb.debian.org'`; paket indeksleri erişilemedi. Qt Debug/Release configure `Qt6Config.cmake` eksik; cargo test/clippy `command not found`. CTest, sanitizer GUI, Rust ve Gentoo LTO/PGO çalışmadı. Tur 15 theme warning düzeltmesi korunur, Gentoo uyarı doğrulaması hâlâ açık.
- Gerçek donanım, privileged helper, fan/SMU/NVcurve/firewall/firmware yazma testleri yapılmadı. Bu tur `crates/` veya CO/NVIDIA apply/reset kaynakları değiştirilmedi. Fiziksel güvenlik kabulü iddia edilmez. GitHub push yapılmadı; main/old dokunulmadı.

## Değişiklik envanteri
- 9 mevcut kaynak/test/CI dosyası değişti, 2 yeni kaynak/test dosyası; hiçbir input dosyası silinmedi. Ek belge ve matrisler aşağıda.
- `.github/workflows/ci.yml`
- `gui/i18n/centurion_tr.ts`
- `gui/src/diagreport.cpp`
- `gui/src/diagreportview.cpp`
- `gui/src/structuredtools.h`
- `gui/src/systemdiagnostics.cpp`
- `gui/src/systools.cpp`
- `gui/tests/diagnostictools_test.cpp`
- `gui/tests/structuredtools_test.cpp`
- `gui/src/structuredpci.h`
- `gui/tests/structuredpci_native_test.py`

## Tam kapsam takibi
- `GUI-TUR16-48-PAKET.tsv`: 48 paket, orijinal tam kapsam ve kabul kriterleri, Tur 15 ve Tur 16 gerçek farkları ve kalan sınırlar.
- `GUI-TUR16-118-KABUL.tsv`: 118 orijinal GUI ID, önceki ve yeni kayıtlar. Hiçbir ID tam kabul edilmedi.
- `GUI-TUR16-RUNNERS.tsv`: 43/43 sonuç. Önceki Tur 14/15 rapor ve matrisleri değişmeden durur.
- `Centurion-Tur16-kanit-ve-kabul.zip`: iki yeni matris, doğrulama raporu, patch, ham test/configure/kurulum logları.

**Sonuç:** Bu bir kısmi kaynak teslimatıdır; bütün 48 paketin kapanmış olduğu veya release hazır olduğu iddia edilmez. Native build/physical acceptance eksik olduğundan experimental branch'e push yapılmamıştır.
