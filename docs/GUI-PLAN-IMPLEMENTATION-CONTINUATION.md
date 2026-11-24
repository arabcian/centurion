# Centurion Experimental — İkinci uygulama turu / kümülatif devir raporu

Temel: `Centurion-experimental-gelistirme-kismi.zip`; orijinal geliştirme planı A01–D05. Önceki kısmi geliştirmeler korunmuştur. Bu paket tamamlanmış üretim sürümü değildir. Yeni turda daha önce açık olan 10 iş paketi için kod eklendi; C03 ve D04 genişletildi. Tam kabul kriteriyle biten paket yoktur.

## Kümülatif durum

| ID | Durum | Gerçek kod değişikliği | Test ve kalan kapsam |
|---|---|---|---|
| A01 | Kısmi | theme.cpp/.h, hometab.cpp, theme_runtime_test.cpp, tools/audit_palette_contrast.py: palette token contrast matrisi, özel tema kontrolü, seçili metin için kontrastı en yüksek siyah/beyaz renk ve filtreli rapor. | 57 palet, 3990 token kontrolü: 737 eşik altı, 57 istisna; CSV gerçek hesaplama. Tam design system, eşik altı çiftlerin düzeltilmesi, rendered/DPI doğrulaması açık. |
| A02 | Açık | — | Live theme switching ve cache invalidation. |
| A03 | Açık | — | Shared telemetry cache, TTL, subscription, quality. |
| A04 | Kısmi | `optimizetab.cpp/.h`: Autotune ağırlık varsayılanı sorgusu asenkron; timeout, görünürlük, busy ve QPointer kontrolü. `netpage.cpp/.h`: scan revision kontrolü. | CO başlangıç/resume dahil kalan blocking yollar, tüm request generation, timeout/cancel fixture'ları açık. |
| A05 | Açık | — | Operation registry, rezervasyon, history ve restoration yaşam döngüsü. |
| A06 | Kısmi | `charts.cpp/.h`: LineChart zoom, klavyeli pan/reset, görünür/tüm CSV, finite sample ve gap kontrolü, min/max bucket. | `appearance_layout_test.cpp` 100k örnek fixture eklendi, çalıştırılamadı. Ortak eksen API, diğer grafik türleri, V/F entegrasyonu, legend ve erişilebilir tablo açık. |
| A07 | Açık | — | İzole hardware simulation backend ve fixture'lar. |
| A08 | Açık | — | Görsel baseline, diff artifact ve gerçek sayfa matrisi. A06 test eklemesi tek başına bu paketi karşılamaz. |
| A09 | Kısmi | `uinavigator.cpp`, `toolcatalog.cpp/.h`, `optimizetab.cpp/.h`, `tuningnavigation.h`, `tools/generate_navigation_index.py`: 279 optimization metadata kaydı, fuzzy arama, stable key ile focus ve mevcut alt tab indeksleri. | Generator --check PASS; `toolcatalog_test.cpp` yeni ranking testleri native engelli. Bütün alt araç/alias/çeviri indeks kapsamı ve lazy sayaç fixture'ı açık. |
| A10 | Kısmi | `hometab.cpp`: experimental panel order, klavyeli sıralama, reset, iki opsiyonel panel görünürlüğü ve reflow hysteresis. | Classic aynı bırakıldı; güvenlik açısından güç ve cihaz panelleri gizlenmedi. İki mod/density restart fixture'ı, polling ve tam görünürlük kapsamı açık. |
| B01 | Açık | — | CCD stability, session archive ve semantik profil diff. |
| B02 | Kısmi | `costrategy.cpp/.h`, `cosearchdialog.cpp`: Search kopyası üzerinde const preview, resume'da sonraki aday ve test süresi. | Mevcut CO simülasyonları PASS; yeni `costrategy_test.cpp` preview state-equality testi native engelli. Typed karar nedenleri, gerçek ETA ve revision invalidation açık. |
| B03 | Açık | — | V/F validation, history, fingerprint ve transaction readback. |
| B04 | Kısmi | systemdiagnostics.cpp: mevcut güç raporunda kaynak hiyerarşisi, birim ve missing state ayrımı. | S08 üzerinden erişim; doğrudan firmware inspector entegrasyonu ve gerçek dependency metadata/fixture açık. |
| B05 | Kısmi | inteltab.cpp/.h, systemdiagnostics.cpp: son helper mailbox verisiyle voltage domain ve CPU package/core/SMT inceleyicisi. | Core type yalnızca gerçek kaynakla; P/E domain birliktelik ve eksik hybrid fixture açık. |
| B06 | Kısmi | amdgputab.cpp/.h: seçili kartın son örneğine bağlı core/VRAM frekans, mevcut DPM/OD noktaları, draft aralık BarChart; kart değişiminde/eski veride temizleme. | Native ve iki kart/sleep fixture, ortak telemetry ve etkileşimli editör entegrasyonu açık. |
| B07 | Açık | — | Optimization provenance/conflict/device inspector ve journal. |
| B08 | Kısmi | optimizetab.cpp/.h: manuel atomik sürümlü VM/scheduler draft save/restore/delete, fingerprint, preview, live/boot mismatch, restore sonrası checkbox boş. | Per-device ve diğer scope, migration ve restart davranış fixture açık. Doğrulanamayan cihaz kimlikleri dışarıda. |
| B09 | Açık | — | Calibration coverage/manager/weights workspace. |
| B10 | Açık | — | Gerçek Rust event stream ve progress timeline. |
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
| C13 | Açık | — | Firewall rule diff ve impact preview. |
| D01 | Açık | — | Visual Scene Builder ve step inspector. |
| D02 | Kısmi | `lightingtab.cpp`: 32 kayıt sınırı olan HEX boya renk paleti, kayıt/temizleme/renk seçimi, sıralı effect numarası. | Mevcut draft/Undo/Redo korunur; yeni palet donanıma yazmaz. Software preview ve tam timeline/sequence editor açık. |
| D03 | Açık | — | Dosya seviyesinde restore preview, güvenli manifest ve config diff. |
| D04 | Kısmi | Yeni arayüz metinleri TR kataloğuna eklendi; mevcut testler korundu. | 27/27 runner PASS; FIX-01–21 native ve görsel QA açık. |
| D05 | Engelli | apt index yeniden denendi; Debug ve Release CMake, Cargo test çalıştırıldı. | DNS engeli devam ediyor; Qt6Config.cmake bulunamadı, Cargo yok. Native, sanitizer, clippy, CI, fiziksel doğrulama ve yayın yapılmadı. |

**39 iş paketi: 21 kısmi (20 fonksiyonel + D04), 17 açık, 1 doğrulama engelli (D05), 0 bütün kabul kriteri tamamlanmış.**

## Bu turdaki kontroller

- 27 mevcut Python test çalıştırıcısı: PASS 27/27. 26 ilk çalıştırmada, GUI stage 3 ise yeni metodun yerleşimi düzeltildikten sonra aynı değiştirilmemiş test ile PASS. Bu testlerin çoğu statik sözleşme kontrolü; iki runner C++ CO simülasyonlarını çalıştırır.
- CO walk: 454710 yürüyüş, 2353711 probe; reboot: 183621 crash ve 327669 clean senaryo PASS.
- Türkçe katalog: 5/5 PASS; navigation generator `--check` PASS; `git diff --check` PASS.
- Palette token audit: 57 tema × 70 kontrol = 3990; 3196 PASS, 737 eşik altı, 57 disabled istisnası. 56 palette en az bir eşik altı çift var. Renkler topluca değiştirilmedi; bu sonuç rendered Qt kontrolü değildir. `docs/GUI-PALETTE-CONTRAST-AUDIT.csv` ve tekrar çalıştırılabilir `tools/audit_palette_contrast.py` dahil.
- `diagnostictools_test.cpp`, `theme_runtime_test.cpp`: yeni davranış fixture'ları eklendi; native Qt çalıştırılamadı.
- Debian apt index: `Temporary failure resolving 'deb.debian.org'`; Debug/Release CMake: Qt6Config.cmake yok; Cargo: command not found. Dolayısıyla derleme hatası veya warning yoktur iddia edilmez. Yeni Qt davranışları çalışır kabul edilmez.
- Donanım yazma, undervolt, restore, firmware execution, GitHub CI/branch push yapılmadı. Main/old/experimental uzak ref'leri bu turda değiştirilmedi. Planın yayın şartı karşılanmadı.

Önceki rapor `docs/GUI-PLAN-IMPLEMENTATION-PARTIAL.md` tarihsel kayıttır. Güncel kapsam için bu belge kullanılmalı. Eksik 17 paket ve kısmi paketlerin kalan kabul kriterleri yukarıdaki tabloda korunur.
