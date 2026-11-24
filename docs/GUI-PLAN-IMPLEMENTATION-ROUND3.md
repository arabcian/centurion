# Centurion Experimental — Üçüncü uygulama turu / kümülatif devir raporu

Temel: `Centurion-experimental-gelistirme-devam.zip` (ikinci tur). A01–D05 planı ve önceki değişiklikler korunmuştur. Bu turda daha önce açık olan 7 pakete kısmi uygulama eklendi; A04 ve D04 genişletildi. Tam kabul kriteriyle biten paket yoktur.

## Kümülatif durum

| ID | Durum | Gerçek kod değişikliği | Test ve kalan kapsam |
|---|---|---|---|
| A01 | Kısmi | theme.cpp/.h, hometab.cpp, theme_runtime_test.cpp, tools/audit_palette_contrast.py: palette token contrast matrisi, özel tema kontrolü, seçili metin için kontrastı en yüksek siyah/beyaz renk ve filtreli rapor. | 57 palet, 3990 token kontrolü: 737 eşik altı, 57 istisna; CSV gerçek hesaplama. Tam design system, eşik altı çiftlerin düzeltilmesi, rendered/DPI doğrulaması açık. |
| A02 | Açık | — | Live theme switching ve cache invalidation. |
| A03 | Açık | — | Shared telemetry cache, TTL, subscription, quality. |
| A04 | Kısmi | Önceki asenkron ağırlık ve Wi-Fi guard + `optimizetab.cpp`: Autotune başlangıçtaki taslak snapshot’ını tamamlanınca yeniden kontrol eder; değişiklik varsa sonucu salt okunur açar. Çıkış kodu/timeout başarısızsa JSON ok true olsa da sonuç yüklenmez. | CO başlangıç/resume blocking yollar, kapsamlı generation/cancel testleri açık. |
| A05 | Kısmi | `mainwindow.cpp/.h`, `tray.cpp`, `optimizetab.h`: işlem merkezi gözlemlenen geçiş geçmişi (200 satır), tray ve gerçek pencere çıkışında busy guard; Autotune busy durumuna dahil. | Polling gözlemi gerçek sonuç değildir, kısa işlemler kaçabilir. Ortak kaynak rezervasyonu, transaction owner, sonuç registry, restoration ve OS shutdown sınırları açık. |
| A06 | Kısmi | `charts.cpp/.h`: LineChart zoom, klavyeli pan/reset, görünür/tüm CSV, finite sample ve gap kontrolü, min/max bucket. | `appearance_layout_test.cpp` 100k örnek fixture eklendi, çalıştırılamadı. Ortak eksen API, diğer grafik türleri, V/F entegrasyonu, legend ve erişilebilir tablo açık. |
| A07 | Açık | — | İzole hardware simulation backend ve fixture'lar. |
| A08 | Açık | — | Görsel baseline, diff artifact ve gerçek sayfa matrisi. A06 test eklemesi tek başına bu paketi karşılamaz. |
| A09 | Kısmi | `uinavigator.cpp`, `toolcatalog.cpp/.h`, `optimizetab.cpp/.h`, `tuningnavigation.h`, `tools/generate_navigation_index.py`: 279 optimization metadata kaydı, fuzzy arama, stable key ile focus ve mevcut alt tab indeksleri. | Generator --check PASS; `toolcatalog_test.cpp` yeni ranking testleri native engelli. Bütün alt araç/alias/çeviri indeks kapsamı ve lazy sayaç fixture'ı açık. |
| A10 | Kısmi | `hometab.cpp`: experimental panel order, klavyeli sıralama, reset, iki opsiyonel panel görünürlüğü ve reflow hysteresis. | Classic aynı bırakıldı; güvenlik açısından güç ve cihaz panelleri gizlenmedi. İki mod/density restart fixture'ı, polling ve tam görünürlük kapsamı açık. |
| B01 | Kısmi | `coarchive.h`, `cosearchdialog.cpp/.h`: mevcut v2 oturum arşivi atomik, kilitli, 40 oturum / 1 MiB sınırlı; aşırı büyük kayıtların eski denemeleri açık sayaçla çıkarılır; bozuk kayıt üzerine yazılmaz. CCD/slot/logical CPU, arama durumu, son başarısız değer, doğrulama ve reset bilgisi görüntülenir. Aktif journal değiştirilmedi. | `coarchive_test.cpp` eklendi, Qt engelli. Arşiv export/delete, CPU fingerprint ve semantik profil karşılaştırması açık. Eski oturumlarda eksik fiziksel kimlik bilinmiyor olarak kalır. |
| B02 | Kısmi | `costrategy.cpp/.h`, `cosearchdialog.cpp`: Search kopyası üzerinde const preview, resume'da sonraki aday ve test süresi. | Mevcut CO simülasyonları PASS; yeni `costrategy_test.cpp` preview state-equality testi native engelli. Typed karar nedenleri, gerçek ETA ve revision invalidation açık. |
| B03 | Açık | — | V/F validation, history, fingerprint ve transaction readback. |
| B04 | Kısmi | systemdiagnostics.cpp: mevcut güç raporunda kaynak hiyerarşisi, birim ve missing state ayrımı. | S08 üzerinden erişim; doğrudan firmware inspector entegrasyonu ve gerçek dependency metadata/fixture açık. |
| B05 | Kısmi | inteltab.cpp/.h, systemdiagnostics.cpp: son helper mailbox verisiyle voltage domain ve CPU package/core/SMT inceleyicisi. | Core type yalnızca gerçek kaynakla; P/E domain birliktelik ve eksik hybrid fixture açık. |
| B06 | Kısmi | amdgputab.cpp/.h: seçili kartın son örneğine bağlı core/VRAM frekans, mevcut DPM/OD noktaları, draft aralık BarChart; kart değişiminde/eski veride temizleme. | Native ve iki kart/sleep fixture, ortak telemetry ve etkileşimli editör entegrasyonu açık. |
| B07 | Açık | — | Optimization provenance/conflict/device inspector ve journal. |
| B08 | Kısmi | optimizetab.cpp/.h: manuel atomik sürümlü VM/scheduler draft save/restore/delete, fingerprint, preview, live/boot mismatch, restore sonrası checkbox boş. | Per-device ve diğer scope, migration ve restart davranış fixture açık. Doğrulanamayan cihaz kimlikleri dışarıda. |
| B09 | Kısmi | `calibrationcoverage.h`, `optimizetab.cpp`: v10 gerçek y alanında sonlu ölçüm sayıları; phase/kernel/context/power-source grupları, dört hedef için ayrı kayıt sayısı ve bilinmeyen/nonpositive weight. | `calibrationcoverage_test.cpp` eklendi, Qt engelli. Modelin yeterlilik/validity kararı değildir. Fingerprint yönetimi, radar, arşiv/import/export açık. |
| B10 | Kısmi | `optimizetab.cpp/.h`, `centurion-autotune.rs`: mevcut gerçek CENTURION_STAGE stderr olaylarını süreli zaman çizelgesinde gösterir, sıra dışı/tekrarlı aşamaları geçer, hata kuyruğunu sınırlar; CLI --progress opt-in. Yüzde uydurulmaz; sonuç ancak başarılı exit + ok JSON ile kabul edilir. | Yeni JSON event/session protokolü, iptal kontrolü, tüm progress fixture’ları ve Rust/native doğrulama açık. |
| B11 | Açık | — | A/B ölçüm, uncertainty, selective apply ve history. |
| C01 | Kısmi | diagreport.cpp/.h, diagreportview.cpp/.h: rapor başına 20 snapshot/1 MiB limit, manuel kayıt ve silme, local QLockFile/QSaveFile, raw/fix/binary hariç, locale ve capture boot ayrımı; sıra bağımsız multiset diff. | Cross-boot unified timeline, adapter, session grouping, stable row keys, notlar ve geçmişin bağımsız gezinmesi açık; native fixture çalıştırılmadı. |
| C02 | Açık | — | Evidence Relationship Graph. |
| C03 | Kısmi | `diagreport.cpp/.h`, `diagreportview.cpp`: bulgu/tablo seçimi, raw opt-in, orijinal verdict korunması, belirli tablo kolonları ve metinde best-effort redaction, düzenlenebilir tek snapshot, QSaveFile. İkili EDID tablosu artık dil-bağımsız privateBinary metadata ile varsayılan export ve history dışında. | `diagnostictools_test.cpp` maskeleme ve verdict fixture eklendi; native engelli. Tüm kaynak-aware alanlar, metadata seçimi ve preview/copy/save hash testi açık. Maskeleme kusursuz garanti değildir. |
| C04 | Kısmi | systemdiagnostics.cpp/.h, diagreport.h, diagreportview.cpp: canonical PCI/USB/NVMe/network ilişkileri, filtreli hierarchy, eski PCIe scan alanları kimlik kontrolüyle kullanılır. | Derin bağlantı, persistent selection, lazy expand, removal/cycle ve native fixture doğrulaması açık. |
| C05 | Kısmi | `diagnostictools.cpp`: salt okunur EDID hex/ASCII, byte offset, base DTD active/blank/total/clock/refresh ve CTA block boundary düzeltmesi. | `diagnostictools_test.cpp` binary ve truncated CTA fixture eklendi; native engelli. Byte seçim/descriptor inspector ve VRR grafik açık. |
| C06 | Kısmi | systemdiagnostics.cpp/.h: CPU package/core ve sibling bilgisi, NUMA memory/distance, unknown RAM/PSI ayrımı. | Cache/CCD/SMBIOS daha geniş entegrasyon, memory topology grafik, tüm fixture açık. |
| C07 | Kısmi | systemdiagnostics.cpp, diagreportview.cpp: kernel exposed ACPI/WMI device tree, instance ve GUID, driver ve status; salt okunur. | Event ilişkilendirme, AML tablo listesi, geniş fixture açık; AML method execution yok. |
| C08 | Kısmi | systemdiagnostics.cpp: USB-C port/partner ve supply gerçek ölçüm/kapasite ayrımı, signed voltage/current power; bilinmeyen kontrat varsayılmıyor. | PD contract grafiği, partner/profile ilişkisi ve tüm fixture açık. |
| C09 | Açık | — | Structured tool output adapters. |
| C10 | Açık | — | Cooling/battery/sleep/storage geçmişi ve karşılaştırma. |
| C11 | Kısmi | `traffic.cpp/.h`, `netpage.cpp`: program bazlı 30 günlük TX/RX/toplam/bağlantı sayısı grafiği ve tablo, saturating qint64 toplam, eksik güne grafik boşluğu. | `traffic_test.cpp` overflow testleri eklendi, native engelli. Özel tarih aralığı, top-N, malformed ledger quality ve 64-bit chart precision açık. |
| C12 | Kısmi | `netpage.cpp/.h`: eldeki scan'den 2.4/5/6 GHz frequency marker, BSSID tablosu, received timestamp, adapter değişimi ve geç sonuç guard. Yeni otomatik tarama yok. | Width uydurulmadı. Actual scan freshness, tüm adaptör yarış fixture'ları ve grafik doğrulaması açık. |
| C13 | Kısmi | `netrules.cpp/.h`, `netfirewall.cpp/.h`: helper’ın bildirdiği kayıtlı kuralları pin/diff; ID, sıra, policy ve alan değişiklikleri; eksik/yinelenen kimlik reddi. Açık add/edit dialogu sırasında bildirilen konfigürasyon değişirse istek gönderilmez. | `netrules_test.cpp` genişletildi, Qt engelli. Kernel effective ruleset değil; canonical semantic normalization, packet matching, backend revision guard ve impact preview açık. |
| D01 | Kısmi | `scenes.cpp/.h`, `scenestab.cpp/.h`: gerçek transaction callback’lerinden running/returned/failed/soft failure/skipped/restore sonuçları; bounded son yürütme geçmişi. Mevcut yürütme sırası ve rollback korunur. | Ayrı görsel builder, tam dependency/trigger analizi, step fixture ve harici daemon sonuçları açık. Helper sonucu donanım readback değildir. |
| D02 | Kısmi | `lightingtab.cpp`: 32 kayıt sınırı olan HEX boya renk paleti, kayıt/temizleme/renk seçimi, sıralı effect numarası. | Mevcut draft/Undo/Redo korunur; yeni palet donanıma yazmaz. Software preview ve tam timeline/sequence editor açık. |
| D03 | Kısmi | `backuppage.cpp`: mevcut config_inspect helper ile configuration restore manifest scope ağacı, bildirilen sayılar ve hedefler; user_state ve system_profile restore edilmez. Sistem image file-level preview unsupported açıkça gösterilir. | Mevcut config_diff ve sistem sidecar zinciri korunur. Dosya manifesti, hash/permission diff, arşiv/hedef revalidation ve traversal fixture açık. Önizleme restore veya mount başlatmaz. |
| D04 | Kısmi | Yeni arayüz metinlerine 88 TR katalog girdisi eklendi. Mevcut testler korundu; önceki 27 runner tekrar çalıştırıldı. | Native FIX-01–21, DPI ve iki UI modu QA açık. |
| D05 | Engelli | Debug + Release CMake tekrar denendi, Qt6Config.cmake yok; cargo bulunamadı. | 27/27 mevcut Python runner PASS (çoğu statik sözleşme, ikisi bağımsız C++ CO simülasyonu). Yeni Qt testleri, Rust/Clippy, sanitizers, CI ve fiziksel doğrulama yapılamadı. Yayın yok. |

**39 iş paketi: 28 kısmi (27 fonksiyonel + D04), 10 açık, 1 doğrulama engelli (D05), 0 bütün kabul kriteri tamamlanmış.**

Henüz hiç uygulanmamış: A02, A03, A07, A08, B03, B07, B11, C02, C09, C10. Diğer paketlerin açık alt maddeleri tabloda belirtilmiştir.

## Bu turda doğrulama

- 27/27 mevcut Python runner PASS. CO walk: 454710 yürüyüş, 2353711 probe; reboot: 183621 crash + 327669 clean senaryo PASS. Bu testler Qt GUI derlemesini doğrulamaz.
- 3 yeni/genişletilmiş native Qt fixture: coarchive, calibrationcoverage, netrules. CTest kapsamındalar, çalıştırılamadılar.
- Mevcut TR katalog testi 5/5 PASS, title-case testi PASS. `git diff --check` PASS.
- Debug (ASan/UBSan flag dahil) ve Release CMake yapılandırması Qt6Config.cmake eksikliği nedeniyle başarısız. Cargo komutu yok; DNS çözümlemesi yine sonuç vermedi. Native build / warning / sanitizer / Rust doğrulaması yapılmış sayılmaz.
- Fiziksel donanım yazması, restore, GitHub CI/push yapılmadı. Experimental branch yayını planın kabul kriteri tamamlanmadığından yapılmadı. Main ve old değiştirilmedi.
- Önceki turdaki `GUI-PLAN-IMPLEMENTATION-CONTINUATION.md` tarihsel kayıt olarak korunmuştur.
