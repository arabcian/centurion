# Centurion Experimental — Kısmi uygulama ve doğrulama raporu

**24 Kasım 2026 — Durum: TAMAMLANMADI / Qt ve Rust native doğrulaması engelli**

Temel kaynak: kullanıcı tarafından yüklenen `centurion-experimental-followup(1).zip`. Kapsam: `Centurion-sadece-eksik-GUI-gelistirme-plani(1).md`. Bu paket yeni bir ara kaynak teslimatıdır; üretim sürümü veya tüm planın tamamlandığına dair onay değildir. Main, old ve experimental dahil hiçbir uzak branch değiştirilmedi. GitHub yayın şartı sağlanmadı.

## Uygulama matrisi

Kısmi = aşağıda belirtilen davranış kodlandı ancak paket kapsamının tümü ve/veya kabul testi bitmedi. Açık = bu teslimatta yeni uygulama yapılmadı. Native Qt testleri derlenmedi; PASS işareti yalnızca gerçekten çalıştırılan testler için kullanılır.

| ID | Durum | Değişen dosyalar ve yeni davranış | Test / kalan kapsam |
|---|---|---|---|
| A01 | Açık | — | Semantik design system, kontrast raporu ve DPI matrisi. |
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
| B04 | Açık | — | Firmware Power Limit Hierarchy. |
| B05 | Açık | — | Intel voltage domain ve P/E topology. |
| B06 | Açık | — | AMD GPU Frequency Visualization. |
| B07 | Açık | — | Optimization provenance/conflict/device inspector ve journal. |
| B08 | Açık | — | Opt-in persistent draft. |
| B09 | Açık | — | Calibration coverage/manager/weights workspace. |
| B10 | Açık | — | Gerçek Rust event stream ve progress timeline. |
| B11 | Açık | — | A/B ölçüm, uncertainty, selective apply ve history. |
| C01 | Açık | — | Persistent diagnostic sessions, cross-boot timeline ve semantic diff. |
| C02 | Açık | — | Evidence Relationship Graph. |
| C03 | Kısmi | `diagreport.cpp/.h`, `diagreportview.cpp`: bulgu/tablo seçimi, raw opt-in, orijinal verdict korunması, belirli tablo kolonları ve metinde best-effort redaction, düzenlenebilir tek snapshot, QSaveFile. İkili EDID tablosu varsayılan seçili değil. | `diagnostictools_test.cpp` maskeleme ve verdict fixture eklendi; native engelli. Tüm kaynak-aware alanlar, metadata seçimi ve preview/copy/save hash testi açık. Maskeleme kusursuz garanti değildir. |
| C04 | Açık | — | Hardware Device Tree ve PCIe topology. |
| C05 | Kısmi | `diagnostictools.cpp`: salt okunur EDID hex/ASCII, byte offset, base DTD active/blank/total/clock/refresh ve CTA block boundary düzeltmesi. | `diagnostictools_test.cpp` binary ve truncated CTA fixture eklendi; native engelli. Byte seçim/descriptor inspector ve VRR grafik açık. |
| C06 | Açık | — | Memory Architecture View. |
| C07 | Açık | — | ACPI/WMI Explorer. |
| C08 | Açık | — | USB-C PD Visualization. |
| C09 | Açık | — | Structured tool output adapters. |
| C10 | Açık | — | Cooling/battery/sleep/storage geçmişi ve karşılaştırma. |
| C11 | Kısmi | `traffic.cpp/.h`, `netpage.cpp`: program bazlı 30 günlük TX/RX/toplam/bağlantı sayısı grafiği ve tablo, saturating qint64 toplam, eksik güne grafik boşluğu. | `traffic_test.cpp` overflow testleri eklendi, native engelli. Özel tarih aralığı, top-N, malformed ledger quality ve 64-bit chart precision açık. |
| C12 | Kısmi | `netpage.cpp/.h`: eldeki scan'den 2.4/5/6 GHz frequency marker, BSSID tablosu, received timestamp, adapter değişimi ve geç sonuç guard. Yeni otomatik tarama yok. | Width uydurulmadı. Actual scan freshness, tüm adaptör yarış fixture'ları ve grafik doğrulaması açık. |
| C13 | Açık | — | Firewall rule diff ve impact preview. |
| D01 | Açık | — | Visual Scene Builder ve step inspector. |
| D02 | Kısmi | `lightingtab.cpp`: 32 kayıt sınırı olan HEX boya renk paleti, kayıt/temizleme/renk seçimi, sıralı effect numarası. | Mevcut draft/Undo/Redo korunur; yeni palet donanıma yazmaz. Software preview ve tam timeline/sequence editor açık. |
| D03 | Açık | — | Dosya seviyesinde restore preview, güvenli manifest ve config diff. |
| D04 | Kısmi doğrulama | `gui/i18n/centurion_tr.ts`: yeni arayüz metinlerinin TR çevirileri; `gui_stage4_static_test.py` iki değişen kaynak sözleşmesinin assertion güncellemesi. | Mevcut 27 runner PASS; FIX-01–21 native regresyonu ve iki UI modu görsel QA açık. |
| D05 | Engelli | Qt6 ve Cargo kurulumu denendi; debug/release CMake ve Cargo doğrulama girişimleri yapıldı. | DNS engeli, Qt6/Cargo eksik. Native test, clippy, cargo-deny, sanitizer, fiziksel doğrulama, GitHub CI ve yayın yapılmadı. |

**Özet:** 39 iş paketi; 10 iş paketinde kısmi fonksiyon değişikliği, D04'te kısmi çeviri/test çalışması, D05 engelli, 27 iş paketi açık. Tam kabul kriterleriyle bitmiş paket sayısı 0.

## Çalıştırılan kontroller

| Kontrol | Sonuç |
|---|---|
| 27 mevcut Python test çalıştırıcısı (gui/tests + tests) | PASS 27/27. Bunların çoğu statik kaynak sözleşmesi, iki tanesi aşağıdaki C++ simülasyonlarını çalıştırır. |
| CO walk C++ simülasyonu | PASS: 454.710 walk, 2.353.711 probe, maksimum 50 probe/core. |
| CO reboot C++ simülasyonu | PASS: 183.621 crash ve 327.669 clean senaryo. |
| Navigation metadata generator `--check` | PASS. |
| `git diff --check` | PASS. |
| Debian paket index güncellemesi | BLOCKED: `Temporary failure resolving 'deb.debian.org'`. |
| Qt6/Cargo paket kurulumu | BLOCKED: paketler bulunamadı; index indirilemedi. |
| Debug CMake, sanitizer ON | BLOCKED: Qt6 >=6.4 `Qt6Config.cmake` bulunamadı. |
| Release CMake | BLOCKED: aynı Qt6 eksikliği. |
| `cargo test --workspace --locked` | BLOCKED: cargo bulunamadı (127). |
| `cargo clippy --workspace --all-targets --locked -- -D warnings` | BLOCKED: cargo bulunamadı (127). |
| Yeni native Qt fixture'ları, bütün CTest, cargo-deny, ASan/UBSan | Çalıştırılmadı. Native build gerektirir. |

Mevcut simülasyon runner'ları bütün Qt uygulamasını derlemez; yeni CO preview metodu da native Qt testinde ayrıca doğrulanmalıdır. Bu nedenle C++/Qt derleme hatası veya warning olmadığı iddia edilmiyor. Fiziksel donanım yazma, undervolt, firmware ve restore testleri yapılmadı. Yeni özelliklerin gerçek UI'da ve fiziksel sistemde doğrulaması açık.

## Sonraki doğrulama

Planın D05 komutları, bağımlılıkları gerçekten kurabilen ortamda aynen çalıştırılmalı. Önce Qt debug ve release build + CTest, ardından workspace Rust tests/clippy/cargo-deny ve offscreen layout QA; sonrasında eksik davranışlar için planın kendi kabul fixture'ları. GitHub yayınından önce hedef branch ve base güncelliği kontrol edilmeli. Bu kısmi arşiv doğrulanmış experimental release değildir.
