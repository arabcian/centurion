# Centurion Experimental — Tur 14 sonrası birleşik devir ve eksik geliştirme raporu

**Yeni tek kaynak:** `Centurion-experimental-tur14-kismi.zip`  
**SHA-256:** `6d50183ae95f4a72b54fa7cf10ec9b3701147acac7a73ca681930c7f488b3235`  
**Hedef:** `arabcian/centurion` / `experimental` (main ve old dokunulmayacak).  
**Tarih:** 24 Kasım 2026. **Sonraki geliştirme:** Tur 15.

## 1. Durum ve kapsam
Tur 5 birleşik planı, Tur 6–14 raporları, son 118 ID TSV, Tur 5/Tur 14 ZIP içerik hash karşılaştırması ve seçili güncel kaynak giriş noktaları esas alınmıştır. Bu bir kapsam uzlaştırması ve devir raporudur; 595 dosyanın tamamının yeniden satır satır denetimi, yeni build/test veya donanım doğrulaması değildir. Raporlarda belgelenmemiş davranışın kaynakta kesin yok olduğu varsayılmaz. Yeni sohbet, özellikle yeni uygulama kaydı bulunmayan satırları önce gerçek çağrı zincirinde kontrol etmelidir.
- **48 yürütme paketi, 118 orijinal GUI ID; 0 tam kabul.** 47 paket kısmi (bazı hedef alt işlevler tamamen eksik), D05 doğrulama engelli. ID tablosunda 63 satır için ilgili ilerleme kaydı, 55 satır için ID özelinde yeni tamamlayıcı davranış kaydı yok. Bu sayı bitirme yüzdesi veya her ID’nin tamamen yok olduğu anlamına gelmez.
- ZIP CRC yeniden kontrol edildi: PASS. Tur 5 554 dosya; Tur 14 595 dosya; 89 mevcut dosya içeriği farklı, 41 yeni dosya, 0 silinen dosya (dizin girdileri hariç; dokümantasyon/test dahil).
- Tur 14 raporunda 42/42 Python runner PASS (bazısı bağımsız native C++; diğerleri statik/fixture). Qt Debug/Release, CTest, GUI sanitizer/visual, Rust, CI ve gerçek donanım kabulü yapılmadı. Bu devir çalışmasında bu testler yeniden çalıştırılmadı.
- Tur 10 Build Fix: QJsonObject için `constBegin()/constEnd()`; koru. Kullanıcının Gentoo PGO derleme hatası bu düzeltmeyi doğurdu; daha sonraki tam build sonucu belgelenmedi.
- 118 ID tek başına tüm kapsam değildir: 48 paketteki ek GUI ID’leri ve FIX-01–21, özgün paket kabul sütunlarında korunur. Yeni numara uydurulmaz; paketler ve ID’ler mükerrer iş olarak sayılmaz.

## 2. Öncelikli tamamen açık alt işlevler
Aşağıdakiler önceki temel parçalar bulunsa bile hedeflenen yeni davranış olarak tamamlanmamıştır; bunları küçük UI ekleriyle kapatmayın. Tam yokluk iddiası değil, rapor ve seçili kaynak kanıtına göre açık uygulama kapsamıdır.

- **A02** — Normal Home/MainWindow tema seçicisini restart yerine live API’ye bağla; palette atomik validation, local QSS/icon/pixmap/brush/chart cache, açık/lazy widget invalidation ve ölçümlü repolish; selection/scroll/undo/devam eden işlem korunmalı; 20 ardışık geçiş no-write testi.
- **A07** — Gerçek MainWindow ve tüm ana sayfalara injected sysinfo/diag/privileged/userhelper/DBus/firmware/GPU/CPU/scene/lighting fake backend, fake config/state ve bellek Apply; fail-closed host access/exec/write counters/sentinel, permission/offline/timeout/boot/init fixture, heavy lazy lifecycle.
- **A08** — A07 gerçek iki shell ile reviewed baselines, sabit Qt/font/locale/clock, 57 tema component ve pairwise TR/EN/density/100–200 DPI (175 dahil)/3 boyut; clipping/focus/contrast, intentional regression, before/after/diff ve CI. Baselinesiz smoke regression kabulü değil.
- **B03** — Profil overlay/delta plot/linked inspector, gerçek GPU UUID+VBIOS/driver/topology identity, profile metadata, salt okunur Rust structured validator/invalid point (aynı flatten yolu); Apply/Reset/Trial fresh readback transaction, quantization/adjusted/missing/partial/clock cap, bounded persistent history ve Xid boot/GPU/session. Undo no write ve mevcut safety testleri.
- **B07** — Per-device stable identity/hotplug, real provenance/drift/freshness, semantic live/boot/preset diff, merge/replace/coverage, combined filters/hidden checks, setting detail/relationship cycle; gerçek helper per-key requested/skipped/failed/applied-unverified/match/mismatch/restore transaction journal, columns order/visibility/persist, ABBA/boot guard.
- **C02** — Session/incident/device/source/tool graph, typed direct/same-device/temporal/candidate edges, quality/confidence/reason/provenance, filter/zoom/group/selection deep link, raw snapshot/detail/source missing fallback; yeni scan yok.
- **C04** — Stable PCI/USB/NVMe/net identity/hotplug, lazy expand/selection, inspector links, current/max links/NUMA/AER provenance; BDF reuse/cycle/detached/duplicate vendor fixture; dGPU wake yok.
- **C10** — Sleep full boot/session/end AC ve missing resume; comparable cooling power/ambient/temp/fan/duration runs, battery charge/discharge/Wh/capacity/cycles, stable NVMe/SATA raw/normalized/worst/threshold/units history; bounded opt-in shared sampling, no auto stress/self-test/wake.
- Diğer paketlerde de tümüyle eksik alt sistemler: A03 typed async subscription; A04 CO async lifecycle; A05 atomik kaynak registry; B02 dinamik ETA; B11 tam A/B ve run history; C01 cross-session event model; C03 source-aware privacy; C09 unified Raw/Structured model; D01 visual builder/trigger dry-run; D02 efekt timeline/animasyon; D03 path manifest; X06 incremental model; X08 stable process/listening scope. Aşağıdaki tabloda ayrıntılı.

### Seçili doğrudan kaynak kontrolü (Tur 14 ZIP)
| Dosya | Güncel satırlar | Bulgular |
|---|---|---|
| `gui/src/simulation.cpp` | 128 | Canlı API çağrısı izole simulation workspace’te. |
| `gui/src/theme.cpp` | 915 | Canlı API tanımı mevcut; normal shell entegrasyonu ayrı. |
| `gui/src/cosearchdialog.cpp` | 478, 481, 490, 497, 727 | CO bloklayan beklemeler hâlâ var. |
| `gui/src/mainwindow.cpp` | 851, 853 | Observed polling history Scene eventlerine ek olarak sürüyor. |
| `gui/src/curvereadback.h` | 9 | Identity GPU metni + indices/voltages; gerçek UUID ayrımı kanıtlanmış değil. |
| `gui/tests/simulation_visual_test.cpp` | 65, 128 | Baseline environment variable ve koşullu karşılaştırma; reviewed baseline kabulü yok. |
| `gui/src/optimizetab.cpp` | 2158 | Opt-in startup draft fonksiyonu mevcut; iterator hotfix korunmalı. |

## 3. 48 iş paketi — yapılmış olanı koru, kalanını tamamla
Her paketin Tur 5 kalan kapsamı referans olarak ayrıca tutulur; “kalan” metni kapsamı daraltmak için kullanılmaz. Orijinal kabul şartının karşılandığı yeni test/diff ile kanıtlanmadan satırı kapatma.

### A01 — Ortak design system, tam yoğunluk ve kontrast matrisi / P0/P1 / 16, 23, 25–29
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** theme.cpp/.h, hometab.cpp, theme_runtime_test.cpp, tools/audit_palette_contrast.py: palette token contrast matrisi, özel tema kontrolü, seçili metin için kontrastı en yüksek siyah/beyaz renk ve filtreli rapor.
- **Tur 6–14 gerçek ilerleme / sınır:** T8 font/QSS point ölçekleme, T13 adaptif Home grid; önceki 57 palet, kontrast raporu ve compact startup korunur.
- **Kalan iş:** Semantik status/typography/spacing/density/icon/table token API; 737 tarihsel düşük kontrast çiftinin güncel yeniden ölçümü ve düzeltme/istisna; hover/focus/disabled/error/chart/custom rendered contrast; paletten bağımsız OLED seçenekleri, colorblind/desen/ikon, tüm sabit ölçüler, keyboard/accessibility. T8 ölçekleme tam density değildir.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Ortak semantik rol/typography/spacing/density/ikon/table/status API; mevcut contrastChecks ve 57 tema raporunu genişlet, 737 eşik altı token çiftini görsel kimliği bozmadan düzelt veya gerekçeli istisna kaydet; rendered hover/focus/disabled/error/chart contrast ve custom palette. Sabit boyutları font-aware yap, renk dışı durum işaretleri ve OLED tercihlerinin kontrolünü tamamla.
- **Kaynak giriş noktaları:** theme.cpp:contrastChecks; hometab.cpp:Palette Contrast Audit; theme.*, experimentalui.h, tüm GUI
- **Orijinal kabul testi:** 57 palet ve geçerli özel palet için kontrast raporu; %100/125/150/200 DPI ve TR/EN'de temel kontroller taşmaz; kritik durumlar renksiz de anlaşılır; kompakt yoğunluk sabit ölçülü kontrollerde de gerçekten işler.

### A02 — Canlı tema değiştirme / P1 / 17
**Durum:** KISMİ — tam kabul yok. T6–14 hedeflenen esas yeni davranış belgelenmedi.
- **Korunacak Tur 5 parçası:** `theme.cpp/.h`, `charts.cpp`, `simulation.cpp`: 19 renk için sabit adresli storage, doğrulanmış live palette API, revision sinyali ve güncel DayBars renkleri; izole simülasyon ekranında canlı tema.
- **Tur 6–14 gerçek ilerleme / sınır:** T5 izole simulation live palette ve revision; T8 font tercihi restart kullanır. T6–14 normal live theme entegrasyonu yok.
- **Kalan iş:** Normal Home/MainWindow tema seçicisini restart yerine live API’ye bağla; palette atomik validation, local QSS/icon/pixmap/brush/chart cache, açık/lazy widget invalidation ve ölçümlü repolish; selection/scroll/undo/devam eden işlem korunmalı; 20 ardışık geçiş no-write testi.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Normal MainWindow/Home tema seçicisini canlı API’ye geçir; yerel QSS, QIcon/QPixmap, item brush ve QPainter cache invalidate, açık ve lazy widget revizyonu; custom paleti atomik doğrula, hatada eski palet; scroll, selection, undo ve devam eden iş korunsun. Restart yalnız shell/dil gibi gerekli durumlarda. Repolish ölç.
- **Kaynak giriş noktaları:** theme.cpp:applyLivePalette; simulation.cpp çağırıyor; hometab.cpp restart yolu; A01, theme.*, main.cpp
- **Orijinal kabul testi:** açık curve editörü ve model simülasyonunda devam eden işlem sırasında art arda 20 tema değişimi; değer/selection/undo/işlem kaybı, crash veya yeni donanım çağrısı yok. Custom palette hatasında eski tema korunur.

### A03 — Ortak telemetri cache ve geçerlilik modeli / P0/P1 / 218, 219, 229
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** `telemetrycache.h`, `sysinfo.cpp/.h`: Home ve metinsel D-Bus Sensors ortak altı sabit boyutlu display slotu; monotonic/boottime TTL, single-flight, generation invalidation, unavailable/pending/expired/failed ayrımı.
- **Tur 6–14 gerçek ilerleme / sınır:** T6 WMI readback ayrımı; T10 AMD boottime/cihaz snapshot ve firmware timestamp; T12 Wi-Fi age; T13 Home görünürlük polling ve unknown runtime, uzun sweep gap. T5 altı TTL slotu korunur.
- **Kalan iş:** Typed metric/device/unit/boot/source/quality/timestamp/error async subscription, numeric D-Bus ve Health/AMD/NVIDIA/stress/Home/SMART tüketicileri; single-flight bounded queue/cancel/timeout/suspend/AC scheduler, stale grafik gap ve GPU wake opt-in. Safety/post-write direct kalır.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Altı textual slotu metric+device+unit+boot+source+quality+timestamp+last error taşıyan ortak async subscription servisine genişlet. Numeric DBus, Health/AMD/NVIDIA/Home tüketicileri, bounded queue, timeout/cancel, visibility/AC/sleep polling; stale gösterimi ve grafik boşluğu. Safety ve post-write readback doğrudan kalacak; GPU uyandırma opt-in.
- **Kaynak giriş noktaları:** telemetrycache.h:Slot, Sample; sysinfo.cpp:displayClock; sysinfo.*, Home ve sensör tüketicileri
- **Orijinal kabul testi:** Home+Health+stress panelinde aynı metrik istenince tek sorgu; stale/timeout/uyuyan GPU/cihaz değişimi fixture'ları; geç örnek yeni veriyi ezmez; eski sıcaklık canlı diye gösterilmez.

### A04 — Kalan bloklayan çağrılar ve geç sonuç koruması / P0 / 216, 232, FIX-02/03/05
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** Önceki asenkron ağırlık ve Wi-Fi guard + `optimizetab.cpp`: Autotune başlangıçtaki taslak snapshot’ını tamamlanınca yeniden kontrol eder; değişiklik varsa sonucu salt okunur açar. Çıkış kodu/timeout başarısızsa JSON ok true olsa da sonuç yüklenmez.
- **Tur 6–14 gerçek ilerleme / sınır:** T6 firmware/Autotune guard; T7 CO preview revision ve exit+JSON; T9 kernel cursor/revision; T10 firmware worker, backup stat guard; T12 Wi-Fi/Scene generation; T13 pstore bounds; T14 stale row ID re-resolve.
- **Kalan iş:** CO waitForStarted/waitForFinished, start/resume/reset/stress teardown ve diğer sync GUI helper/file/IPC yollarını async yap; journal sırası korunur. Ortak request/device/session/revision/lifetime/cancel, A→B→A, timeout ve restoring busy; 60s fake helper responsiveness ve close/cancel testleri.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** CO başlangıç/resume/reset/stress teardown ve kalan GUI thread sync process/file/IPC yollarını envanterle; asenkronlaştır, request/device/session/revision/QPointer, timeout/cancel ve restoration busy. Mevcut Wi-Fi/Autotune guard korunur; güvenli journal sırası değişmez.
- **Kaynak giriş noktaları:** cosearchdialog.cpp:waitForStarted/waitForFinished (456,459,468,475,700); privileged.cpp; userhelper.cpp; privileged.*, userhelper.*, CO/Optimize
- **Orijinal kabul testi:** mock helper 60 saniye cevap vermediğinde event loop ve Cancel çalışır; geç callback ve kapanmış widget fixture'ları; timeout success değildir. CO crash simülasyonlarının sonuçları değişmemeli.

### A05 — Ortak işlem registry, hata/işlem geçmişi, kaynak çakışma koruması / P0 / 11, 222–226
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** `mainwindow.cpp/.h`, `tray.cpp`, `optimizetab.h`: işlem merkezi gözlemlenen geçiş geçmişi (200 satır), tray ve gerçek pencere çıkışında busy guard; Autotune busy durumuna dahil.
- **Tur 6–14 gerçek ilerleme / sınır:** T12 Scene UUID/adım/ertelenen GPU sonuçları, normal exit guard, Scenes/Spectrum dirty; eski 200 satır observed history korunur.
- **Kalan iş:** Merkezi gerçek event registry (owner/resource/phase/progress/outcome/cancel/restore), atomik read/write rezervasyon, scene owner propagation, dış helper lock, all quit/restart/restore dirty, redacted bounded error/notification history ve page link. CO/trial/cooling/backup/optimization/lighting dahil; restore bitmeden lock bırakma.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Polling geçmişi yerine olay tabanlı gerçek operation registry: ID, owner, kaynak, phase, progress, result, restore, cancel capability. Atomik read/write rezervasyonları, scene transaction owner propagation, dış helper lock gözlemi, bütün normal quit/restart/restore yolları + dirty kontrolü. Bounded redacted hata/bildirim geçmişi, ilgili sayfa linki. Unknown outcome success değildir.
- **Kaynak giriş noktaları:** mainwindow.cpp:operationHistory_; tray.cpp; scenes.cpp:stepState; A04, mainwindow.*, scene/test/helper akışları
- **Orijinal kabul testi:** eşzamanlı CO/scene/restore çakışma testleri, restoration bitmeden kaynak bırakılmaması, bağımsız salt okunur sorguların çalışması, işlem geçmişinin gerçek result ile eşleşmesi, yarım işlemin başarı göstermemesi.

### A06 — Ortak grafik etkileşimi ve zoom / P1 / 228
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** `charts.cpp/.h`: LineChart zoom, klavyeli pan/reset, görünür/tüm CSV, finite sample ve gap kontrolü, min/max bucket.
- **Tur 6–14 gerçek ilerleme / sınır:** T9 chartaxis range/tick, signed bars, LineChart visible data table, CSV gap, DayBars keyboard; native bağımsız test PASS.
- **Kalan iş:** Bar/DayBars/VF ve multi-series ortak units/time transform, nearest-point/legend/accessible table/visible-all export, V/F edit semantics korunarak zoom hit-test. 100k sample Qt performans, CSV roundtrip ve edge fixture.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** LineChart zoom/pan/gap/downsample/CSV korunarak ortak axis/range/units/time transform ve tick API; BarChart/DayBars/VF adaptasyonu, multi-series legend, erişilebilir tablo, nearest-point; visible/all export, zoom sadece görünüm. 100k performance, NaN/inf, tek nokta/negatif/küçük aralık testleri.
- **Kaynak giriş noktaları:** charts.cpp:bucket, setViewRange; vfcurvewidget.cpp; charts.*, vfcurvewidget.*
- **Orijinal kabul testi:** 100k örnekte akıcı çizim, kayıp veri boşlukları, tek nokta/negatif değer/çok küçük pencere, CSV roundtrip, V/F mevcut güvenlik testleri.

### A07 — Donanım simülasyon modu / P1 / 234
**Durum:** KISMİ — tam kabul yok. T6–14 hedeflenen esas yeni davranış belgelenmedi.
- **Korunacak Tur 5 parçası:** `main.cpp`, `simulation.cpp/.h`: `--simulate` ile normal shell/instance/socket/daemon/helper yollarından ayrı izole workspace; geçici XDG ve fixture kökü, 6 senaryo, gerçek V/F ve LineChart, S10/S13 gerçek parser, simulated Apply sadece bellek.
- **Tur 6–14 gerçek ilerleme / sınır:** T5 altı fixture ve izole --simulate workspace, mevcut lazy altyapı; T6–14 gerçek shell injection eklenmedi.
- **Kalan iş:** Gerçek MainWindow ve tüm ana sayfalara injected sysinfo/diag/privileged/userhelper/DBus/firmware/GPU/CPU/scene/lighting fake backend, fake config/state ve bellek Apply; fail-closed host access/exec/write counters/sentinel, permission/offline/timeout/boot/init fixture, heavy lazy lifecycle.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Ayrı güvenli simulation workspace’i koru; gerçek MainWindow ve bütün ana sayfaları injected sysinfo/diagnostics/privileged/userhelper/DBus/firmware/GPU/CPU/scene/lighting backend ile çalıştır. Fake config/state, fake Apply bellek içi; fail-closed host access, permission/timeout/offline/boot/init fixtures, host sentinel ve exec/write counters.
- **Kaynak giriş noktaları:** main.cpp:--simulate; simulation.cpp; lazywidget.h; A03–A05, lazywidget.h
- **Orijinal kabul testi:** gerçek sistem erişimlerini sayan fake backend ile sıfır privileged çağrı/yazma; host dosya sentinel'leri değişmez; 5+ donanım fixture'ı ile her ana sayfa açılır; hata ve yarış koşulu testi yapılır.

### A08 — Gerçek görsel regresyon altyapısı / P0 / 235, 28
**Durum:** KISMİ — tam kabul yok. T6–14 hedeflenen esas yeni davranış belgelenmedi.
- **Korunacak Tur 5 parçası:** `simulation_visual_test.cpp`, CMake/CI, `GUI-SIMULATION-VISUAL-TESTS.md`: 4 tema, 6 fixture, 2 panel, 2 boyut, 4 DPI, TR/EN ve iki shell-style seçimi; pairwise; repeat-render ve optional reference pixel comparison, geometry, PNG artifact.
- **Tur 6–14 gerçek ilerleme / sınır:** T5 optional PNG baseline/repeat-render smoke, 4 tema/6 fixture/iki panel; T6–14 onaylı gerçek shell baseline yok.
- **Kalan iş:** A07 gerçek iki shell ile reviewed baselines, sabit Qt/font/locale/clock, 57 tema component ve pairwise TR/EN/density/100–200 DPI (175 dahil)/3 boyut; clipping/focus/contrast, intentional regression, before/after/diff ve CI. Baselinesiz smoke regression kabulü değil.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Var olan native harness üzerine gerçek Classic/Experimental ana sayfaların onaylı PNG baseline’ları; sabit Qt/font/locale/clock, 57 palette component test ve pairwise TR/EN, density, %100–200 DPI, 900×600/1280×800/1920×1080. Clipping/focus/contrast assertions, intentional regression, before/after/diff artifacts; baseline yoksa smoke ayrı, başarılmış regression değil.
- **Kaynak giriş noktaları:** gui/tests/simulation_visual_test.cpp:CENTURION_VISUAL_BASELINES; .github/workflows/ci.yml; A01–A07, gui/tests, CI
- **Orijinal kabul testi:** bilerek kırpılmış label, yanlış kontrast, küçük pencere taşması, theme refresh hatası testte yakalanır; CTest ve GitHub CI gerçek Qt native testlerini çalıştırır.

### A09 — Açılmamış tüm ayarların indekslenmesi, fuzzy arama / P1 / 01–02
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** `uinavigator.cpp`, `toolcatalog.cpp/.h`, `optimizetab.cpp/.h`, `tuningnavigation.h`, `tools/generate_navigation_index.py`: 279 optimization metadata kaydı, fuzzy arama, stable key ile focus ve mevcut alt tab indeksleri.
- **Tur 6–14 gerçek ilerleme / sınır:** T14 stable ID metadata: mevcut firmware satırları, Ryzen CCD/slot, Intel, NVIDIA, Home, 19 AMD aday, ranking/alias/bounds, duplicate ve stale row; 279 optimization index korunur.
- **Kalan iş:** Gerçekten henüz oluşmamış dinamik firmware/CPU/GPU/fan ve tüm lazy tool/setting metadata (metadata provider; aramada constructor/helper yok); global ID uniqueness, TR/EN synonyms/risk/capability, Health route birleştirme, lazy/helper counter ve Qt focus fixtures. T14 static AMD aday destek kanıtı değildir.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** 279 optimization kaydı/fuzzy ranking korunur. Açılmamış firmware, CPU/GPU/fan ve tüm alt tool/setting metadata’sı, stable ID/focus, TR/EN alias/risk/capability; duplicate/limit ve lazy constructor/helper counter fixture. Health deep-link yollarını birleştir.
- **Kaynak giriş noktaları:** uinavigator.cpp; tuningnavigation.h; tools/generate_navigation_index.py; uinavigator.*, toolcatalog.*, tuning metadata
- **Orijinal kabul testi:** hiç açılmamış setting ve tool bulunur, typo/alias eşleşir, aynı ID tek sonuç, lazy widget sayaçları arama sırasında artmaz.

### A10 — Dashboard panel sıralaması ve tam panel görünürlüğü / P2 / 31
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** `hometab.cpp`: experimental panel order, klavyeli sıralama, reset, iki opsiyonel panel görünürlüğü ve reflow hysteresis.
- **Tur 6–14 gerçek ilerleme / sınır:** T13 Classic bağımsız order/visibility/reset, ortak hysteresis grid, Power/Device gizlenemez; eski Experimental ayarları korunur; görünür sensör getter azalır.
- **Kalan iş:** Tüm güvenli panel visibility kombinasyonları, geri erişim ve capability-aware compact fan/MUX/panel/battery/core parking row, gizli polling A03, iki shell restart/migration/DPI/resize/no-favorites testleri.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Experimental sıralama ve iki panel görünürlüğü korunur; Classic için bağımsız tercih/migration, tam güvenli panel görünürlüğü/geri erişim, capability-aware compact device controls ve adaptif grid, gizli sensör polling azaltma; restart/DPI test. Kritik kontrolleri kaybettirme.
- **Kaynak giriş noktaları:** hometab.cpp; experimentalui.h; hometab.*, experimentalui.h
- **Orijinal kabul testi:** sıralama ve görünürlük restart sonrası korunur, hiç favori eklenmediyse boş kalır, iki UI modunun tercihleri birbirini bozmaz, Home gizli sensörler gereksiz sorgulanmaz.

### B01 — Tam CCD stability ve kalıcı CO oturum arşivi / P1 / 43–46, 51, 57
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** `coarchive.h`, `cosearchdialog.cpp/.h`: mevcut v2 oturum arşivi atomik, kilitli, 40 oturum / 1 MiB sınırlı; aşırı büyük kayıtların eski denemeleri açık sayaçla çıkarılır; bozuk kayıt üzerine yazılmaz. CCD/slot/logical CPU, arama durumu, son başarısız değer, doğrulama ve reset bilgisi görüntülenir. Aktif journal değiştirilmedi.
- **Tur 6–14 gerçek ilerleme / sınır:** T7 CPU/topoloji fingerprint, CCD/slot archive diff, filtre/export/delete; bounded mevcut arşiv/journal korunur.
- **Kalan iş:** Live SMT/offline/V-Cache kanıtı, kanıta dayalı stability heatmap (contradictory/exclusion), session/boot/event/candidate stable IDs ve filtreler, ryzentab profil semantik diff (all/per-core), migration/corruption/reboot fixture; aktif journal dışarıda.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Mevcut bounded CO archive/CCD inspector üzerine CPU/topology fingerprint + session/boot identity ve doğrulanmış SMT/V-Cache metadata; kanıt temelli stability heatmap, çelişkili evidence, core/candidate filter, çoklu session diff, archive export/delete (aktif journal hariç), CCD/slot semantik profil diff; migration ve corruption fixture.
- **Kaynak giriş noktaları:** coarchive.h; cosearchdialog.cpp:showSessionArchive; costrategy.h; cosearchdialog.*, costrategy.*, coevidence.*, coinsights.*, profilediff.h
- **Orijinal kabul testi:** iki oturum, CPU topolojisi değişmiş profil, reboot sonrası kayıt ve bozuk arşiv fixture'ı; seçim/filtre aktif denemeyi etkilemez; eski journal migration ve crash testleri geçer.

### B02 — Gerçek karar açıklaması, resume preview, dinamik ETA / P0/P1 / 47, 59, 60
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** `costrategy.cpp/.h`, `cosearchdialog.cpp`: Search kopyası üzerinde const preview, resume'da sonraki aday ve test süresi.
- **Tur 6–14 gerçek ilerleme / sınır:** T7 typed decision, exclusion boundary, recorded search value, const preview ve journal/guard revision; mevcut CO native regressions PASS.
- **Kalan iş:** Tam gerçek algoritma branch reason, trustworthy last value/guard replay, dispatch-preview eşitliği ve gerçek event timeline; Trial::seconds+overhead+remaining range dinamik ETA (unknown, retry, terminal zero); Qt recovery fixture; crash guard algoritması değişmez.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Const preview ve resume candidate korunur; gerçek algoritma dallarından typed decision/reason, guard exclusion/replay ve güvenilir son değer; journal revision ile preview invalidate; gerçek event timeline ve Trial::seconds + overhead + kalan deneme aralığıyla dinamik ETA/unknown. Crash guard ve replay kuralları değişmez.
- **Kaynak giriş noktaları:** costrategy.h:previewNext; cosearchdialog.cpp:1455; costrategy.*, cosearchdialog.*
- **Orijinal kabul testi:** preview çağrısı öncesi/sonrası `save()` eşit; clean replay ve crash sonrası aday gerçek dispatch ile eşleşir; binlerce fixture üzerinden crash guard sınırları ihlal edilmez, ETA sonlanan işlemde sıfır, aktif retry'da sahte tamamlanma yok.

### B03 — V/F eksik katman, validation, kalıcı history ve transaction readback / P0/P1 / 61–64, 66–68
**Durum:** KISMİ — tam kabul yok. T6–14 hedeflenen esas yeni davranış belgelenmedi.
- **Korunacak Tur 5 parçası:** `nvidiatab.cpp/.h`, `curvereadback.h`, `nvcurve-root-helper.rs`: request UUID ile işlem sonuçlarını eşleştirme, gerçek GPU nokta indeksleri ve uV düzeni, apply öncesi helper kilidi altında model adı/nokta kontrolü, eski veya geçersiz readback reddi, reset sonrası gerçek offset okuması.
- **Tur 6–14 gerçek ilerleme / sınır:** T5 request UUID, model+index+uV layout, fresh reset readback, draft undo; T14 yalnız NVIDIA arama metadata. V/F esas eksikleri açık.
- **Kalan iş:** Profil overlay/delta plot/linked inspector, gerçek GPU UUID+VBIOS/driver/topology identity, profile metadata, salt okunur Rust structured validator/invalid point (aynı flatten yolu); Apply/Reset/Trial fresh readback transaction, quantization/adjusted/missing/partial/clock cap, bounded persistent history ve Xid boot/GPU/session. Undo no write ve mevcut safety testleri.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Profil overlay, delta plot, linked point inspector ve profile metadata; UUID+VBIOS/driver/topology fingerprint (aynı model farklı GPU ayrımı); Rust validator salt okunur structured invalid point/reason, aynı flatten hesaplama yolu. Apply/Reset/Trial fresh readback transaction snapshot, gerçek quantization/adjusted/missing/partial ve clock limit ayrımı, kalıcı bounded operation journal, Xid boot/GPU/session ilişkisi; GUI ve helper birlikte.
- **Kaynak giriş noktaları:** curvereadback.h:Identity model+indices+voltages; nvidiatab.cpp:reportClamping; nvcurve-root-helper.rs; nvidiatab.*, vfcurvewidget.*, nvcurve crate
- **Orijinal kabul testi:** helper invalid point fixture grafikte doğru nokta; eski readback yeni işlemi doğrulamaz; GPU değişimi ve başarısız/adjusted readback; Undo/Redo hiçbir donanım yazması yapmaz; mevcut flatten/safety testleri korunur.

### B04 — Firmware Power Limit Hierarchy / P2 / 84
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** systemdiagnostics.cpp: mevcut güç raporunda kaynak hiyerarşisi, birim ve missing state ayrımı.
- **Tur 6–14 gerçek ilerleme / sınır:** T10 mevcut S08 Power Limit Sources raporu worker/read-only firmware inspector, kaynak/timestamp/platform profili ve default ayrımı.
- **Kalan iş:** Gerçek PL/PPT/STAPM live/draft/default dependency ve enforcement provenance, transport/WMI error/units; unknown ilişkileri açık, QThread lifecycle/no-write/multi-model Qt fixture. Kaynak sırası enforcement değildir.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** S08 source gruplamasını doğrudan firmware inspector’a bağla; platform profile, live/draft/default, PL/PPT/STAPM ve gerçek dependency metadata; ilişki yoksa unknown, kaynak sıralamasını enforcement hierarchy sanma. WMI hata/ünite fixture.
- **Kaynak giriş noktaları:** systemdiagnostics.cpp:Power Limit Sources; fwattrtab.cpp; fwattrtab.*, platformprofile.*, helper capabilities
- **Orijinal kabul testi:** farklı model/eksik limit fixture'ı, WMI erişim hatası, birim dönüşümü; hierarchy açılırken write helper çağrısı yok.

### B05 — Intel Voltage Domain ve P/E topology / P1 / 85–86
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** inteltab.cpp/.h, systemdiagnostics.cpp: son helper mailbox verisiyle voltage domain ve CPU package/core/SMT inceleyicisi.
- **Tur 6–14 gerçek ilerleme / sınır:** T10 ortak CPU list/reciprocal SMT, die/online contradiction; T14 Intel metadata; önceki mailbox domains.
- **Kalan iş:** Gerçek güvenilir core_type/helper P/E sınıflandırması, voltage domain coupling/support ve power/voltage details deep-link, package/core/SMT/online integration, unsupported guard, hybrid/nonhybrid fixtures.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Mevcut Intel mailbox ve CPU topology raporunu gerçek domain coupling/support, package/core/P/E/SMT/online, domain power/voltage detail deep link ile tamamla. P/E yalnız reliable core_type/helper, fallback ve unsupported write guard.
- **Kaynak giriş noktaları:** inteltab.cpp:Voltage Domains and CPU Topology; systemdiagnostics.cpp:cpuTopology; inteltab.*, sysinfo.*, mevcut Intel helper
- **Orijinal kabul testi:** P/E + SMT, hibrit olmayan ve kısıtlı helper fixture'ları; mevcut undervolt editör ve değerleri aynı kalır; unsupported domain'e yazma düğmesi açılmaz.

### B06 — AMD GPU Frequency Visualization / P2 / 90
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** amdgputab.cpp/.h: seçili kartın son örneğine bağlı core/VRAM frekans, mevcut DPM/OD noktaları, draft aralık BarChart; kart değişiminde/eski veride temizleme.
- **Tur 6–14 gerçek ilerleme / sınır:** T10 gerçek DPM core/VRAM durumları, boottime stale ve cihaz/uyku guard; explicit sysfs read. T14 metadata.
- **Kalan iş:** A03 ortak async identity/freshness; hotplug stable identity, capability-aware same editor validation/min-max interaction, multi-GPU/min>max/missing DPM/uyuyan dGPU fixtures. DPM okuması GUI thread, no-wake garanti yok.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Seçili kartın frequency overview’ını shared telemetry freshness/identity ve gerçek min/max/DPM/core/VRAM/draft ile tamamla; capability varsa aynı editor validation üzerinden etkileşim, yoksa read-only; multi-GPU, min>max, sleep fixture.
- **Kaynak giriş noktaları:** amdgputab.cpp:frequencyOverview; amdgpu.*, amdgputab.*
- **Orijinal kabul testi:** iki kart arasında seçim, farklı aralıklar, min>max invalid, eksik DPM, uyuyan dGPU; read-only graph hiçbir yazma başlatmaz.

### B07 — Optimization source/conflict/device/detail ve işlem journal'ı / P0/P1 / 95, 100, 103–104, 106–107, 110–111
**Durum:** KISMİ — tam kabul yok. T6–14 hedeflenen esas yeni davranış belgelenmedi.
- **Korunacak Tur 5 parçası:** `optimizetab.cpp/.h`: son helper snapshot ve gerçek policy metadata üzerinden source/scope/readback, conflict/coverage ve drift inceleyicisi; kanıtı olmayan kaynak unknown.
- **Tur 6–14 gerçek ilerleme / sınır:** T5 source/conflict/coverage/state matrix ve provenance unknown; T6–14 esas per-key backend/journal tamamlanmadı.
- **Kalan iş:** Per-device stable identity/hotplug, real provenance/drift/freshness, semantic live/boot/preset diff, merge/replace/coverage, combined filters/hidden checks, setting detail/relationship cycle; gerçek helper per-key requested/skipped/failed/applied-unverified/match/mismatch/restore transaction journal, columns order/visibility/persist, ABBA/boot guard.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Source/conflict workspace korunur; stable per-device inspector/hotplug, gerçek provenance ve freshness, semantic preset vs live/boot diff, coverage/merge/replace, conflict/dependency ve birleşik filters. Gerçek per-key helper result requested/skipped/failed/applied-unverified/match/mismatch + transaction journal/boot-live scope ve kolon order/visibility/persistence; ABBA/boot guard.
- **Kaynak giriş noktaları:** optimizetab.cpp:sourceWorkspace; driftwatch.*; tune-helper; optimizetab.*, tuningviewstate.h, driftwatch.*, tune-helper
- **Orijinal kabul testi:** boot preset yüklü ama checkbox boş, başka servis drift, unsupported per-NVMe, kısmi helper hata, stale readback, yeniden açılışta kolonlar; ABBA ve boot guard testleri geçer.

### B08 — Restart sonrası kalıcı optimization draft / P1 / 102
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** optimizetab.cpp/.h: manuel atomik sürümlü VM/scheduler draft save/restore/delete, fingerprint, preview, live/boot mismatch, restore sonrası checkbox boş.
- **Tur 6–14 gerçek ilerleme / sınır:** T7 opt-in VM/scheduler draft startup restore ilk fresh full readback sonrası, unchecked; T10 constBegin/constEnd hotfix.
- **Kalan iş:** Diğer scope/per-device stable fingerprint, schema/revision migration, invalid/missing/discard/reset/live-boot mismatch, startup no tuning write ve restart fixture. Mevcut VM/scheduler atomik draft korunur.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Manuel VM/scheduler draft save/restore korunur; opt-in startup restore, diğer scope/per-device ancak stable fingerprint ile, revision/schema migration, invalid/missing reporting, discard/reset ve live/boot conflict. Startup/restore hiçbir ayarı uygulamaz; checkbox bağımsız.
- **Kaynak giriş noktaları:** optimizetab.cpp:draftWorkspace; B07
- **Orijinal kabul testi:** reboot/restart sonrası taslak korunur, boot/live değer değişmişse mismatch gösterilir, bozuk draft'ta güvenli fallback, start'ta sıfır tuning yazması.

### B09 — Etkileşimli weight/coverage/validity ve kalibrasyon yönetimi / P1 / 113, 116–117, 122
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** `calibrationcoverage.h`, `optimizetab.cpp`: v10 gerçek y alanında sonlu ölçüm sayıları; phase/kernel/context/power-source grupları, dört hedef için ayrı kayıt sayısı ve bilinmeyen/nonpositive weight.
- **Tur 6–14 gerçek ilerleme / sınır:** T11 dokuz eksen radar ve effective snapshot, v10 salt okunur signature inspect/export SHA, product/CPU/BIOS/RAM karşılaştırma ve benchmark/date.
- **Kalan iş:** Gerçek model validity/coverage requirements/freshness/quality/context/kernel/BIOS; preset overlay ve gerçek sonraki solver weights eşitliği; tam schema doğrulama, desteklenen passive session management, kontrollü import/atomic backup/rollback ve active calibration/ABBA mutation guard; GUI doğrudan signature yazmaz.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Coverage count tablosu üzerine modelin gerçek ölçüm gereksinimleri/freshness/quality/validity ve kernel/BIOS/hardware mismatch gerekçesi; legal weight radar/preset overlay/numeric edit. Sürüm doğrulamalı export/import, desteklenirse pasif session yönetimi, atomic backup/rollback, aktif calibration/ABBA sırasında mutation yok.
- **Kaynak giriş noktaları:** calibrationcoverage.h; optimizetab.cpp:editAutotuneWeights; optimizetab.*, autotunereport.*, signature v10
- **Orijinal kabul testi:** v10 gerçek fixture, eski/bozuk/yeni bilinmeyen schema, fingerprint mismatch, eksik context, legal weight maksimumları; yönetim işlemi modele gizlice yeni ölçüm yazmaz.

### B10 — Autotune Progress Timeline / P0 / 118
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** `optimizetab.cpp/.h`, `centurion-autotune.rs`: mevcut gerçek CENTURION_STAGE stderr olaylarını süreli zaman çizelgesinde gösterir, sıra dışı/tekrarlı aşamaları geçer, hata kuyruğunu sınırlar; CLI --progress opt-in. Yüzde uydurulmaz; sonuç ancak başarılı exit + ok JSON ile kabul edilir.
- **Tur 6–14 gerçek ilerleme / sınır:** T6 opt-in structured Autotune progress run ID/sequence, eski CENTURION_STAGE fallback ve başarılı exit+JSON guard.
- **Kalan iş:** Gerçek solver alt event kapsamı ve biliniyorsa done/total, registry cancel/restore, malformed/late ordering, stdout uyumu ve Rust/Qt native run fixtures. Bilinmeyen total indeterminate.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Mevcut CENTURION_STAGE ve elapsed timeline yerine/üzerine geriye uyumlu opt-in structured event channel: run/session ID, sequence, phase, done/total yalnız biliniyorsa. Rust gerçek solver events, bounded parser, malformed/late ignore, registry progress ve güvenli cancel; stdout result protokolü bozulmasın.
- **Kaynak giriş noktaları:** optimizetab.cpp:CENTURION_STAGE; centurion-autotune.rs; centurion-autotune.rs, autotune.rs, A05
- **Orijinal kabul testi:** aşamalar gerçek solver olaylarıyla eşleşir; profiling total bilinmiyorsa indeterminate; iptal/başarısızlıkta %100 başarı göstermez; mevcut solver çıktısı ve testleri geriye uyumlu.

### B11 — Model belirsizliği, ölçümlü A/B, seçilebilir öneri ve history / P0/P1 / 119–123, 125–127
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** `optimizetab.cpp`, `autotunereport.cpp`: seçili gerçek önerileri yalnızca taslağa alma, mevcut Apply/ABBA akışını koruma, seçilebilir scope ve overlap kontrolü; hatalı/eksik skor ve uncertainty ayrımı.
- **Tur 6–14 gerçek ilerleme / sınır:** T11 predcheck mu/sd/obs/error/z ve filter/graph, T5 selective staging/invalid score guard.
- **Kalan iş:** Kalıcı run/report history, run-context workspace, structured objective/evidence/constraint/fallback/ranking, recommendation-level interval/evidence sufficiency, interactive alternative tradeoff, same-context paired benchmark/game A/B, stable recommendation ID/dependency/revalidation ve Apply/ABBA scope. Predcheck utility FPS/başarı olasılığı değil.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Selective staging ve invalid score ayrımı korunur; gerçek objective/evidence/constraints/ranking/uncertainty metadata ve alternative-weight tradeoff graph, same-context paired A/B/game session ve metric units, measured/model ayrımı, prediction error; persistent report/run history; selected recommendation ID/scope/dependency revalidation mevcut Apply/ABBA ile.
- **Kaynak giriş noktaları:** optimizetab.cpp:Keep only selected recommendations checked; autotunereport.cpp; B09–10, Autotune report, mevcut frametime.h
- **Orijinal kabul testi:** Explore ile eski draft karışmaz; bir öneri seçildiğinde yalnızca o scope değişir, karşılıklı bağımlılıkta apply engeli/uyarı; karşılaştırılmayan A/B, yetersiz ölçüm ve eski schema fixture'ları.

### C01 — Kalıcı cross-boot diagnostik oturumları ve semantik diff / P1 / 128, 130–131, 134, 136, 151
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** diagreport.cpp/.h, diagreportview.cpp/.h: rapor başına 20 snapshot/1 MiB limit, manuel kayıt ve silme, local QLockFile/QSaveFile, raw/fix/binary hariç, locale ve capture boot ayrımı; sıra bağımsız multiset diff.
- **Tur 6–14 gerçek ilerleme / sınır:** T6 two-boot; T9 new full kernel UUID/legacy prefix; T12 Scene journal UUID/outcome; T13 boot state. T5 bounded report archive/multiset diff.
- **Kalan iş:** Cross-boot stable event/session/device/source/quality, adapters flightrec/kernel/pstore/curve/scene/sleep, idempotent import/dedup, clock uncertainty, semantic finding/row keys, source/device/time/review filters, notes/raw links/retention, raw-tool snapshot diff; legacy prefix aday ayrı ve correlation neden değil.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Mevcut per-report archive/multiset diff’i session ID + full boot/device/source/quality/event ID ile genişlet; cross-boot timeline adapters (flightrec/kernel/pstore/curve/scene/sleep), idempotent import, clock uncertainty, filtre, raw link, notlar. Stable row/finding keys ve device provenance ile semantic diff, retention ve geçmiş navigation; prefix-only candidate ayrı.
- **Kaynak giriş noktaları:** diagreportview.cpp:archiveWorkspace; diagreport.cpp snapshot schema; healthevidence.*, evidencereports.*, diagreportview.*, flightrec.*
- **Orijinal kabul testi:** iki boot ve iki cihazlı fixture, prefix çakışması, duplicate event, eski/bozuk rapor, değişen tablo sırası, stale data; session yeniden açılabilir, notlar doğru olayda kalır, correlation kesin neden olarak etiketlenmez.

### C02 — Evidence Relationship Graph / P1 / 129
**Durum:** KISMİ — tam kabul yok. T6–14 hedeflenen esas yeni davranış belgelenmedi.
- **Korunacak Tur 5 parçası:** `diagreportview.cpp/.h`: mevcut raporun doğrudan bulgu/tablo/satır/limit ilişkileri, filtreli ve boyut sınırlı salt okunur ağaç, seçili detaylar; redacted snapshot, yeni tarama yok.
- **Tur 6–14 gerçek ilerleme / sınır:** T5 bounded report-local relationship tree/detail; T6–14 session/incident graph yeni implementasyonu belgelenmedi.
- **Kalan iş:** Session/incident/device/source/tool graph, typed direct/same-device/temporal/candidate edges, quality/confidence/reason/provenance, filter/zoom/group/selection deep link, raw snapshot/detail/source missing fallback; yeni scan yok.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Mevcut bounded report relation tree’yi session/incident/device/source/tool graph’a genişlet; typed edges direct/same-device/temporal/candidate, gerekçe ve provenance, filter/zoom/group/selection deep link; source missing fallback, yeni tarama yok.
- **Kaynak giriş noktaları:** diagreportview.cpp:Evidence Relationships; C01, diagreport.*, toolcatalog.*
- **Orijinal kabul testi:** ilişkisiz loglar bağlanmaz; boot prefix çakışması ayrı etiketlenir; node silinmiş/veri eksikse güvenli fallback; graph oluşturmak yeni donanım taraması başlatmaz.

### C03 — Alan düzeyinde rapor seçimi ve anonimleştirme / P0 / 142–143
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** `diagreport.cpp/.h`, `diagreportview.cpp`: bulgu/tablo seçimi, raw opt-in, orijinal verdict korunması, belirli tablo kolonları ve metinde best-effort redaction, düzenlenebilir tek snapshot, QSaveFile. İkili EDID tablosu artık dil-bağımsız privateBinary metadata ile varsayılan export ve history dışında.
- **Tur 6–14 gerçek ilerleme / sınır:** T7 frozen row/column/export selection, Copy/Save ortak preview, SHA-256 ve labeled UUID redaction; privateBinary hariç.
- **Kalan iş:** Granular evidence/source metadata seçim ve excluded sections, source-aware hostname/path/serial/MAC/IP/UUID tam fixture, raw conservative/manual override/uyarı, actual clipboard/file aynı frozen bytes/hash ve refresh race testi; teknik Xid/PCI/MCE korunur.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Bulgu/tablo ve raw seçim/tek editable snapshot korunur; row/column/evidence/source metadata seçimi, excluded sections, source-aware hostname/path/serial/MAC/IP/UUID redaction, raw conservative + manual override ve uyarı. Preview/copy/save aynı frozen snapshot/hash, refresh race test; privateBinary hariç.
- **Kaynak giriş noktaları:** diagreportview.cpp:previewExport; diagreport.cpp; diagreportview.*, reportpreview.*
- **Orijinal kabul testi:** fixture içindeki bilinen özel değerler export'ta bulunmaz, teknik Xid/MCE/PCI adresi gereksiz yere silinmez; preview/copy/save aynı hash; işlem sırasında report refresh önceki export'u değiştirmez.

### C04 — Grafiksel Hardware Device Tree ve PCIe topology / P1 / 145, 153
**Durum:** KISMİ — tam kabul yok. T6–14 hedeflenen esas yeni davranış belgelenmedi.
- **Korunacak Tur 5 parçası:** systemdiagnostics.cpp/.h, diagreport.h, diagreportview.cpp: canonical PCI/USB/NVMe/network ilişkileri, filtreli hierarchy, eski PCIe scan alanları kimlik kontrolüyle kullanılır.
- **Tur 6–14 gerçek ilerleme / sınır:** T5 canonical device tree ve PCIe delta; T10 CPU topology ortaklığı cihaz graph tamamlanması değil.
- **Kalan iş:** Stable PCI/USB/NVMe/net identity/hotplug, lazy expand/selection, inspector links, current/max links/NUMA/AER provenance; BDF reuse/cycle/detached/duplicate vendor fixture; dGPU wake yok.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Canonical PCI/USB/NVMe/net tree üzerine stable identity/hotplug, lazy expand/selection persistence, inspector deep links ve current/max links/NUMA/AER provenance. BDF reuse/cycle/detached fixture, dGPU wake yok.
- **Kaynak giriş noktaları:** systemdiagnostics.cpp:deviceTopology; diagnostictools.*, advanceddiagnostics.*, diagsources.*
- **Orijinal kabul testi:** nested PCIe bridge, detached USB/NVMe, iki aynı vendor/device, link unknown, symlink cycle fixture; mevcut delta identity ve AER unknown testleri korunur.

### C05 — EDID binary inspector ve VRR zamanlama görselleştirmesi / P1 / 152
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** `diagnostictools.cpp`: salt okunur EDID hex/ASCII, byte offset, base DTD active/blank/total/clock/refresh ve CTA block boundary düzeltmesi.
- **Tur 6–14 gerçek ilerleme / sınır:** T7 EDID connector/byte/field/checksum, CTA declared bounds ve bozuk VRR reddi; parser native PASS.
- **Kalan iş:** Verified timing/VRR grafik, byte selection synchronization ve connector disconnect, çok bloklu/truncated/checksum/unknown range Qt fixture, optional raw export privacy. Override write yok; genel range VRR değildir.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Mevcut hex/ASCII/timing ve CTA bounds korunur; byte seçimi, offset/checksum/extension/descriptor inspector, connector seçimi ve doğrulanmış timing/VRR grafik; genel range VRR kanıtı değildir. İsteğe bağlı raw export privacy warning; override write yok.
- **Kaynak giriş noktaları:** diagnostictools.cpp; diagsources.cpp; diagnostictools.*, mevcut EDID parser
- **Orijinal kabul testi:** 1/2/çok bloklu, checksum mismatch, truncated CTA, genel refresh range fakat VRR yok, connector disconnect fixture; byte seçimi doğru alanı gösterir.

### C06 — Memory Architecture View / P1 / 154
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** systemdiagnostics.cpp/.h: CPU package/core ve sibling bilgisi, NUMA memory/distance, unknown RAM/PSI ayrımı.
- **Tur 6–14 gerçek ilerleme / sınır:** T7 cache shared CPU/SMBIOS slot capacity/rate; T10 ortak CPU topology validation; T5 NUMA/distance.
- **Kalan iş:** Doğrulanmış CCD/cache/online/SMBIOS slot data entegrasyonu ve architecture graph, memory scope/units, container/partial/Intel hybrid/NUMA fixtures, swap/zram ayrı; eksik veri unknown.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** CPU package/core/NUMA/distance üzerine cache shared_cpu_list, doğrulanmış CCD/online, güvenilir SMBIOS slot/speed/capacity ve memory architecture graph. Swap/zram ayrı; unknown ve container/partial topology fixture.
- **Kaynak giriş noktaları:** systemdiagnostics.cpp:cpuTopology; sysinfo.*, diagsources.*, memorydialog.*
- **Orijinal kabul testi:** tek NUMA, iki node, AMD cache-sharing, Intel hybrid, container/eksik sysfs; toplamlar yalnızca aynı scope ve birimde, unknown değerleri sıfırla doldurma.

### C07 — ACPI / WMI Explorer / P2 / 155
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** systemdiagnostics.cpp, diagreportview.cpp: kernel exposed ACPI/WMI device tree, instance ve GUID, driver ve status; salt okunur.
- **Tur 6–14 gerçek ilerleme / sınır:** T7 bounded ACPI header list; T5 exposed ACPI/WMI hierarchy.
- **Kalan iş:** Filtreli event/driver/GUID bağlantısı, gerçek table listing ve duplicate/permission/missing/detached fixtures; AML namespace uydurma, method/EC/WMI execution/escalation yok.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Exposed ACPI/WMI tree’ye filtreli event/driver/GUID bağları ve okunabiliyorsa bounded DSDT/SSDT table list; full AML namespace iddia etme, method/EC/WMI execute veya escalation yok.
- **Kaynak giriş noktaları:** systemdiagnostics.cpp:Exposed ACPI / WMI Device Hierarchy; acpievents.cpp; systools.*, systemdiagnostics.*, acpievents.*
- **Orijinal kabul testi:** eksik ACPI/WMI, permission denied, duplicate GUID, GUID formatı, detached driver; listeleme herhangi bir method execution yapmaz.

### C08 — USB-C PD Visualization / P2 / 156
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** systemdiagnostics.cpp: USB-C port/partner ve supply gerçek ölçüm/kapasite ayrımı, signed voltage/current power; bilinmeyen kontrat varsayılmıyor.
- **Tur 6–14 gerçek ilerleme / sınır:** T7 USB-C PD source profile parser, ölçüm/capacity ayrımı; T5 role/partner/supply.
- **Kalan iş:** Gerçek reported contract/profile ile partner ilişkisi görselleştirme, role swap/disconnect/barrel/negative current fixture; capability/contract/consumption ayrı, bilinmeyen contract tahmin edilmez.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Type-C role/partner/supply ölçüm ayrımı üzerine gerçek PD capability/contract/profile ve partner ilişki görselleştirmesi; role swap/disconnect/negative current fixture; capacity ≠ contract ≠ consumption, read-only.
- **Kaynak giriş noktaları:** systemdiagnostics.cpp; sysinfo.*, diagsources.*, systemdiagnostics.*
- **Orijinal kabul testi:** USB-C PD, barrel AC, disconnected port, role swap, eksik `typec` sysfs, negatif current fixture; power role veya charge state değiştiren kontrol yok.

### C09 — Eksik yapılandırılmış tool output adaptörleri / P1 / 146–147
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** `structuredtools.h`, `systools.cpp`: Kernel lsmod ve Battery uevent için sınırlı, strict parser, raw fallback, tablo kopyalama, hassas alan whitelist ve eksik/truncated çıktı uyarısı.
- **Tur 6–14 gerçek ilerleme / sınır:** T6 lsusb ve lscpu -e typed adapter; T5 lsmod/Battery adapter ve raw fallback.
- **Kalan iş:** Diğer uygun tool IDs için bounded typed parser/helper JSON, locale/version/size/failure/raw fallback; aynı frozen snapshot üzerinde Raw/Structured toggle, model/proxy/sort/search/selection/export; Qt malformed fixtures.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Kernel lsmod ve Battery uevent adapter’ları korunur; uygun diğer tool ID’leri için typed bounded parsers/helper JSON, locale/version error/raw fallback; aynı frozen snapshot Raw/Structured toggle, model/proxy, sort/selection, selective export.
- **Kaynak giriş noktaları:** structuredtools.h; systools.cpp:Structured output; systools.*, diagreport.*, toolcatalog.*
- **Orijinal kabul testi:** örnek çıktıda malformed field, non-English output, boş ve büyük çıktı, unknown version; raw kayıpsız erişilebilir, parser hata verirse GUI donmaz.

### C10 — Ölçüme dayalı cooling/battery/sleep/storage geçmişi ve karşılaştırma / P1 / 163–169, 170–171
**Durum:** KISMİ — tam kabul yok. T6–14 hedeflenen esas yeni davranış belgelenmedi.
- **Korunacak Tur 5 parçası:** `sleeppage.cpp/.h`, `sleepadvice.cpp`: mevcut sleep kayıtlarından süre ve HW residency geçmişi, mode filtresi, eksik ölçümlerde boşluk, tekrar kayıtları ayıklama; eksik enerji veya HW ölçümünden sonuç uydurulmaması.
- **Tur 6–14 gerçek ilerleme / sınır:** T5 Sleep duration/HW residency history ve mode filter; T6–14 cooling/battery/SMART history eklenmesi belgelenmedi.
- **Kalan iş:** Sleep full boot/session/end AC ve missing resume; comparable cooling power/ambient/temp/fan/duration runs, battery charge/discharge/Wh/capacity/cycles, stable NVMe/SATA raw/normalized/worst/threshold/units history; bounded opt-in shared sampling, no auto stress/self-test/wake.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Sleep duration/residency history korunur; full boot/session/end AC ve missing resume, context matching. Cooling power/ambient/temp/fan/duration comparable runs; battery charge/discharge/capacity/Wh/cycles, SMART stable device raw/normalized/threshold/NVMe/SATA history; opt-in bounded sampling, shared polling, no auto stress/self-test/wake.
- **Kaynak giriş noktaları:** sleeppage.cpp; sleepadvice.cpp; coolingpage.cpp; batterypage.cpp; smartpage.cpp; coolingpage.*, batterypage.*, sleeppage.*, smartpage.*
- **Orijinal kabul testi:** karşılaştırılamayan cooling koşulu, battery şarj/deşarj geçişi, sleep duplicate/missing resume, SMART device replacement/unknown counters; yanlış eşleştirme, sahte history veya otomatik stress yok.

### C11 — Process-level network traffic charts / P1 / 179
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** `traffic.cpp/.h`, `netpage.cpp`: program bazlı 30 günlük TX/RX/toplam/bağlantı sayısı grafiği ve tablo, saturating qint64 toplam, eksik güne grafik boşluğu.
- **Tur 6–14 gerçek ilerleme / sınır:** T6 date range/top-N/other, exact saturating qint64, malformed quality; T5 daily graph.
- **Kalan iş:** Gerçek stable process identity/PID reuse ve missing day/overflow presentation, recorded context/instant rate sadece kaynak varsa; Qt date/ledger/table-chart totals fixtures.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Mevcut 30 günlük program TX/RX/total/conns graph üzerine custom range/top-N/other, ledger malformed quality, exact qint64 presentation/overflow, missing day; process identity/PID reuse ayrı, instant rate sadece gerçek kaynak varsa.
- **Kaynak giriş noktaları:** traffic.cpp; netpage.cpp:Daily Traffic Charts; traffic.*, netpage.*
- **Orijinal kabul testi:** 64-bit büyük byte, eksik gün, tarih filtresi, boş dosya, bozuk JSON, iki farklı program; chart toplamı mevcut tablodaki değerlerle aynı.

### C12 — Wi-Fi channel visualization / P2 / 183
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** `netpage.cpp/.h`: eldeki scan'den 2.4/5/6 GHz frequency marker, BSSID tablosu, received timestamp, adapter değişimi ve geç sonuç guard. Yeni otomatik tarama yok.
- **Tur 6–14 gerçek ilerleme / sınır:** T12 CLOCK_BOOTTIME age lower-bound/seen_ms, 60s freshness, stale/unknown marker, invalid RSSI ve generation guard; T5 BSSID/frequency marker.
- **Kalan iş:** Helper gerçek scan time/width/center verified metadata varsa channel overlap graph, unknown width marker, hidden SSID/multi-adapter fixture. IPC latency ayrı; yeni polling/auto scan yok.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Mevcut scan frequency marker/BSSID/adapter guard korunur; gerçek scan timestamp/freshness, width ve RSSI varsa verified band/range overlap graph, unknown width marker, hidden SSID ve multi-adapter stale fixture. Yeni polling yok.
- **Kaynak giriş noktaları:** netpage.cpp:rescanWifi; netpage.*, mevcut Wi-Fi helper
- **Orijinal kabul testi:** aynı SSID farklı BSSID, 6 GHz, unknown width, invalid signal, stale scan ve adapter switch; grafik eski adapter verisini yeniye bağlamaz.

### C13 — Firewall rule diff ve örnek eşleşme önizlemesi / P0/P1 / 181–182
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** `netrules.cpp/.h`, `netfirewall.cpp/.h`: helper’ın bildirdiği kayıtlı kuralları pin/diff; ID, sıra, policy ve alan değişiklikleri; eksik/yinelenen kimlik reddi. Açık add/edit dialogu sırasında bildirilen konfigürasyon değişirse istek gönderilmez.
- **Tur 6–14 gerçek ilerleme / sınır:** T8 saved-chain IPv4/IPv6/CIDR/port/ordered match preview ve optimistic snapshot guard helper lock altında, Rust fixture yazıldı.
- **Kalan iş:** Effective vs saved/draft canonical diff ve actual recorded context/unsupported chain/sets/stateful/NAT unknown, Qt/Rust behavior/no-write tests; old helper guard capability/compatibility fail-closed, gerçek access garantisi yok.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Saved-rule pin/diff ve GUI revision guard korunur; canonical CIDR/family/protocol/port/order/verdict diff, effective vs draft separation, recorded connection impact preview gerçek semantics ile; unsupported chain/sets/stateful Unknown. Backend revision revalidation ve no-write fixture; future access garantisi yok.
- **Kaynak giriş noktaları:** netrules.cpp; netfirewall.cpp; netfirewall.*, netrules.*, netpage.*
- **Orijinal kabul testi:** IPv4/v6, port aralığı, CIDR, first-match/order, invalid rule, unknown expression; preview sırasında sıfır firewall write; eski kurallar ve explicit apply davranışı korunur.

### D01 — Visual Scene Builder, tam dependency ve execution/step inspector / P0/P1 / 189–192, 195, 197–198
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** `scenes.cpp/.h`, `scenestab.cpp/.h`: gerçek transaction callback’lerinden running/returned/failed/soft failure/skipped/restore sonuçları; bounded son yürütme geçmişi. Mevcut yürütme sırası ve rollback korunur.
- **Tur 6–14 gerçek ilerleme / sınır:** T12 transaction UUID, real callbacks, deferred/skipped/superseded/readback mismatch/rollback distinction, normal exit guard; T5 preview/step log.
- **Kalan iş:** Aynı draft visual builder, structured ordered actual plan old-new/scope/Choice semantic diff, bütün dependency/profile/capability refs, engine trigger overlap/priority, simulated AC/game/time no command, daemon/deferred persistent events ve registry integration; real verified partial results.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Mevcut SceneEngine preview ve step log korunur; aynı draft’ı kullanan visual builder, gerçek ordered structured plan/old-new/scope/skip/dependency, semantic Choice diff, bütün profile/capability refs, trigger overlap/priority gerçek engine, read-only simulated event. Step result transaction ID + verified/partial/rollback distinction ve external daemon events; A05.
- **Kaynak giriş noktaları:** scenestab.cpp:Execution Log, eng_->preview; scenes.cpp; scenes.*, scenestab.*, A05
- **Orijinal kabul testi:** missing profile, conflicting trigger, Custom dependency, Reset/Unchanged, dry-run with command ve bir adımı başarısız scene fixture'ı; preview sıfır hardware/user command exec; kısmi sonuç başarı olarak gösterilmez.

### D02 — Lighting effect timeline/preview ve kayıtlı renk paleti / P2 / 200, 202, 204
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** `lightingtab.cpp`: 32 kayıt sınırı olan HEX boya renk paleti, kayıt/temizleme/renk seçimi, sıralı effect numarası.
- **Tur 6–14 gerçek ilerleme / sınır:** T9 validated HEX palette import ve per-key zoom/pan; T12 Spectrum dirty quit. T5 palette/effect numbers/draft preview/undo.
- **Kalan iş:** Ordered effect timeline/reorder same draft, approximate software animation hidden stop, supported speed/direction/color/capacity/encodedLen validation; draft/applied/readback/pending/failed/unknown status; zone4 parity X09, no HID write in preview.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Mevcut HEX palette, effect numarası, key canvas/draft preview/undo korunur; ordered sequence editor ve reorder aynı draft, yaklaşık software animation (hidden stop), only supported speed/direction/color modes, capacity/encodedLen validation, no host-side unsupported claims, palette import validation.
- **Kaynak giriş noktaları:** lightingtab.cpp:refreshPreview; lighting::Effect; kblayout.cpp; lighting.*, lightingtab.*, kblayout.*
- **Orijinal kabul testi:** Static/multi-color/speed/direction ve unsupported host effect, kapasite sınırı, undo/reorder, kapalı preview; animasyon sırasında HID/helper write yok, palette import invalid ise reddedilir.

### D03 — Dosya düzeyinde Restore Preview, dependency tree, configuration diff / P0/P1 / 210–211, 213–215
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** `backuppage.cpp`: mevcut config_inspect helper ile configuration restore manifest scope ağacı, bildirilen sayılar ve hedefler; user_state ve system_profile restore edilmez. Sistem image file-level preview unsupported açıkça gösterilir.
- **Tur 6–14 gerçek ilerleme / sınır:** T10 stat identity device/inode/size/mtime/ctime ns, inspect before/after guards, legacy verification distinction; T5 config manifest/sidecar chain/progress.
- **Kalan iş:** Helper bounded path-level manifest/hash/permission/create/replace/delete, traversal/symlink/decompression limits, chain graph stable IDs/cycle/missing/ambiguous/auth, crypto identity/time/per-file and target revalidation under helper/FD, history filters, true progress/registry, cancel/password vs corrupt. Image unsupported korunur.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Mevcut config_inspect scope manifest, config_diff, sidecar chain ve restore guard korunur; helper-supported bounded file manifest/hash/permission/target create/replace/delete semantics (image unsupported kalabilir), path traversal/symlink/decompression bounds. Chain graph stable IDs/cycle/missing/ambiguous/authentication; verification identity/timestamp; preview sonrası archive/target revalidate ve no restore/mount/write tests.
- **Kaynak giriş noktaları:** backuppage.cpp:previewRestore; bundle.*; configpage.cpp; backuppage.*, bundle.*, configpage.*, mevcut backup helper
- **Orijinal kabul testi:** full/incremental chain, missing parent/cycle, malicious archive path, eski verification, changed target ve unsupported image fixture'ları; preview hiçbir restore/mount/write başlatmaz; yanlış seçili arşivde işlem engellenir.

### D04 — Yalnızca kalan layout/çeviri/mantık regresyonları / P0/P1 / 30, 28, FIX-01–21
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** Önceki 78 TR katalog girdisi + bu tur 18 yeni Simulation çevirisi ve structured header i18n marker’ları; önceki çeviriler korunur.
- **Tur 6–14 gerçek ilerleme / sınır:** T6–14 yeni TR katalog girdileri, layout ve source regression değişiklikleri; Tur10 iterator hotfix.
- **Kalan iş:** Bütün FIX-01–21 için gerçek native davranış sonucu, visible string context/placeholder/plural/title-case audit, iki shell TR/EN/DPI/font/keyboard, selection/favori/scroll/style regressions. Warning suppression/teknik anahtar değiştirme yok.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** FIX-01–21 her biri için native davranış sonucu; unresolved blocking/layout/cached style ve selection/scroll regression; tüm visible strings i18n context/placeholder/plural/title case, iki shell/dil/DPI ve keyboard accessibility. Önceki düzeltmeleri yeniden yazma, warning suppression yok.
- **Kaynak giriş noktaları:** gui/src; gui/tests; i18n.cpp; bütün GUI; A01/A08
- **Orijinal kabul testi:** TR/EN ve iki UI modu ekran testlerinde kırpılan label/yanlış active tab yok; mevcut favoriler ve kullanıcı seçimi korunur; tamamlanmamış bir özellik “hazır” olarak gösterilmez.

### D05 — Native build/test, güvenlik denetimi ve experimental branch / P0 / Teslimat
**Durum:** ENGELLİ — native/release kapısı açık.
- **Korunacak Tur 5 parçası:** Debug+Release CMake yapılandırması yeniden denendi; Qt6Config.cmake yok. Paket sunucusu DNS çözülemiyor; apt paketleri bulamadı, Cargo yok.
- **Tur 6–14 gerçek ilerleme / sınır:** T14 42/42 runner PASS (statik+bağımsız native), ZIP CRC PASS; önceki Qt/Rust bağımlılık engeli ve hotfix mevcut.
- **Kalan iş:** Önce toolchain kur/gerçek Qt Debug+Release (Gentoo PGO dahil uygun test), CTest ve GUI sanitizer; Rust locked build/test/clippy/deny, tüm runner, actual CI, güvenlik/visual/hardware fixture, clean ZIP; experimental HEAD/permission ve ancak kabul sonrası normal push; main/old untouched.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Gerçek Qt6 Debug+Release, CTest, ASan/UBSan, Rust locked build/test/Clippy/cargo-deny, mevcut Python runners ve CI; güvenlik/readback/restore regressions, test artifacts. Source ZIP temizliği, GitHub HEAD/branch doğrulama ve ancak kabul testleri geçtiğinde experimental’a normal commit/push; main/old untouched.
- **Kaynak giriş noktaları:** gui/CMakeLists.txt; Cargo.lock; .github/workflows/ci.yml; Cargo workspace, gui/CMakeLists.txt, .github/workflows/ci.yml
- **Orijinal kabul testi:** Debug/Release, CTest, Rust locked/Clippy/cargo-deny, CI ve güvenli fixture; doğrulanmadan push yok.

### X01 — Navigasyon: resize rail, manuel global favoriler, açık startup page selector.
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** Orijinal X işinin dayandığı mevcut GUI davranışını koru; aşağıdaki Tur 6–14 kaydını kullan.
- **Tur 6–14 gerçek ilerleme / sınır:** T6 rail splitter/manual favorite/explicit startup per shell ve fallback.
- **Kalan iş:** Global alt araç stable IDs ve orphan clean-up, unsupported fallback, collapse/restore/long TR/DPI/restart/empty favorites native test; mevcut sayfa/Health favorileri korunur.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Navigasyon: resize rail, manuel global favoriler, açık startup page selector.
- **Kaynak giriş noktaları:** mainwindow.cpp, hometab.cpp, toolcatalog.cpp
- **Orijinal kabul testi:** Restart, unsupported page fallback, empty favorites, TR label ve collapse restore.

### X02 — Ortak sürümlü workspace state store (per shell/view) ve dirty interface A05 ile; selection/filter/sort/splitter/column/scroll persistence.
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** Orijinal X işinin dayandığı mevcut GUI davranışını koru; aşağıdaki Tur 6–14 kaydını kullan.
- **Tur 6–14 gerçek ilerleme / sınır:** T12 Scenes/Spectrum quit dirty; T13 workspace/v1 32KiB per shell, boot filter/split/full UUID, network header order/width ve bazı sort.
- **Kalan iş:** Tüm lazy view selection/filter/sort/splitter/column visibility/scroll, migration/reset/vanished selection, common dirty marker/loss summary (all editors/restart/quit) A05 ile; corrupt/restart native fixtures.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Ortak sürümlü workspace state store (per shell/view) ve dirty interface A05 ile; selection/filter/sort/splitter/column/scroll persistence.
- **Kaynak giriş noktaları:** mainwindow.cpp, netpage.cpp, cosearchdialog.cpp, bootpage.cpp
- **Orijinal kabul testi:** Bozuk/eski settings, hidden/vanished selection, dirty cancel ve restart.

### X03 — Kullanıcı tipografi tercihleri, mono numeric font, preview ve reset; A01 density ile ortak.
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** Orijinal X işinin dayandığı mevcut GUI davranışını koru; aşağıdaki Tur 6–14 kaydını kullan.
- **Tur 6–14 gerçek ilerleme / sınır:** T8 family/8–14pt/numeric mono/preview/reset/fallback, QSS scale.
- **Kalan iş:** Tüm numeric tables/controls, missing font fallback ve larger/smaller font DPI/layout native test, density A01. Mevcut tercih ve restart guard korunur.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Kullanıcı tipografi tercihleri, mono numeric font, preview ve reset; A01 density ile ortak.
- **Kaynak giriş noktaları:** theme.cpp, hometab.cpp, main.cpp
- **Orijinal kabul testi:** Eksik font fallback, büyük/küçük font ve DPI.

### X04 — Firmware refresh/provenance, write öncesi tekil/toplu old-new/transport/units preview, reason enum ve partial result; fan factory overlay yalnız gerçek default.
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** Orijinal X işinin dayandığı mevcut GUI davranışını koru; aşağıdaki Tur 6–14 kaydını kullan.
- **Tur 6–14 gerçek ilerleme / sınır:** T6 old-new confirmation/sysfs readback/WMI guard; T10 firmware source/time/platform/S08.
- **Kalan iş:** Ortak disabled reason enum (permission/device/busy/unsupported/profile), true transport/units/partial WMI results/quality, refresh fixture, factory fan curve yalnız verified stock/point/timestamp; no-write cancel/floor guards.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Firmware refresh/provenance, write öncesi tekil/toplu old-new/transport/units preview, reason enum ve partial result; fan factory overlay yalnız gerçek default.
- **Kaynak giriş noktaları:** fwattrtab.cpp, platformprofile.cpp, fancurvedialog.cpp
- **Orijinal kabul testi:** Cancel sıfır write, WMI failure, unsupported disabled, historical ≠ stock, safety floor.

### X05 — Boot service inspector (OpenRC/systemd real enabled/active/result/last run/boot ID), unit-aware canonical integer editors overflow/rounding safe.
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** Orijinal X işinin dayandığı mevcut GUI davranışını koru; aşağıdaki Tur 6–14 kaydını kullan.
- **Tur 6–14 gerçek ilerleme / sınır:** T6 exact integer byte unit parser; T8 OpenRC markers/systemd async inspector boot ID/guard.
- **Kalan iş:** ns/us/ms ve uygun canonical unit editor kapsamı, full int64 UI roundtrip, real service apply receipt/last run/result and enabled vs applied, OpenRC/systemd/permission fixtures; no service exec.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Boot service inspector (OpenRC/systemd real enabled/active/result/last run/boot ID), unit-aware canonical integer editors overflow/rounding safe.
- **Kaynak giriş noktaları:** bootadvisor.cpp, optimizetab.cpp, int64spinbox.cpp
- **Orijinal kabul testi:** Enabled ≠ applied; no service exec, INT64 unit roundtrip.

### X06 — Report adaptive section state, capability indicators real executable/kernel/device/permission, incremental model/view stable keys.
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** Orijinal X işinin dayandığı mevcut GUI davranışını koru; aşağıdaki Tur 6–14 kaydını kullan.
- **Tur 6–14 gerçek ilerleme / sınır:** T13 report same code+unique table+exact row restore selection/sort/scroll/width; rebuild devam eder.
- **Kalan iş:** Stable section collapse/adaptive detail, executable/kernel/device/permission capability indicators; incremental typed model stable row keys across reports/network/boot, changed-only 10k fixture/vanished selection; rebuild+restore incremental değil.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Report adaptive section state, capability indicators real executable/kernel/device/permission, incremental model/view stable keys.
- **Kaynak giriş noktaları:** diagreportview.cpp, systools.cpp, toolcatalog.cpp, netpage.cpp
- **Orijinal kabul testi:** 10k row changed-only update, refresh selection, missing binary/permission distinguish.

### X07 — Kernel typed combined filters (context independent), seçilebilir iki boot semantic comparison, full boot IDs.
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** Orijinal X işinin dayandığı mevcut GUI davranışını koru; aşağıdaki Tur 6–14 kaydını kullan.
- **Tur 6–14 gerçek ilerleme / sınır:** T6 selectable boot; T9 combined kernel filters, independent context, full UUID; T13 boot persistence.
- **Kalan iş:** Kalan semantic boot sources kernel/driver/guard/preset/pstore and unknown counts, typed driver/device semantics, native combined filter/context/full ID/prefix/restart fixture.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Kernel typed combined filters (context independent), seçilebilir iki boot semantic comparison, full boot IDs.
- **Kaynak giriş noktaları:** klogpage.cpp, bootpage.cpp
- **Orijinal kabul testi:** Existing comparedWithPrevious reuse; missing events ≠ 0, prefix ambiguity, context target retained.

### X08 — Network workspace deep links connection→rule→log, stable process grouping and listening bind scope/exposure evidence.
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** Orijinal X işinin dayandığı mevcut GUI davranışını koru; aşağıdaki Tur 6–14 kaydını kullan.
- **Tur 6–14 gerçek ilerleme / sınır:** T8 connection→saved-rule preview; T6 C11 top-N/date (stable process grouping değil).
- **Kalan iş:** Ortak network workspace capability/state banner, connection→rule→log exact deep links, PID+start/exe groups/socket children, listening bind scope IPv4/v6/UID/exposure evidence, no-write navigation/unknown PID/reuse fixture.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Network workspace deep links connection→rule→log, stable process grouping and listening bind scope/exposure evidence.
- **Kaynak giriş noktaları:** netpage.cpp, traffic.cpp, netrules.cpp
- **Orijinal kabul testi:** PID reuse, loopback vs wildcard, unknown PID, no firewall write on navigation.

### X09 — Adaptive keyboard canvas zoom/pan/aspect and hit-test, zone4 parity; D02 existing preview/undo preserved.
**Durum:** KISMİ — tam kabul yok.
- **Korunacak Tur 5 parçası:** Orijinal X işinin dayandığı mevcut GUI davranışını koru; aşağıdaki Tur 6–14 kaydını kullan.
- **Tur 6–14 gerçek ilerleme / sınır:** T9 per-key 1–4x zoom/pan/fit/hit-test math ve 4-zone reflow, bağımsız native transform PASS.
- **Kalan iş:** Zone4 tam canvas parity, content/aspect adaptive, real hit-test/key selection/resize/DPI/clipping native tests, no HID/helper write. D02 draft/undo korunur.
- **Tur 5 orijinal açık kapsam (referans; mevcut alt işlevi tekrar yazma):** Adaptive keyboard canvas zoom/pan/aspect and hit-test, zone4 parity; D02 existing preview/undo preserved.
- **Kaynak giriş noktaları:** lightingtab.cpp, kblayout.cpp, zone4tab.cpp
- **Orijinal kabul testi:** DPI/resize selection same key, no clipping or hardware write.

## 4. 118 GUI ID — tek tek kalan alt iş ve kabul matrisi
ID özelinde tarihsel ilerleme kanıtı yoksa orijinal açık iş aynen bırakılmıştır. Paket düzeyinde ilişkili değişiklik olabilir. Bütün satırlarda native kabul açık. Daha ayrıntılı tarihsel metin ve kaynak sütunları TSV dosyasında da bulunur.

| GUI ID / özellik | Paket | Tur 6–14 kayıt / durum | Kalan alt iş | Kaynak | Kabul testi |
|---|---|---|---|---|---|
| GUI-01 — Global Command Palette | A09 | T14: KISMİ — Firmware mevcut satır metadata cihaz/parametre ID; Ryzen CCD/SMU slot, Intel domain/limit, NVIDIA V/F, Home fan ve AMD 19 statik kontrol metadata; yinelenen ID reddi, deferred-delete ID retire; navigation seçildikten sonra resolve, nested tab focus; yeni hardware probe yok. Lazy tüm araç ve gerçek Qt fixture açık. | Bütün henüz açılmamış dinamik ayarlar ve lazy alt araç metadata, global unique ID/risk/capability ve no-constructor/helper native focus testi. | mainwindow.cpp, uinavigator.cpp, toolcatalog.cpp | Açılmamış firmware/optimization ayarı bulunmalı; arama sırasında widget/helper oluşmamalı. |
| GUI-02 — Intelligent Search | A09 | T14: KISMİ — TR/EN alias, exact stable ID > exact title > label prefix > token exact > token prefix > substring > edit distance; 256 input/100 result sınırı, mevcut fuzzy korunur. Native Qt rank ve tüm sözlük kapsamı açık. | Eksiksiz TR/EN alias/synonym sözlüğü ve native typo/exact ordering/duplicate/bounds testleri. | uinavigator.cpp | Türkçe/İngilizce alias ve tek karakter hatası doğru sonuç vermeli; exact match önce gelmeli. |
| GUI-03 — Adaptive Workspace | A01/A08/D04 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Yoğun sayfaların responsive min-size/form/splitter düzeni ve iki shell; A01/A08. | mainwindow.cpp, hometab.cpp, experimentalui.h | Dar pencere ve %200 DPI'da ana eylemler erişilebilir; gereksiz yatay kaydırma yok. |
| GUI-04 — Resizable Navigation | X01 | T6: İLİŞKİLİ KISMİ — Rail QSplitter ve ayarlı genişlik/collapse; Qt restart/DPI açık. | Rail min/max/collapse ve uzun TR/DPI/restart gerçek Qt testi; mevcut splitter yeniden yazılmaz. | mainwindow.cpp | Sürükle, kapat/aç ve restart sonrası genişlik korunmalı; uzun Türkçe başlık kırpılmamalı. |
| GUI-05 — Navigation Favorites | X01 | T6: İLİŞKİLİ KISMİ — Manuel stable-ID sayfa ve Health favorileri; diğer alt araçlar ve native test açık. | Tüm alt araç global favorileri, orphan ID temizliği ve empty/default/restart native test. | toolcatalog.cpp, healthtab.cpp, mainwindow.cpp | Yeni config boş; yalnızca kullanıcı seçtiği araç eklenmeli ve doğru araca açılmalı. |
| GUI-06 — Workspace Memory | X02 | T13: Per-shell workspace/v1 store; boot filter/full UUID/splitter ve ağ header tercihleri. Diğer lazy sayfalar/scroll/filter ve restart testi açık. | Diğer lazy sayfalar scroll/filter/splitter/selection, migration/reset/hidden/vanished selection ve restart fixture. | mainwindow.cpp, cosearchdialog.cpp, netpage.cpp | Restart ve lazy sayfa oluşturma sonrası doğru durum dönmeli; eski/bozuk kayıt güvenle atlanmalı. |
| GUI-11 — Global Operation Center | A05 | T12: KISMİ — Scene transaction UUID ve gerçek callback olayları, deferred GPU timer/in-flight normal çıkış koruması, kısmi/rollback/unverified sonuç; ortak atomik kaynak rezervasyonu ve diğer owner/cancel/restore entegrasyonları açık. | Gerçek merkezi atomik registry/lock, all owner/resource/cancel/restore ve error/log; Scene dışı işlemler. | mainwindow.cpp, cpustressbox.cpp, coolingpage.cpp | Başarısız/cancel/restoring ayrı; terminal duruma gelmeden kaynak serbest kalmamalı. |
| GUI-13 — Unsaved Changes Indicator | A05/X02 | T12: KISMİ — Scenes ve Spectrum Lighting unsaved quit uyarısı, Cancel varsayılan, onaydan sonra active operation tekrar kontrolü; tüm editörler ve workspace store/selection/splitter/sort/persistence açık. | Tüm editörlerde ortak dirty page marker, apply/save/revert ve restart loss summary; mevcut quit uyarısı korunur. | scenestab.cpp, optimizetab.cpp, lightingtab.cpp | Taslak değişince yalnız ilgili sayfa dirty; revert/save/apply doğru geçiş; cancel taslağı korur. |
| GUI-14 — Custom Startup Workspace | X01 | T6: İLİŞKİLİ KISMİ — Home/last/explicit per-shell tercihi, fallback; Qt restart testi açık. | Unsupported/removed page fallback ve iki shell restart fixture. | mainwindow.cpp, hometab.cpp | Seçilen sayfaya restart sonrası gitmeli; silinen/unsupported sayfa Home'a düşmeli. |
| GUI-21 — OLED Appearance Mode | A01/A02 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Paletten bağımsız OLED yüzey/parlaklık tercihleri ve kontrast; burn-in garantisi yok. | theme.cpp, hometab.cpp | OLED görünümünde metin/selection contrast korunmalı; diğer 56 tema değişmemeli. |
| GUI-22 — Typography Controls | X03 | T8: Kısmi — font family, 8–14 pt, mono spinbox, preview/reset ve fallback; native layout/fallback açık. | Tüm numeric tables/controls ve eksik font/large-small DPI native fixture. | theme.cpp, hometab.cpp, main.cpp | Büyük/küçük fontlarda erişilebilir kontrol ve tablo hücresi; font bulunmazsa fallback. |
| GUI-23 — Compact / Standard Density | A01 | T8: İlişkili kısmi — shared QSS point scaling; tüm density ve fixed geometry açık. | Tüm fixed geometry ve density token coverage; QSS point scale yeterli değil. | theme.cpp, experimentalui.h | Compact/Standard tüm sayfalarda tutarlı; ikon/metin kesilmemeli. |
| GUI-25 — Contrast Validation | A01/A08 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Rendered state matrix ve düşük kontrast düzeltmeleri/istisna gerekçeleri; custom ve grafik testleri. | theme.cpp, gui/tests | Bütün palet × durum çiftleri test edilmeli; düşük contrast açıkça raporlanmalı. |
| GUI-26 — Semantic Color System | A01 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Status enum → palette token + ikon + erişilebilir metin; yerel hardcoded renkleri taşı. | theme.cpp, experimentalui.h | Aynı durum Home, Health ve tuning'de aynı anlama gelsin; Unknown başarı rengi olmasın. |
| GUI-28 — High-DPI Layout Audit | A08/D04 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Tüm GUI'de min-size, scroll, table header, tooltip, popup ve chart label denetimi; DPR varyantları. | mainwindow.cpp, charts.cpp, ryzentab.cpp, fwattrtab.cpp | %100/125/150/175/200, iki dil ve iki shell; metin ve ana buton kırpılmamalı. |
| GUI-29 — Colorblind-Friendly Mode | A01 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Renkten bağımsız ikon/desen/metin; seçilebilir güvenli palet ve grafikte çizgi stili. | cosearchdialog.cpp, theme.cpp | Grayscale simülasyonda pass/fail/unknown ayrılmalı. |
| GUI-30 — Translation & Terminology Audit | D04 | T6: İLİŞKİLİ KISMİ — 50 yeni TR girdi; tüm GUI dil denetimi açık.; T7: KISMİ — 113 yeni katalog girdisi; placeholder/katalog testi PASS. Tam çeviri ve native UI denetimi açık.; T8: İlişkili kısmi — 58 yeni TR katalog mesajı; katalog testi PASS, runtime çeviri açık.; T9: 42 TR katalog girdisi, placeholder integrity PASS; tam terminology ve DPI audit açık.; T10: İLİŞKİLİ KISMİ — 25 yeni TR çevirisi; T12: İLİŞKİLİ KISMİ — 32 yeni TR katalog girdisi, katalog testi PASS; native layout/çeviri matrisi açık.; T14: İLİŞKİLİ KISMİ — navigation metadata TR katalog girdileri ve uyarılar; tüm GUI çeviri denetimi açık.; T11: Türkçe katalog ekleri (Tur 11 raporu); T13: grid çeviri girdisi (Tur 13 raporu). | Tüm visible string/context/plural/placeholder/title-case denetimi, teknik log/anahtarlar ve TR/EN native QA. | i18n.cpp, gui/src, gui/tests | Türkçe/İngilizce placeholder ve çeviri kapsamı testleri; kullanıcı arayüzünde istenmeyen İngilizce kalmamalı. |
| GUI-31 — Configurable Dashboard | A10 | T13: Classic ayrı panel order/visibility/reset, Experimental eski tercihler korundu. Qt restart/DPI testi açık. | Geri erişim/tüm güvenli panel seçenekleri, polling ve iki shell restart/DPI fixtures. | hometab.cpp, experimentalui.h | Sıra ve panel tercihi restart sonrası korunmalı, min-size ihlali olmamalı. |
| GUI-39 — Compact Device Controls | A01/A10 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Ortak capability-aware label/value/action satırı; fan/MUX/panel/battery/core parking. | hometab.cpp, experimentalui.h | Unsupported kontrol yer tutmamalı, uzun etiket ve birim taşmamalı. |
| GUI-40 — Adaptive Dashboard Grid | A10 | T13: Classic ortak adaptif grid ve 24px hysteresis; native matematik PASS, gerçek layout/resize testi açık. | Tüm visibility kombinasyonları ve gerçek Qt reflow/resize/clipping/hysteresis fixture. | hometab.cpp | Panel gizlenince boş grid hücresi bırakmamalı; resize sırasında flicker olmamalı. |
| GUI-43 — CCD Topology View | B01 | T7: KISMİ — Arşiv CPU/topoloji fingerprint ve CCD/slot/logical CPU eşleme; canlı SMT/offline/X3D/heatmap açık. | Live SMT/offline/V-Cache kanıtı, topology doğrulaması ve heatmap bağlantısı. | cosearchdialog.cpp, sysinfo.cpp | CPU ID boşlukları, SMT kapalı, offline core ve bilinmeyen CCD fixture'ları. |
| GUI-44 — Core Stability Heatmap | B01 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Journal kanıtı ile live heatmap ve contradictory state; exclusion ≠ pass. | cosearchdialog.cpp, coevidence.cpp | Test edilmemiş core yeşil olmasın; reboot exclusion geçer sonucu sayılmasın. |
| GUI-45 — Per-Core Candidate History | B01 | T7: KISMİ — Arşiv session filtre/export/delete ve iki oturum karşılaştırma; candidate/event ID ve tam trial filtre açık. | Stable candidate/event/boot IDs, per-core/trial/candidate filters ve eski/corrupt/reboot fixtures; archive export/delete/diff korunur. | cosearchdialog.cpp, coevidence.cpp, coinsights.cpp | Eski oturumlar yeni arama başlatınca silinmemeli; eksik journal kayıtları Unknown. |
| GUI-46 — Reboot Recovery Inspector | B01/B02 | T7: KISMİ — Typed decision, recorded search value ve exclusion boundary; journal/guard revizyon kontrolü. Tam guard/evidence ve Qt replay testi açık. | Tam guard/exclusion/replay ve last trusted value açıklaması, actual resume Qt fixture. | cosearchdialog.cpp, costrategy.cpp | Preview gerçek state'i mutate etmemeli; tekrar reboot senaryosunda dışlanan aday yeniden önerilmemeli. |
| GUI-51 — CO Profile Diff | B01 | T7: KISMİ — Arşivde CCD/slot semantik diff ve CPU fingerprint; ryzentab profil editörü diff entegrasyonu yapılmadı. | ryzentab profil diff entegrasyonu, all-core/per-core ve stable core identity/verification; archive diff tekrar yazılmaz. | ryzentab.cpp, profilediff.h | Dizi sırası değişince yanlış core diff oluşmamalı; eksik değer sıfır olmamalı. |
| GUI-59 — Resume Safety Preview | B02 | T7: KISMİ — Preview gerçek Search kopyası ve journal/guard revizyon kontrolü; mevcut CO native regressions PASS; yeni Qt fixture açık. | Preview-dispatch replay ve crash/guard revision native Qt fixtures; mevcut Search-copy preview korunur. | cosearchdialog.cpp, costrategy.cpp | Preview/resume aynı state'te aynı adayı seçmeli; journal ve guard yazımı yok. |
| GUI-61 — Multi-Curve Overlay | B03 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Kayıtlı profil katmanı ekle; voltage/point ID eşlemesi, bağımsız legend ve karşılaştırılabilirlik kontrolü. | nvidiatab.cpp, vfcurvewidget.cpp | Farklı GPU/point topology eşleşmediğinde çizme veya uyar; reconstructed base factory stock denmemeli. |
| GUI-62 — Curve Delta View | B03 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Aynı voltage ID'leri için base/readback/profile farkını ayrı alt grafik yap; birim MHz. | vfcurvewidget.cpp, nvidiatab.cpp | Eşleşmeyen nokta boş; negatif ve pozitif fark doğru; sıfır yalnız gerçek eşitlikte. |
| GUI-63 — Curve Point Inspector | B03 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Grafik seçimiyle bağlı nokta inspector; çoklu seçim özeti ve validation status. | vfcurvewidget.cpp, nvidiatab.cpp | Zoom/selection değişince inspector aynı point ID'yi izlemeli; cached data canlı gösterilmemeli. |
| GUI-66 — Curve Editing History | B03 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Kalıcı operation journal: edit action, timestamp, before/after, transaction sonucu; undo geçmişinden bağımsız. | nvidiatab.cpp, vfcurvewidget.cpp | Undo GPU'ya yazmamalı; journal başarısız işlemleri de göstermeli; veri sınırı olmalı. |
| GUI-68 — Hardware Readback Comparison | B03 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Gerçek UUID/fingerprint, full transaction scope, backend quantization, adjusted/partial/missing status ve kalıcı history. | nvidiatab.cpp, mevcut nvcurve helper | Eski readback yeni transaction'ı doğrulamasın; farklı GPU kimliği kabul edilmesin. |
| GUI-71 — Curve Profile Metadata | B03 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | GPU UUID/model, VBIOS/driver, curve topology, created/verified tarihleri; eski profil için unknown. | nvidiatab.cpp, profilediff.h | Donanım/driver uyuşmazlığı belirgin; metadata eksikliği güvenilirlik garantisi sayılmasın. |
| GUI-74 — Trial Session Monitor | A05/B03 | T12: İLİŞKİLİ KISMİ — Scene deferred GPU işi normal çıkış/restart guard kapsamına alındı; gerçek trial readback ve registry açık. | Gerçek NVIDIA trial deadline/restore/readback events ve merkezi registry; deferred Scene guard trial monitor yerine geçmez. | mainwindow.cpp, trialbar.cpp, nvidiatab.cpp | Trial sona erince restore callback bitene kadar busy; force close garantisi verme. |
| GUI-76 — Xid Event Integration | C01/B03 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Full boot/GPU UUID/request ID ilişkisi ve editörden deep-link; korelasyon nedensellik değil. | evidencereports.cpp, nvidiatab.cpp, flightrec.cpp | Yanlış boot/cihaz eşleşmez; yakın zaman ilişkisi nedensellik olarak sunulmaz. |
| GUI-79 — Firmware Parameter Inspector | A03/X04 | T6: İLİŞKİLİ KISMİ — WMI per-row known ve sysfs readback/guard; tam timestamp/freshness açık.; T10: KISMİ — firmware snapshot kaynak/okuma zamanı, platform profil snapshot ve açık istekle S08 kaynak raporu | Ortak A03 freshness/quality/refresh ve WMI unavailable/late fixture, bağımsız verified snapshot. | fwattrtab.cpp | WMI readback yoksa eski değer live/verified değil. |
| GUI-80 — Firmware Change Summary | A05/X04 | T6: İLİŞKİLİ KISMİ — Tekil/toplu old/new onay, direct sysfs readback; WMI fresh ve fixture açık. | Transport/units/device ve per-row WMI fresh/partial readback result; cancel no-write fixture. | fwattrtab.cpp | Cancel hiçbir write yapmamalı; partial WMI/sysfs sonucu ayrı raporlanmalı. |
| GUI-81 — Locked Control Explanation | X04 | T6: İLİŞKİLİ KISMİ — WMI bilinen değer ve busy guard; ortak reason enum açık. | Tüm disabled reasons için ortak enum/label ve profile/permission/busy/unsupported native fixture. | fwattrtab.cpp, platformprofile.cpp | Custom değişiminde lock güncellenmeli; read-only parametre sebepsiz etkinleşmemeli. |
| GUI-91 — Fan Curve Overlay | A06/X04 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Doğrulanabilen factory/default katmanı, timestamp, ayrı legend ve per-point karşılaştırma; yoksa unknown. | fancurvedialog.cpp | EC snapshot stock diye etiketlenmemeli; safety floor ve write yolu değişmemeli. |
| GUI-94 — Optimization State Matrix | B07 | T7 startup draft readback guard B08; B07 state matrix tam readback/restore-original açık. | Restore-original yalnız backend varsa; true verification timestamp ve stale mismatch. | optimizetab.cpp, tuningviewstate.h | Snapshot factory default sayılmaz; unreadable/stale boot match olmaz. |
| GUI-95 — Setting Source Indicator | B07 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Per-device/source provenance, real drift attribution ve unknown external writer, fixture. | optimizetab.cpp, driftwatch.cpp | TLP benzeri servis açık diye değer kaynağı otomatik atanmaz. |
| GUI-96 — Smart Selection Filters | B07 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Birleşik conflict/covered/verified filtre, per-section persist ve hidden selected count. | optimizetab.cpp | Filtre include checkbox/draft değiştirmemeli; hidden selected sayısı açık olmalı. |
| GUI-99 — Setting Detail Inspector | B07 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Tek setting detail pane: key/path/unit/range/effect/risk/dependency ve gerçek current/original values. | optimizetab.cpp | Tooltip'teki teknik bilgi eksiksiz taşınmalı; unknown range uydurulmamalı. |
| GUI-100 — Optimization Profile Diff | B07 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Semantik live/boot diff ve coverage/missing/stable device normalization. | optimizetab.cpp, profilediff.h | Ayar key sırası fark yaratmamalı; unsupported ve no-op ayrı. |
| GUI-101 — Boot Service Inspector | X05 | T8: Kısmi — OpenRC markers, systemd async read-only properties, full boot ID/guard; gerçek apply receipt ve init fixture açık. | Gerçek last apply receipt ve init service result ayrımı, OpenRC/systemd fixture; enabled ≠ applied. | optimizetab.cpp, bootadvisor.cpp | Enabled ≠ başarılı apply; service absent/permission denied ayrı; dış servis çalıştırma yok. |
| GUI-102 — Persistent Edit Drafts | B08 | T7: KISMİ — Opt-in ilk fresh full readback üzerinden VM/scheduler v1 unchecked restore; diğer scope/per-device/migration ve gerçek no-write restart fixture açık. | Diğer scope/per-device stable identity, schema migration/invalid reporting/discard/live-boot conflict ve no-write restart fixture. | optimizetab.cpp | Restart hiçbir ayarı otomatik uygulamamalı; incompatible draft açıkça işaretlenmeli. |
| GUI-103 — Conflict Relationship View | B07 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Relationship graph/detail, dependency cycles ve operation scope/unknown reason. | optimizetab.cpp, driftwatch.cpp | Conflicting rows gizlenmemeli; graph cycle güvenli; existing guard korunmalı. |
| GUI-104 — Per-Device Tuning Inspector | B07 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Stable device identity/hotplug/inspector/filter; stale target write guard. | optimizetab.cpp | Yok olan aygıt üzerinde write seçimi kalmamalı; cihaz ID değişimini tespit et. |
| GUI-105 — Apply Scope Selector | B07 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Scope/preset merge/remove etkilerini onay öncesi gerçek helper semantiği ile göster. | optimizetab.cpp | Live-only boot'u değiştirmez; boot-only live write yapmaz (mevcut helper semantiğine göre doğrula). |
| GUI-106 — Apply Result Report | B07 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Per-key structured helper result + transaction ID, actual/verify/partial/skipped/restore states. | optimizetab.cpp, tuning helper | Timeout/partial/rejected success gibi görünmesin; failed restore ayrımı. |
| GUI-107 — Preset Coverage View | B07 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Preset coverage included/not specified/unsupported/shadowed ve merge/replace/hidden selection. | optimizetab.cpp | Eksik key default olarak uygulanmaz; hidden checked kapsamı gösterilir. |
| GUI-109 — Unit-Aware Editors | X05 | T6: İLİŞKİLİ KISMİ — Exact integer byte parser PASS; ns/us/ms ve Qt davranışı açık. | ns/us/ms ve tüm ilgili int64 editörler, overflow/rounding ve gerçek Qt roundtrip. | int64spinbox.cpp, optimizetab.cpp | INT64 sınırı ve unit roundtrip; dönüşüm yüzünden ayar sessiz değişmemeli. |
| GUI-111 — Table Column Customization | B07 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | İsteğe bağlı sütun görünürlüğü/sıra/genişlik, stable ID ve reset; kritik status/selection erişilebilir kalmalı. | optimizetab.cpp | Restart layout korunmalı; filtre/selection verisini değiştirmemeli. |
| GUI-112 — Autotune Workspace | B11 | T6 structured progress run ID B10; B11 full workspace/history entegrasyonu açık. | Run ID ile settings/execution/report/history workspace bağları; Explore ayrımı. | optimizetab.cpp, autotunereport.cpp | Explore ve Apply ayrımı korunmalı; running işlem sırasında yeni sonuç yanlış oturuma bağlanmamalı. |
| GUI-113 — Weight Studio | B09 | T11: Radar ve sayısal varsayılan/düzenlenen/etkin ağırlık; solver sınırları ve eksik helper verisi guard. Qt test açık; güncel solver eşitliği ve preset overlay kabulü açık. | Preset overlay, actual current solver weights ve UI validation, native behavior; mevcut radar korunur. | optimizetab.cpp, autotunereport.cpp | Radar değerleri solver'ın gerçek weights'ine eşit; default/restore korunmalı. |
| GUI-114 — Candidate Ranking Explorer | B11 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Aday listesini sıralama/filter, seçilen ile alternatifler, constraint nedeniyle elenenler; yalnız model verisi varsa. | optimizetab.cpp, autotunereport.cpp | Skor karşılaştırmaları aynı goal/context içinde; eksik skor uydurulmaz. |
| GUI-115 — Autotune Explainability | B11 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Yapılandırılmış decision breakdown: objective contributions, evidence, constraints, fallback; backend sağlamıyorsa açıklanmıyor. | optimizetab.cpp, autotunereport.cpp | Gösterilen açıklama aynı run/setting/adaydan; skor yeniden tahmin edilmesin. |
| GUI-117 — Calibration Data Manager | B09 | T11: v10 imza harici salt okunur açma (8 MiB/20.000 satır); yeni benzersiz kullanıcı dosyasına atomik frozen export, SHA-256. Model import/rollback, passive session mutation ve davranış testleri açık. | Tam schema/validated controlled import, atomic backup/rollback, supported passive session management ve active calibration guard. GUI signature dosyasını doğrudan yazmaz. | optimizetab.cpp, signature v10 inspector | GUI doğrudan signature değiştirmesin; bozuk/gelecek schema read-only hata; model corpus korunmalı. |
| GUI-119 — Confidence Visualization | B11 | T11: Gerçek predcheck SD ve z değeri gösterimi, eksik/bozuk ölçüm ayrımı. Recommendation-level interval ve evidence sufficiency açık. | Recommendation-level real interval/confidence ve evidence sufficiency, model vs measurement distinction ve Qt fixture. | autotunereport.cpp, optimizetab.cpp | Model interval yoksa sahte yüzde üretilmez; measurement/model ayrımı korunur. |
| GUI-120 — Trade-off Explorer | B11 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Same-context alternate candidates/weights interactive graph, draft preview. | optimizetab.cpp, autotunereport.cpp | Alternatif seçimi donanıma yazmaz; yeniden hesaplama ve mevcut koşul belirgin. |
| GUI-121 — Before / After Benchmark | B11 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Paired measured context/unit/repeat/quality karşılaştırması ve mismatch warning. | optimizetab.cpp, autotunereport.cpp | Farklı workload/kernel/power context eşleştirme uyarısı; n/a fark sıfır değil. |
| GUI-122 — Calibration Validity Inspector | B09 | T11: Ürün/CPU/BIOS/RAM fingerprint karşılaştırması, RAM yüzde 2 asimetrik tolerans, kernel ve son kayıt/benchmark sürümleri. Gerçek model validity kararı ve context/quality gereksinimleri açık. | Gerçek model validity requirements/context/quality/freshness reason; kernel/BIOS/hardware compatibility fixture. | optimizetab.cpp, signature v10 inspector | Sadece tarih farkı invalid dememeli; desteklenmeyen imza alanı unknown. |
| GUI-123 — Game A/B Session Explorer | B11 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Run/game/context-key paired selection, measured coverage/diff graph; birimler aynı. | optimizetab.cpp, frametime.h | Farklı oyun/çözünürlük/ayarlar eşleşmez; P99 hesabı ve units aynı tanımdan. |
| GUI-125 — Selective Recommendation Apply | B11 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Stable recommendation ID, dependencies/conflicts ve stale result revalidation, mevcut Apply/ABBA sonucu ile scope doğrulama. | optimizetab.cpp | Unselected uygulanmaz; stale result yeniden doğrulanır; rollback helper korunur. |
| GUI-127 — Estimated vs Measured | B11 | T11: Gerçek predcheck mu/sd/obs, error/z, ±2 SD sayımı ve kaynak/aşama/hedef/oturum filtreli grafik. Diğer aynı-context A/B, GUI ve solver fixture açık. | Same-run/context paired measured comparison ve diğer metric units/quality coverage; predcheck error chart korunur. | autotunereport.cpp, optimizetab.cpp | Ölçülmemiş tahmin başarı olarak sunulmaz; karşılaştırılamayan metrik boş. |
| GUI-128 — Unified Incident Timeline | C01 | T9: İlişkili: kernel.log yeni full UUID, prefix adayları ayrıldı; unified incident timeline entegrasyonu açık.; T12: İLİŞKİLİ KISMİ — Scene journal transaction_id/outcome alanları, incident okuyucusunda eski success kaydı unverified; deferred olay kalıcı journal ve cross-session bağlantı açık. | Gerçek cross-boot incident session model, all adapters/source/device/time quality/clock uncertainty/dedup/filter; kernel UUID ve Scene fields yeterli değil. | evidencereports.cpp, healthevidence.cpp, flightrec.cpp | Farklı bootlar karışmaz; saat geri alınırsa chronology uncertainty gösterilir. |
| GUI-131 — Evidence Comparison | C01 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Stable finding/table row/device keys, semantic changes, severity/provenance ve unknown. | diagreportview.cpp, diagreport.cpp | Satır sırası fark sayılmaz; değişen severity ve unavailable ayrı; snapshot restart sonrası korunur. |
| GUI-132 — Diagnostic Confidence Levels | C01/C02 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Severity’den ayrı evidence quality/confidence typed states, heuristic/candidate/confirmed ayrımı. | diagreport.cpp, diagreportview.cpp, diagnostictools.cpp | Eksik veri normal sayılmaz; correlation confirmed cause olarak görünmez. |
| GUI-133 — Findings Detail Inspector | C02 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Finding-specific raw/source/limitation/tool deep link, gerçek snapshot references. | diagreportview.cpp, diagreport.cpp | Inspector filtrelenmiş rapordan değil gerçek snapshot'tan beslenmeli; eksik source açık. |
| GUI-134 — Incident Correlation | C01 | T12: İLİŞKİLİ KISMİ — Scene journal transaction_id/outcome alanları, incident okuyucusunda eski success kaydı unverified; deferred olay kalıcı journal ve cross-session bağlantı açık. | Typed cross-source event correlations/full boot/device/time tolerance/quality; causal claims yok. | evidencereports.cpp, healthevidence.cpp | Benzer timestamp nedensellik değildir; prefix-only eşleşme ayrı. |
| GUI-136 — Diagnostic Notes | C01 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Persistent session/event-key note drafts/explicit save/discard; 2000 limit korunur. | healthevidence.cpp | Filtre ve restart not kaybetmemeli; cancel/discard doğru; eski backend sınırı korunmalı. |
| GUI-137 — Advanced Report Filters | C01 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Source/device/time/review combined filters, counts ve export bağımsızlığı. | diagreportview.cpp | Filtre verdict'i ve full export'u değiştirmemeli; count doğru. |
| GUI-138 — Diagnostic Report Layout | X06 | T13: Aynı report code + benzersiz tablo ve tam satır eşleşmesi ile seçim/sort/scroll restore; section collapse ve Qt refresh testi açık. | Stable section collapse/selection, adaptive table/detail ve Qt refresh fixture; exact row restore korunur. | diagreportview.cpp | Rapor yenilendiğinde stable section state korunur; farklı rapora taşınmaz. |
| GUI-139 — Health Navigation Unification | A09 | T14: Health/global navigasyon metadata genişletildi; bütün Health route birleştirme açık (A09). | Overview/Investigation/Evidence Lab/System Tools aynı stable route/deep-link; mevcut tool key’leri korunur. | healthtab.cpp, overviewpage.cpp, toolcatalog.cpp, uinavigator.cpp | Semptom/arama/favori yolları aynı tool'a gider; arama read-only. |
| GUI-141 — Diagnostic Freshness | A03/C01 | T10 firmware read timestamp, T13 Home freshness; bütün diagnostik report standardı açık (A03/C01). | Collection vs render time, source/boot/error/freshness standardizasyonu bütün raporlar. | diagreportview.cpp, smartpage.cpp, netpage.cpp | Failed refresh eski successful time'ı güncellemez; boot değişimi işaretlenir. |
| GUI-142 — Evidence Export Selection | C03 | T7: KISMİ — Frozen export row/column seçimi, metadata ve excluded sayıları; evidence/source granular seçim ve native byte fixture açık. | Granular evidence/source metadata seçimi ve excluded sections/verdict/actual bytes fixture. | diagreportview.cpp, reportpreview.cpp | Seçilmeyen alan bytes olarak export'ta yok; tam rapor verdict değişmez. |
| GUI-143 — Privacy Preview | C03 | T7: KISMİ — Copy/Save aynı review; SHA-256, editable snapshot ve etiketli UUID redaction; tam source-aware ve actual clipboard/file native testi açık. | Tam source-aware redaction ve raw/manual override, technical IDs koruma, actual clipboard/file bytes ve frozen hash race fixture. | diagreportview.cpp, reportpreview.cpp | Sentinel secret fixture preview/copy/save'da kalmaz; redaction kapalıysa açık uyarı; kusursuz anonimlik iddiası yok. |
| GUI-146 — Structured Tool Output | C09 | T6: İLİŞKİLİ KISMİ — lsusb ve lscpu -e typed adapter, raw fallback; diğer araçlar ve Qt testleri açık. | Diğer uygun tools için bounded structured parsers/JSON/version/locale/failure, Qt fixtures. | systools.cpp, diagreport.cpp, diagnostictools.cpp | Kısmi/bozuk çıktı sıfır değer üretmez; mevcut raw veriye erişim kaybolmaz. |
| GUI-147 — Raw / Structured View | C09 | T6: İLİŞKİLİ KISMİ — Yeni adapter’lar aynı captured çıktıyı kullanır; tek dialog toggle ve model açık. | Tek dialog Raw/Structured toggle, same frozen snapshot, sort/search/selection/model/proxy/export ve no query fixture. | systools.cpp, diagreportview.cpp | Toggle yeni donanım sorgusu yapmaz; raw ve structured aynı snapshot'a ait. |
| GUI-150 — Tool Capability Indicators | X06 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Tool başına executable/kernel/device/permission requirements ve precise missing reason; read-only capability check. | toolcatalog.cpp, systools.cpp, diagreport.cpp | Eksik binary, permission ve unsupported ayrı; helper çalıştırma yalnız açık işlemde. |
| GUI-151 — Read-Only Tool Comparison | C01 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Raw-only araç snapshot/semantic or bounded line diff; stable tool/device and source identity. | diagreportview.cpp, systools.cpp | Farklı tool/device snapshot eşleşmez; restart sonrası pin korunur. |
| GUI-152 — EDID / VRR Visual Inspector | C05 | T7: KISMİ — EDID connector/byte/field/checksum inspection, bozuk VRR reddi. Native parser PASS; grafik ve Qt fixture açık. | Verified timing/VRR graph, connector disconnect ve byte field selection Qt fixture; raw/privacy ve no-write. | diagnostictools.cpp, diagsources.cpp | Corrupt/truncated extension crash olmaz; genel refresh range VRR olarak gösterilmez; yazma yok. |
| GUI-153 — PCIe Topology Visualization | C04 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Lazy tree/selection/deep link, stable identity hotplug/BDF reuse, current/max/AER details. | diagnostictools.cpp, advanceddiagnostics.cpp, diagsources.cpp | Unknown parent tahmin edilmez; hotplug/BDF reuse eski delta sayılmaz. |
| GUI-170 — Storage Health Dashboard | C10 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Storage health history stable NVMe/SATA identity ve capability, replacement fixture. | smartpage.cpp | NVMe/SATA farkı korunmalı; unsupported SMART normal sayılmaz. |
| GUI-171 — SMART Attribute Inspector | C10 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Vendor-specific raw/normalized/worst/threshold, NVMe ayrı, unknown units/threshold. | smartpage.cpp | Eksik threshold unknown, vendor-specific raw değer yanlış birime çevrilmez. |
| GUI-172 — Kernel Log Severity Filter | X07 | T9: Combined severity/text/kind/code/device-token/exact boot/date filters, context independent, match count, full UUID new records; Qt fixture çalışmadı. | Typed driver/device semantics ve context/match count/full boot/date Qt fixture; mevcut combined filters korunur. | klogpage.cpp | Severe-only filtre context penceresinde hedefi gizlememeli; kaydedilmemiş kayıt uydurulmaz. |
| GUI-175 — Boot Comparison | C01/X07 | T6: İLİŞKİLİ KISMİ — İki boot seçimi, tam ID, eski önek ambiguity guard; guard/preset history ve fixture açık.; T9: Existing two-boot comparison korundu; boot detail exact UUID ve ayrı legacy prefix candidate; diğer boot kaynakları ve Qt acceptance açık.; T13: Boot full UUID seçimi ve filtre persistence; duplicate/hidden restore reddi. Tam semantic comparison kapsamı açık. | Kernel/driver/guard/preset/pstore/full boot semantic compare, unknown counts, legacy ambiguity ve restart/context testleri. | bootpage.cpp | Tam boot ID ile eşleştir; filtre dışı boot seçili kalmaz; eksik kayıt '0 hata' sayılmaz. |
| GUI-177 — Network Workspace | X08 | T8: Kısmi — kayıtlı bağlantıdan saved-rule preview; full workspace/deep links açık. | Ortak network workspace, capability/state banner ve connection→rule→log deep links, no-write navigation. | netpage.cpp, netfirewall.cpp, netrules.cpp | Bütün alt araçlar erişilebilir; navigasyon firewall state değiştirmez. |
| GUI-178 — Process-Centric Connections | C11/X08 | T6: İLİŞKİLİ KISMİ — C11 tarih/top-N/other trafik sayaçları; PID reuse ve stable grouping açık. | PID+start/exe stable grouping/socket child/totals ve PID reuse/unknown fixture; program traffic aggregate tek başına yeterli değil. | netpage.cpp, traffic.cpp | PID reuse yanlış gruplanmaz; unknown PID ayrı; action yalnız unique socket üzerinde. |
| GUI-186 — Listening Port Inspector | X08 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Bind scope, IPv4/IPv6, process/exe/UID ve port exposure detail; source evidence, unknown process. | netpage.cpp | 0.0.0.0 ve loopback ayrımı; socket identity doğrulanmadan action yok. |
| GUI-187 — Network Table Persistence | X02 | T13: Per-shell network column order/width ve mevcut sorting destekli tablolarda sort restore; eski genişlik migration. Traffic sort bağlama göre kalıyor. Visibility/filter ve restart testi açık. | Visibility/filter ve traffic sort kararları, restart/refresh/vanished selection fixture; header persistence korunur. | netpage.cpp | Refresh ve restart filtre kaybetmez; vanished/ambiguous selection temizlenir. |
| GUI-190 — Scene Diff | D01 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Scene component/Choice semantics ve scene vs live/operation effects. | scenestab.cpp, profilediff.h | Eksik component no-op ile default ayrılmalı; secret command args loglanmamalı. |
| GUI-191 — Scene Conflict Inspector | D01 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Engine priority ve overlap/component conflict relationships; static vs runtime. | scenes.cpp, scenestab.cpp | Çakışma sonucu priority gerçekten engine ile eşleşir; dry-run side effect yok. |
| GUI-192 — Scene Execution Preview | D01 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Structured actual ordered plan old/new/skip/blocked/reason/dependency/revision. | scenes.cpp, scenestab.cpp | Preview/execute plan aynı input'ta uyuşur; preview write/command launch yapmaz. |
| GUI-193 — Automation Timeline | C01/D01 | T12: Scene journal transaction_id/outcome (C01/D01), deferred/daemon persistent events açık. | Cross-boot persistent automation trigger/rule selected/rejected, step/rollback, secret redaction. | scenes.cpp, evidencereports.cpp | Clock/boot boundaries korunur; secret arguments yok; unknown trigger uydurulmaz. |
| GUI-195 — Scene Dependency Checker | D01 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Bütün scene refs, capabilities/identity/support/ambiguity ve dependency revalidate. | scenes.cpp | Silinen profile göre apply disabled veya mevcut policy açık; preview no writes. |
| GUI-197 — Scene Apply Result | D01/A05 | T12: KISMİ — transaction UUID, step/rollback/deferred/skipped/superseded/result olayları, readback mismatch ayrı; gerçek visual builder, tam ordered plan, trigger simulation, kalıcı deferred olaylar ve daemon integration açık. | Per-component requested/verified/partial/rollback ve dış daemon/deferred persistent events/registry; full actual plan/dependency fixture. | scenes.cpp, scenestab.cpp | Partial apply success olarak gösterilmez; rollback başarısızlığı açık; diğer scene sonucu karışmaz. |
| GUI-198 — Automation Dry Run | D01 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Simulated AC/game/time/trigger input, actual engine priority/conditions step plan, zero command/write. | scenes.cpp, scenestab.cpp | Simulated AC/game/time event'leri engine kararını eşler; command/privileged call yok. |
| GUI-199 — Adaptive Keyboard Canvas | D02/X09 | T9: Per-key anchored zoom/pan/fit, inverse hit-test; 4-zone reflow. 300.000 native transform PASS; actual Qt hit/selection/DPI ve zone4 canvas açık. | Zone4 tam zoom/pan/hit-test eşdeğerliği ve actual Qt key selection/DPI/resize/no-write fixtures. | lightingtab.cpp, kblayout.cpp, zone4tab.cpp | Dar/geniş ve DPI değişiminde aynı tuş seçilir; clipping yok. |
| GUI-206 — Unsaved Lighting Preview | A05/D02 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Draft/applied/readback/pending/failed/unknown explicit status, shared dirty registry; mevcut preview tekrar yazılmaz. | lightingtab.cpp, lighting.cpp | Undo/Redo/preview donanıma yazmaz; failed apply draft'ı applied olarak işaretlemez. |
| GUI-209 — Backup History Explorer | D03 | T10: İLİŞKİLİ KISMİ — yerel verification stat kimliği, legacy ve sidecar ayrımı | Type/size/chain/encryption/verified/stale filters ve stable selection/sidecar unknown, native fixtures. | backuppage.cpp | Eksik sidecar unknown; yanlış selection restore action'a dönüşmez. |
| GUI-210 — Restore Preview | D03 | T10: İLİŞKİLİ KISMİ — config_inspect öncesi/sonrası archive stat identity kontrolü | Path manifest/target metadata/hash/create-replace-delete ve path traversal/symlink/decompression limits; helper target/archive revalidation ve no-write preview. | backuppage.cpp, mevcut backup helper | Preview hiçbir dosya değiştirmez; path traversal/symlink ve yanlış target reddedilir; confirmation korunur. |
| GUI-211 — Backup Dependency Tree | D03 | T10: KISMİ — zincirde tekrarlı veya erişilemeyen stat identity uyarısı, doğrulama zamanı | Full dependency graph/missing/cycle/ambiguous/auth distinction ve helper fail-closed, stat identity tek başına authorization değil. | backuppage.cpp | Metadata tek başına restore authorization değil; cycle/ambiguous chain fail-closed. |
| GUI-212 — Backup Progress Inspector | A05/D03 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Gerçek byte/file/phase/elapsed kapsamını kontrol et; eksik total indeterminate, verify/cancel/restore registry entegrasyonu. Mevcut progress’i tekrar yazma. | backuppage.cpp, mevcut backup helper | Yüzde total olmadan uydurulmaz; cancelled/verify/restore phase ayrı. |
| GUI-213 — Backup Integrity Report | D03 | T10: KISMİ — inode/device/size/mtime/ctime nanosecond kimliği ve canonical result eşlemesi | Cryptographic content identity/per-file/chain/auth/freshness, cancellation/password vs corruption ve restore readiness fixtures. | backuppage.cpp | İptal/password denied corrupt sayılmaz; readable ≠ authenticated/restore-ready. |
| GUI-216 — Non-Blocking GUI | A04 | T9: İlişkili: kernel helper exit/boot/sequence/write cursor guards ve typing debounce; diğer sync ve GUI responsiveness acceptance açık. | Tüm sync call graph (CO özellikle), async worker/QProcess timeout/cancel/restore/lifetime fake helper behavior. | privileged.cpp, userhelper.cpp, cosearchdialog.cpp, optimizetab.cpp, systools.cpp, cpustressbox.cpp | Fake helper yavaş/timeout/malformed iken event loop responsive; late result ignore. |
| GUI-217 — Extended Lazy Initialization | A07 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Diğer heavy pages create-on-demand ve busy lifecycle, startup construction counters. | lazywidget.h, mainwindow.cpp, healthtab.cpp | Startup tüm heavy widgets oluşturmamalı; lazy switch aktif işlemi kesmemeli. |
| GUI-219 — Adaptive Polling | A03 | T13: Home görünür Live/plot tüketicisine göre getter azaltımı; fan guard/device ve EC stream korunur. Ortak scheduler ve gerçek donanım testi açık. | Ortak subscriber visibility/freshness/AC/sleep/dGPU wake-aware scheduler ve bounded intervals. | hometab.cpp, sysinfo.cpp, netpage.cpp | Hidden tab poll düşer; aynı query çoğalmaz; uyuyan GPU sırf polling ile uyanmaz. |
| GUI-221 — Incremental Table Updates | X06 | T13: Tam satır eşleşmesiyle selection restore; widget yeniden kuruluyor, incremental update uygulanmadı. | Gerçek incremental typed model/stable row keys/changed-only 10k; mevcut rebuild+restore bunun yerine geçmez. | netpage.cpp, diagreportview.cpp, bootpage.cpp | 10k row refresh sadece değişen satırları günceller; vanished selection güvenli temizlenir. |
| GUI-222 — Unified Error Presentation | A05 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Structured error model source/op/code/retryable/cancel/stale + redacted bounded log. | mainwindow.cpp, privileged.cpp, diagreportview.cpp | Permission/timeout/malformed/cancel ayrı; secret arguments gösterilmez. |
| GUI-224 — Unified Operation State | A05 | T12: KISMİ — Scene transaction UUID ve gerçek callback olayları, deferred GPU timer/in-flight normal çıkış koruması, kısmi/rollback/unverified sonuç; ortak atomik kaynak rezervasyonu ve diğer owner/cancel/restore entegrasyonları açık. | Merkezi all-operation lifecycle transitions, duplicate finish, real restore/verify and owner/resources; Scene events korunur. | mainwindow.cpp, cpustressbox.cpp, coolingpage.cpp | Impossible transition reddedilir; restoring'de idle olmaz; duplicate finish tek kez. |
| GUI-225 — Concurrent Operation Guard | A05 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Atomik central resource lock, transaction owner ve independent read policies; no lock leak. | mainwindow.cpp, coolingpage.cpp, cpustressbox.cpp | Çakışan işlem bloke; independent read-only işlem uygun olduğunda serbest; failure lock leak yok. |
| GUI-226 — Safe Restart Coordination | A05 | T12: KISMİ — Scenes ve Spectrum Lighting unsaved quit uyarısı, Cancel varsayılan, onaydan sonra active operation tekrar kontrolü; tüm editörler ve workspace store/selection/splitter/sort/persistence açık. | Tüm config/restart/preference/close paths ve common dirty/restore guard; force-kill garanti yok. | mainwindow.cpp, hometab.cpp, configpage.cpp | Active write/restoring normal quit ile kesilmez; cancel sonrası state korunur; zorla kill garantisi yok. |
| GUI-228 — Unified Chart Engine | A06 | T9: Shared range/tick + signed bars + visible snapshot table + frozen CSV with gaps + DayBars keyboard; 300.000 native range PASS; Qt/VF/multi-series/CSV roundtrip acceptance açık. | VF/multi-series ve diğer chart API/units/time/nearest/legend/accessibility, real Qt hit-test/CSV/100k tests. | charts.cpp, charts.h, vfcurvewidget.cpp | NaN/gaps doğru, zoom selection/hit-test doğru; V/F write/edit semantics korunmalı. |
| GUI-229 — Stale Data Detection | A03 | T6: İLİŞKİLİ KISMİ — Firmware stale WMI ve doğrudan sysfs okuma ayrımı; ortak telemetry kapsamı açık.; T10: İLİŞKİLİ KISMİ — AMD GPU boottime örnek yaşı ve identity/inactive guard; firmware read timestamp; T13: Home runtime status eksikse unknown; 6s üzeri sweep graph boşluğu. Ortak freshness kapsamı açık. | Bütün consumers typed boot/device/source/freshness/quality; post-write direct, stale verification yasak. | sysinfo.cpp, hometab.cpp, netpage.cpp, smartpage.cpp, fwattrtab.cpp, optimizetab.cpp | Failed refresh eski veriyi fresh göstermez; stale boot match ve hardware verification yasak. |
| GUI-230 — Keyboard Accessibility | A01/A08 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Tüm GUI tab order/focus/name/description, keyboard-only actions/custom chart accessible fallback. | mainwindow.cpp, hometab.cpp, vfcurvewidget.cpp, tüm GUI | Mouse olmadan tüm read-only araçlar ve normal işlemler erişilir; focus trap yok. |
| GUI-231 — Persistent Widget State | X02 | T13: Per-shell sürümlü JSON store, bounded integer/order/split validation. Tüm editörler ve Qt davranış testi açık. | Tüm stable view IDs/editörler, migration/reset/filter/scroll/selection ve corrupt/restart fixture. | mainwindow.cpp, cosearchdialog.cpp, netpage.cpp, bootpage.cpp | Eski corrupt setting safe fallback; 2 shell state çakışmaz; hidden selection restore edilmez. |
| GUI-232 — Asynchronous Result Protection | A04 | T6: İLİŞKİLİ KISMİ — WMI row revision ve Autotune run ID guard; ortak async altyapı açık.; T7: KISMİ — Describe normal exit + exit code + JSON ok; CO preview revision guard. Tüm async producer/device/lifetime testleri açık.; T10: İLİŞKİLİ KISMİ — firmware worker QDialog context, backup verification result identity guard; T12: İLİŞKİLİ KISMİ — Wi-Fi rescan generation + adapter kontrolü ve eski deferred GPU sonucunun yeni Scene durumunu değiştirmemesi; genel bloklayan yollar ve Qt callback fixture açık. | Tüm helper producer request/generation/device/lifetime/cancel ve A→B→A/late result fixtures. | scenestab.cpp, privileged.cpp, userhelper.cpp | A→B→A ve reversed replies fixture; eski result hiçbir yeni editörü mutate etmez. |
| GUI-235 — Automated Visual Regression Tests | A08 | ID özelinde yeni uygulama kaydı yok. Paket düzeyindeki ilişkili ilerlemeyi ve gerçek kaynak işlevini kontrol et. | Gerçek main app fake fixture ve reviewed PNG baselines, DPI/dil/shell matrix, failure artifact, intentional regression. | gui/tests, gui/CMakeLists.txt, .github/workflows/ci.yml | Hardware helper erişimi yok; CI native test çalıştırır, intentional layout regression yakalanır. |

## 5. Uygulama sırası (kapsamı azaltmaz)
0. D05 önce: SHA doğrula, native toolchain ve testleri kur; mümkünse Tur14 temiz baseline build. Derleme hatalarını yeni özellikten önce çöz.
1. A03/A04/A05/A07/X02: typed read/cache, async lifetime, resource registry/dirty, fail-closed simulation.
2. A01/A02/A06/A08/A09/A10/X01/X03/X06: gerçek iki shell, theme/DPI/accessibility/visual, lazy navigation/model.
3. B01–B11/X04/X05: CO/GPU safety/readback, firmware, optimization per-key/ABBA, Autotune gerçek evidence.
4. C01–C13/X07/X08: identity/session/privacy, device graph, typed tools, history/network.
5. D01–D04/X09: Scene/Lighting/Backup ve FIX regresyonları.
6. D05 final: tüm acceptance tests, security review, CI ve uygun donanım doğrulaması. Yalnız başarılıysa experimental push.

## 6. Değişmez güvenlik / doğrulama sözleşmesi
- Qt ≥ 6.4 / C++20 ve mevcut Rust workspace/Cargo.lock. İki shell aynı davranış; kompakt, teknik ayrıntıları eksiltmeyen TR/EN arayüz, klavye/HiDPI. Favoriler yalnız kullanıcı eylemiyle.
- Preview/read-only/simulation hiçbir gerçek hardware write, user command, pkexec, EC/HID write, mount/restore veya daemon mutation başlatmaz. Fake backend kullan. Helper, polkit, CO crash guard, NVcurve validator, fan safety floor, ABBA/boot guard, restore confirmation korunur. Donanım güvenlik kontrollerini display cache’e bağlama.
- Unknown/stale/unsupported/draft/failed/verified farklı; model/measurement, boot snapshot/saved/live, requested/readback/factory ayrı. Restore/rollback bitmeden busy lock bırakma. Source provenance uydurma; eski readback yeni işlem kanıtı değildir.
- Persistent store bounded, versioned, atomic; corrupt/incompatible korunur veya açık hata. Secret/raw identifiers gereksiz saklanmaz. Test assertion/flag/safety silerek warning çözme.
- Her paket ve GUI ID için davranış ve failure tests, gerekirse fixture/mock, translation. String presence test tek başına kabul değildir.

### Native derleme komutları (orijinal plan)
```bash
cmake -S gui -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCENTURION_BUILD_TESTS=ON -DCENTURION_SANITIZE=ON -DCENTURION_LTO=OFF
cmake --build build-debug --parallel
QT_QPA_PLATFORM=offscreen ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir build-debug --output-on-failure
cmake -S gui -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCENTURION_BUILD_TESTS=ON
cmake --build build-release --parallel
QT_QPA_PLATFORM=offscreen ctest --test-dir build-release --output-on-failure
cargo test --workspace --locked
cargo clippy --workspace --all-targets --locked -- -D warnings
python3 gui/tests/translation_tr_test.py
git diff --check
```
CI’daki cargo-deny, Tur 14 mevcut 42 Python runner ve yeni eklenenler, native yeni tests, visual baseline artifacts ve hardware validation gerektiren noktaları ayrı raporla. Qt6-base/svg/tools/l10n, libGL, libxkbcommon, dbus ve Cargo/Rust stable workspace sürümü doğrula; kurulum engeli varsa gerçek hata logu kaydet. Fiziksel hardware yazması test için otomatik çalıştırılmaz. GitHub HEAD/experimental history/permission doğrulanır; force push yok. Native/CI engelli sürümü hazır diye yayınlama.

D05 eski engeller: `Qt6Config.cmake` yok, Cargo/rustc PATH üzerinde yok, apt DNS/indeks sorunu. Önce yeni ortamda yeniden doğrula. CTest “No tests were found” PASS değildir. Statik runner PASS Qt build/test yerine geçmez. Gentoo ebuild/PGO doğrulaması ayrı.

## 7. Yeni sohbete tek talimat
> Tur 14 kaynak ZIP ve bu devir raporu tek güncel temel. SHA ve native build/test baseline doğrula. 48 paketin bütün açık alt işlerini, 118 GUI ID matrisindeki kabul kriterlerini ve paket düzeyindeki ek ID/FIX kriterlerini kaynakta karşılaştırarak tamamla. Tur 6–14’te eklenen çalışan özellikleri yeniden yazma; belgelenmemiş alt davranışları önce mevcut kodda kontrol et. Her paket için gerçek diff/fonksiyon, kalan davranış, test adı/çıktı ve PASS/FAIL/BLOCKED kaydet. Küçük altyapı veya source test PASS ile tam kabul verme. Qt6 Debug/Release, CTest, GUI sanitizer/visual, Rust locked build/test/clippy/deny ve güvenli fixture’ları çalıştır; hataları/warning’leri düzelt. Mevcut CO/NVcurve/fan/ABBA/boot/restore güvenliğini koru. Kapsamı azaltma; fiziksel write testini otomatik yapma. Ancak tam kabul sonrası experimental’a normal commit/push; main/old untouched. Her teslimatta güncel kaynak ZIP ve gerçek kalan işler ver.

| Paket / ID | Mevcut fonksiyon | Gerçek diff | Kalan alt iş | Test adı/çıktı | PASS / FAIL / BLOCKED / Kısmi | Fiziksel doğrulama |
|---|---|---|---|---|---|---|

## 8. Kaynak izlenebilirliği
- Tur 5 orijinal birleşik planı ve Tur 6–14 doğrulama raporları; Tur 14 118 GUI TSV. Kaynak ZIP’ler doğrudan içerik hash ile karşılaştırıldı.
- Tur 14 raporu ve ham kanıt ZIP’inde 42 runner çıkışı, Qt configure ve ortam logları. Bu rapor yeni test kanıtı değildir.
- Aşağıdaki manifest değişikliklerin içeriğini tek başına doğrulamaz; yeniden inceleme sırasında alanı daraltır.

### Tur 5 → Tur 14 değişen dosyalar
```text
.github/workflows/ci.yml
crates/centurion-helpers/src/autotune.rs
crates/centurion-helpers/src/bin/netguard-helper.rs
crates/centurion-helpers/src/netguard.rs
crates/centurion-helpers/src/wifi.rs
gui/CMakeLists.txt
gui/i18n/centurion_tr.ts
gui/src/amdgpu.cpp
gui/src/amdgpu.h
gui/src/amdgputab.cpp
gui/src/amdgputab.h
gui/src/backuppage.cpp
gui/src/backuppage.h
gui/src/bootadvisor.cpp
gui/src/bootpage.cpp
gui/src/bootpage.h
gui/src/calibrationcoverage.h
gui/src/charts.cpp
gui/src/charts.h
gui/src/coarchive.h
gui/src/cosearchdialog.cpp
gui/src/costrategy.cpp
gui/src/costrategy.h
gui/src/diagnostictools.cpp
gui/src/diagnostictools.h
gui/src/diagreport.cpp
gui/src/diagreport.h
gui/src/diagreportview.cpp
gui/src/diagsources.h
gui/src/fwattrtab.cpp
gui/src/fwattrtab.h
gui/src/healthtab.cpp
gui/src/hometab.cpp
gui/src/hometab.h
gui/src/insights.cpp
gui/src/int64spinbox.cpp
gui/src/int64spinbox.h
gui/src/inteltab.cpp
gui/src/klogpage.cpp
gui/src/klogpage.h
gui/src/klogview.cpp
gui/src/klogview.h
gui/src/lightingtab.cpp
gui/src/lightingtab.h
gui/src/mainwindow.cpp
gui/src/mainwindow.h
gui/src/netfirewall.cpp
gui/src/netfirewall.h
gui/src/netpage.cpp
gui/src/netpage.h
gui/src/netrules.cpp
gui/src/netrules.h
gui/src/nvidiatab.cpp
gui/src/optimizetab.cpp
gui/src/optimizetab.h
gui/src/ryzentab.cpp
gui/src/scenes.cpp
gui/src/scenes.h
gui/src/scenestab.cpp
gui/src/scenestab.h
gui/src/structuredtools.h
gui/src/systemdiagnostics.cpp
gui/src/systools.cpp
gui/src/theme.cpp
gui/src/theme.h
gui/src/toolcatalog.cpp
gui/src/traffic.cpp
gui/src/traffic.h
gui/src/uinavigator.cpp
gui/src/zone4tab.cpp
gui/src/zone4tab.h
gui/tests/appearance_layout_test.cpp
gui/tests/calibrationcoverage_test.cpp
gui/tests/coarchive_test.cpp
gui/tests/costrategy_test.cpp
gui/tests/diagnostictools_test.cpp
gui/tests/experimental_layout_static_test.py
gui/tests/gui_completion_static_test.py
gui/tests/gui_stage3_static_test.py
gui/tests/gui_stage4_static_test.py
gui/tests/int64spinbox_test.cpp
gui/tests/klogview_test.cpp
gui/tests/netrules_test.cpp
gui/tests/structuredtools_test.cpp
gui/tests/studio_v4_static_test.py
gui/tests/theme_runtime_test.cpp
gui/tests/toolcatalog_test.cpp
gui/tests/traffic_test.cpp
tests/test_evidence_lab_static.py
```
### Yeni dosyalar
```text
docs/GUI-PLAN-IMPLEMENTATION-ROUND6.md
docs/GUI-ROUND6-ACCEPTANCE.tsv
docs/GUI-TUR7-118-KABUL.tsv
docs/GUI-TUR7-DOGRULAMA.md
docs/GUI-TUR8-118-KABUL.tsv
docs/GUI-TUR8-DOGRULAMA.md
docs/GUI-TUR9-118-KABUL.tsv
docs/GUI-TUR9-DOGRULAMA.md
gui/src/archiveidentity.h
gui/src/autotuneprogress.h
gui/src/calibrationmath.h
gui/src/canvasviewport.h
gui/src/chartaxis.h
gui/src/edidfields.h
gui/src/firewallpreview.h
gui/src/hardwarefields.h
gui/src/integerunits.h
gui/src/navigationtargets.h
gui/src/sceneoutcome.h
gui/src/topologyfields.h
gui/src/wifiage.h
gui/src/workspacefields.h
gui/src/workspacestate.h
gui/tests/archiveidentity_native_test.py
gui/tests/autotuneprogress_test.cpp
gui/tests/calibrationmath_native_test.py
gui/tests/calibrationviewer_static_test.py
gui/tests/canvasviewport_native_test.py
gui/tests/chartaxis_native_test.py
gui/tests/edidfields_native_test.py
gui/tests/firewallpreview_native_test.py
gui/tests/hardwarefields_native_test.py
gui/tests/integerunits_native_test.py
gui/tests/navigationtargets_static_test.py
gui/tests/navigationtargets_test.cpp
gui/tests/scene_wifi_contract_static_test.py
gui/tests/scene_wifi_native_test.py
gui/tests/topologyfields_native_test.py
gui/tests/workspacefields_native_test.py
gui/tests/workspacestate_test.cpp
tests/test_firewall_preview_contract_static.py
```
