# Centurion Experimental — Tur 6 kısmi uygulama ve doğrulama raporu

**Teslimat durumu: TAM KABUL YOK — kısmi geliştirme.**

Kapsam değiştirilmemiştir: Tur 5 birleşik planındaki 48 yürütme paketi ve 118 GUI ID geçerlidir. Bu çalışma 7 pakette yeni alt işlev, 3 ilişkili pakette sınırlı değişiklik ve D05 doğrulama denemesi içerir. Diğer 37 paketin kalan işlevleri bu tur uygulanmadı. Hiçbir paket veya GUI ID bütün kabul kriterleriyle doğrulanmış sayılmadı. Bu bir tamamlanmış experimental release değildir; derlenebilirlik dahi Qt/Rust bütününde kanıtlanmamıştır. Planın tüm kapsamını gerçekleştirme talebi karşılanmamıştır. Bağımlılık engeli yalnız native doğrulamayı açıklar; uygulanmayan işlevlerin ayrıca açık olduğu aşağıda belirtilmiştir.

## 1. Kaynak ve güvenlik sınırı

- Temel ZIP: `Centurion-experimental-gelistirme-tur5(1).zip`, SHA-256 `f3852209269b0a31ea57c1ef84a19091a8d3921bf62c1eafddb49ea950739a04` (planla eşleşiyor).
- Mevcut 554 kaynak dosyası korundu; değişen ve eklenen dosyalar ayrı manifestte. Kaynakta donanım yazımı yapan helper’lar gerçekten çalıştırılmadı. Mevcut CO crash guard, NVIDIA flatten/safety, backup restore ve tuning helper akışları yeniden yazılmadı. Firmware GUI değişikliğinde mevcut backend write validator’ları korunmuştur; gerçek donanım güvenliğinin tam testi yapılmadı.
- ZIP içinde `.git` yok. GitHub HEAD/experimental push yapılmadı; main/old değiştirilmedi. Plan D05 kabul kapısı açık.
- İki eski kaynak sözleşmesi testi güncellenmiştir: sabit rail ölçüsü yerine splitter ve persisted width, Home’daki eski startup anahtarı yerine MainWindow’daki migration ve shell scope. Güvenlik testleri kaldırılmadı. Kaynak sözleşmesi PASS runtime PASS değildir.

## 2. Test kanıtları

| Kontrol | Durum | Açıklama |
|---|---|---|
| 28 Python test runner | PASS | 28/28, bazıları native C++ derleyip çalıştırır; diğerleri kaynak kontrolü/fixture. |
| CO reboot native | PASS | 183.621 crash + 327.669 clean senaryo. |
| CO walk native | PASS | 454.710 yürüyüş, 2.353.711 probe, en fazla 50/core. |
| Yeni integerunits native | PASS | Debug, O2, ASan/UBSan; her biri 200.000 roundtrip. |
| Mevcut telemetrycache native | PASS | Debug, Release, ASan/UBSan; -Wall -Wextra -Werror. |
| TR kaynak kontrolü | PASS | 50 yeni katalog girdisinden sonra tekrar çalıştırıldı. |
| Qt Debug ve Release | ENGELLİ | Qt6Config.cmake yok; configure ve build exit 1. |
| CTest | ÇALIŞMADI | No tests were found; configure başarısız. Exit 0 başarı değildir. |
| Rust build/test/Clippy/cargo-deny | ENGELLİ | Cargo bulunamadı (exit 127). |
| Apt | ENGELLİ | DNS/indeks ve paket bulunamama; install exit 100. |
| Gerçek Qt görsel regresyon/GUI ASan | ÇALIŞMADI | Native Qt bağımlılıkları eksik. |
| Gerçek donanım/privileged fixture, CI, branch push | ÇALIŞMADI | Tam güvenlik ve release kabulü yok. |

### Çalıştırıcı sonuçları

| Test | Sonuç |
|---|---|
| `gui/tests/advanced_17_static_test.py` | PASS |
| `gui/tests/boot_preset_home_ui_static_test.py` | PASS |
| `gui/tests/co_reboot_native_test.py` | PASS |
| `gui/tests/co_walk_native_test.py` | PASS |
| `gui/tests/experimental_layout_static_test.py` | PASS |
| `gui/tests/experimental_ui_static_test.py` | PASS |
| `gui/tests/gui_completion_static_test.py` | PASS |
| `gui/tests/gui_stage1_static_test.py` | PASS |
| `gui/tests/gui_stage2_static_test.py` | PASS |
| `gui/tests/gui_stage3_static_test.py` | PASS |
| `gui/tests/gui_stage4_static_test.py` | PASS |
| `gui/tests/health_catalog_redesign_static_test.py` | PASS |
| `gui/tests/health_ui_static_test.py` | PASS |
| `gui/tests/integerunits_native_test.py` | PASS |
| `gui/tests/optimization_home_test.py` | PASS |
| `gui/tests/six_features_static_test.py` | PASS |
| `gui/tests/studio_v4_static_test.py` | PASS |
| `gui/tests/theme_palettes_static_test.py` | PASS |
| `gui/tests/title_case_static_test.py` | PASS |
| `gui/tests/toolcatalog_static_test.py` | PASS |
| `gui/tests/translation_tr_test.py` | PASS |
| `tests/nvcurve_safety_static_test.py` | PASS |
| `tests/test_algorithm_guardrails_static.py` | PASS |
| `tests/test_compare_autotune_timings.py` | PASS |
| `tests/test_evidence_lab_static.py` | PASS |
| `tests/test_health_diagnostics_contract.py` | PASS |
| `tests/test_nov2026_security_fixes_static.py` | PASS |
| `tests/test_session_audit_static.py` | PASS |

## 3. 48 iş paketi — gerçek durum ve orijinal açık kabul kapsamı

**ÖNCEKİ KISMİ:** yeni uygulama yapılmadı; önceki Tur 5 mevcut parçaları korunuyor, kabul açık. **KISMİ:** bu tur somut kod değişikliği var ama tüm kriterler açık. **ENGELLİ:** doğrulama tamamlanamadı. Orijinal kalan iş sütunu değişmeden taşınmıştır; kısmi satırlarda yeni kod ile birlikte okunmalıdır.

### A01 — Ortak design system, tam yoğunluk ve kontrast matrisi / P0/P1 / 16, 23, 25–29

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** theme.cpp/.h, hometab.cpp, theme_runtime_test.cpp, tools/audit_palette_contrast.py: palette token contrast matrisi, özel tema kontrolü, seçili metin için kontrastı en yüksek siyah/beyaz renk ve filtreli rapor.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Ortak semantik rol/typography/spacing/density/ikon/table/status API; mevcut contrastChecks ve 57 tema raporunu genişlet, 737 eşik altı token çiftini görsel kimliği bozmadan düzelt veya gerekçeli istisna kaydet; rendered hover/focus/disabled/error/chart contrast ve custom palette. Sabit boyutları font-aware yap, renk dışı durum işaretleri ve OLED tercihlerinin kontrolünü tamamla.

**Kaynak:** theme.cpp:contrastChecks; hometab.cpp:Palette Contrast Audit; theme.*, experimentalui.h, tüm GUI

**Orijinal kabul testi (tam PASS yok):** 57 palet ve geçerli özel palet için kontrast raporu; %100/125/150/200 DPI ve TR/EN'de temel kontroller taşmaz; kritik durumlar renksiz de anlaşılır; kompakt yoğunluk sabit ölçülü kontrollerde de gerçekten işler.

### A02 — Canlı tema değiştirme / P1 / 17

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** `theme.cpp/.h`, `charts.cpp`, `simulation.cpp`: 19 renk için sabit adresli storage, doğrulanmış live palette API, revision sinyali ve güncel DayBars renkleri; izole simülasyon ekranında canlı tema.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Normal MainWindow/Home tema seçicisini canlı API’ye geçir; yerel QSS, QIcon/QPixmap, item brush ve QPainter cache invalidate, açık ve lazy widget revizyonu; custom paleti atomik doğrula, hatada eski palet; scroll, selection, undo ve devam eden iş korunsun. Restart yalnız shell/dil gibi gerekli durumlarda. Repolish ölç.

**Kaynak:** theme.cpp:applyLivePalette; simulation.cpp çağırıyor; hometab.cpp restart yolu; A01, theme.*, main.cpp

**Orijinal kabul testi (tam PASS yok):** açık curve editörü ve model simülasyonunda devam eden işlem sırasında art arda 20 tema değişimi; değer/selection/undo/işlem kaybı, crash veya yeni donanım çağrısı yok. Custom palette hatasında eski tema korunur.

### A03 — Ortak telemetri cache ve geçerlilik modeli / P0/P1 / 218, 219, 229

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** `telemetrycache.h`, `sysinfo.cpp/.h`: Home ve metinsel D-Bus Sensors ortak altı sabit boyutlu display slotu; monotonic/boottime TTL, single-flight, generation invalidation, unavailable/pending/expired/failed ayrımı.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Altı textual slotu metric+device+unit+boot+source+quality+timestamp+last error taşıyan ortak async subscription servisine genişlet. Numeric DBus, Health/AMD/NVIDIA/Home tüketicileri, bounded queue, timeout/cancel, visibility/AC/sleep polling; stale gösterimi ve grafik boşluğu. Safety ve post-write readback doğrudan kalacak; GPU uyandırma opt-in.

**Kaynak:** telemetrycache.h:Slot, Sample; sysinfo.cpp:displayClock; sysinfo.*, Home ve sensör tüketicileri

**Orijinal kabul testi (tam PASS yok):** Home+Health+stress panelinde aynı metrik istenince tek sorgu; stale/timeout/uyuyan GPU/cihaz değişimi fixture'ları; geç örnek yeni veriyi ezmez; eski sıcaklık canlı diye gösterilmez.

### A04 — Kalan bloklayan çağrılar ve geç sonuç koruması / P0 / 216, 232, FIX-02/03/05

**Durum:** KISMİ — ilişkili değişiklik.

**Tur 5 dayanağı:** Önceki asenkron ağırlık ve Wi-Fi guard + `optimizetab.cpp`: Autotune başlangıçtaki taslak snapshot’ını tamamlanınca yeniden kontrol eder; değişiklik varsa sonucu salt okunur açar. Çıkış kodu/timeout başarısızsa JSON ok true olsa da sonuç yüklenmez.

**Bu tur yapılan:** Firmware satır revizyonu ve WMI in-flight guard; Autotune run ID kontrolü.

**Açık kalan:** CO sync teardown/start/resume/reset, bütün helper call graph, timeout/cancel/restoration kapsamı eksik.

**Orijinal kalan kapsam:** CO başlangıç/resume/reset/stress teardown ve kalan GUI thread sync process/file/IPC yollarını envanterle; asenkronlaştır, request/device/session/revision/QPointer, timeout/cancel ve restoration busy. Mevcut Wi-Fi/Autotune guard korunur; güvenli journal sırası değişmez.

**Kaynak:** cosearchdialog.cpp:waitForStarted/waitForFinished (456,459,468,475,700); privileged.cpp; userhelper.cpp; privileged.*, userhelper.*, CO/Optimize

**Orijinal kabul testi (tam PASS yok):** mock helper 60 saniye cevap vermediğinde event loop ve Cancel çalışır; geç callback ve kapanmış widget fixture'ları; timeout success değildir. CO crash simülasyonlarının sonuçları değişmemeli.

### A05 — Ortak işlem registry, hata/işlem geçmişi, kaynak çakışma koruması / P0 / 11, 222–226

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** `mainwindow.cpp/.h`, `tray.cpp`, `optimizetab.h`: işlem merkezi gözlemlenen geçiş geçmişi (200 satır), tray ve gerçek pencere çıkışında busy guard; Autotune busy durumuna dahil.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Polling geçmişi yerine olay tabanlı gerçek operation registry: ID, owner, kaynak, phase, progress, result, restore, cancel capability. Atomik read/write rezervasyonları, scene transaction owner propagation, dış helper lock gözlemi, bütün normal quit/restart/restore yolları + dirty kontrolü. Bounded redacted hata/bildirim geçmişi, ilgili sayfa linki. Unknown outcome success değildir.

**Kaynak:** mainwindow.cpp:operationHistory_; tray.cpp; scenes.cpp:stepState; A04, mainwindow.*, scene/test/helper akışları

**Orijinal kabul testi (tam PASS yok):** eşzamanlı CO/scene/restore çakışma testleri, restoration bitmeden kaynak bırakılmaması, bağımsız salt okunur sorguların çalışması, işlem geçmişinin gerçek result ile eşleşmesi, yarım işlemin başarı göstermemesi.

### A06 — Ortak grafik etkileşimi ve zoom / P1 / 228

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** `charts.cpp/.h`: LineChart zoom, klavyeli pan/reset, görünür/tüm CSV, finite sample ve gap kontrolü, min/max bucket.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** LineChart zoom/pan/gap/downsample/CSV korunarak ortak axis/range/units/time transform ve tick API; BarChart/DayBars/VF adaptasyonu, multi-series legend, erişilebilir tablo, nearest-point; visible/all export, zoom sadece görünüm. 100k performance, NaN/inf, tek nokta/negatif/küçük aralık testleri.

**Kaynak:** charts.cpp:bucket, setViewRange; vfcurvewidget.cpp; charts.*, vfcurvewidget.*

**Orijinal kabul testi (tam PASS yok):** 100k örnekte akıcı çizim, kayıp veri boşlukları, tek nokta/negatif değer/çok küçük pencere, CSV roundtrip, V/F mevcut güvenlik testleri.

### A07 — Donanım simülasyon modu / P1 / 234

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** `main.cpp`, `simulation.cpp/.h`: `--simulate` ile normal shell/instance/socket/daemon/helper yollarından ayrı izole workspace; geçici XDG ve fixture kökü, 6 senaryo, gerçek V/F ve LineChart, S10/S13 gerçek parser, simulated Apply sadece bellek.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Ayrı güvenli simulation workspace’i koru; gerçek MainWindow ve bütün ana sayfaları injected sysinfo/diagnostics/privileged/userhelper/DBus/firmware/GPU/CPU/scene/lighting backend ile çalıştır. Fake config/state, fake Apply bellek içi; fail-closed host access, permission/timeout/offline/boot/init fixtures, host sentinel ve exec/write counters.

**Kaynak:** main.cpp:--simulate; simulation.cpp; lazywidget.h; A03–A05, lazywidget.h

**Orijinal kabul testi (tam PASS yok):** gerçek sistem erişimlerini sayan fake backend ile sıfır privileged çağrı/yazma; host dosya sentinel'leri değişmez; 5+ donanım fixture'ı ile her ana sayfa açılır; hata ve yarış koşulu testi yapılır.

### A08 — Gerçek görsel regresyon altyapısı / P0 / 235, 28

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** `simulation_visual_test.cpp`, CMake/CI, `GUI-SIMULATION-VISUAL-TESTS.md`: 4 tema, 6 fixture, 2 panel, 2 boyut, 4 DPI, TR/EN ve iki shell-style seçimi; pairwise; repeat-render ve optional reference pixel comparison, geometry, PNG artifact.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Var olan native harness üzerine gerçek Classic/Experimental ana sayfaların onaylı PNG baseline’ları; sabit Qt/font/locale/clock, 57 palette component test ve pairwise TR/EN, density, %100–200 DPI, 900×600/1280×800/1920×1080. Clipping/focus/contrast assertions, intentional regression, before/after/diff artifacts; baseline yoksa smoke ayrı, başarılmış regression değil.

**Kaynak:** gui/tests/simulation_visual_test.cpp:CENTURION_VISUAL_BASELINES; .github/workflows/ci.yml; A01–A07, gui/tests, CI

**Orijinal kabul testi (tam PASS yok):** bilerek kırpılmış label, yanlış kontrast, küçük pencere taşması, theme refresh hatası testte yakalanır; CTest ve GitHub CI gerçek Qt native testlerini çalıştırır.

### A09 — Açılmamış tüm ayarların indekslenmesi, fuzzy arama / P1 / 01–02

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** `uinavigator.cpp`, `toolcatalog.cpp/.h`, `optimizetab.cpp/.h`, `tuningnavigation.h`, `tools/generate_navigation_index.py`: 279 optimization metadata kaydı, fuzzy arama, stable key ile focus ve mevcut alt tab indeksleri.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** 279 optimization kaydı/fuzzy ranking korunur. Açılmamış firmware, CPU/GPU/fan ve tüm alt tool/setting metadata’sı, stable ID/focus, TR/EN alias/risk/capability; duplicate/limit ve lazy constructor/helper counter fixture. Health deep-link yollarını birleştir.

**Kaynak:** uinavigator.cpp; tuningnavigation.h; tools/generate_navigation_index.py; uinavigator.*, toolcatalog.*, tuning metadata

**Orijinal kabul testi (tam PASS yok):** hiç açılmamış setting ve tool bulunur, typo/alias eşleşir, aynı ID tek sonuç, lazy widget sayaçları arama sırasında artmaz.

### A10 — Dashboard panel sıralaması ve tam panel görünürlüğü / P2 / 31

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** `hometab.cpp`: experimental panel order, klavyeli sıralama, reset, iki opsiyonel panel görünürlüğü ve reflow hysteresis.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Experimental sıralama ve iki panel görünürlüğü korunur; Classic için bağımsız tercih/migration, tam güvenli panel görünürlüğü/geri erişim, capability-aware compact device controls ve adaptif grid, gizli sensör polling azaltma; restart/DPI test. Kritik kontrolleri kaybettirme.

**Kaynak:** hometab.cpp; experimentalui.h; hometab.*, experimentalui.h

**Orijinal kabul testi (tam PASS yok):** sıralama ve görünürlük restart sonrası korunur, hiç favori eklenmediyse boş kalır, iki UI modunun tercihleri birbirini bozmaz, Home gizli sensörler gereksiz sorgulanmaz.

### B01 — Tam CCD stability ve kalıcı CO oturum arşivi / P1 / 43–46, 51, 57

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** `coarchive.h`, `cosearchdialog.cpp/.h`: mevcut v2 oturum arşivi atomik, kilitli, 40 oturum / 1 MiB sınırlı; aşırı büyük kayıtların eski denemeleri açık sayaçla çıkarılır; bozuk kayıt üzerine yazılmaz. CCD/slot/logical CPU, arama durumu, son başarısız değer, doğrulama ve reset bilgisi görüntülenir. Aktif journal değiştirilmedi.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Mevcut bounded CO archive/CCD inspector üzerine CPU/topology fingerprint + session/boot identity ve doğrulanmış SMT/V-Cache metadata; kanıt temelli stability heatmap, çelişkili evidence, core/candidate filter, çoklu session diff, archive export/delete (aktif journal hariç), CCD/slot semantik profil diff; migration ve corruption fixture.

**Kaynak:** coarchive.h; cosearchdialog.cpp:showSessionArchive; costrategy.h; cosearchdialog.*, costrategy.*, coevidence.*, coinsights.*, profilediff.h

**Orijinal kabul testi (tam PASS yok):** iki oturum, CPU topolojisi değişmiş profil, reboot sonrası kayıt ve bozuk arşiv fixture'ı; seçim/filtre aktif denemeyi etkilemez; eski journal migration ve crash testleri geçer.

### B02 — Gerçek karar açıklaması, resume preview, dinamik ETA / P0/P1 / 47, 59, 60

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** `costrategy.cpp/.h`, `cosearchdialog.cpp`: Search kopyası üzerinde const preview, resume'da sonraki aday ve test süresi.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Const preview ve resume candidate korunur; gerçek algoritma dallarından typed decision/reason, guard exclusion/replay ve güvenilir son değer; journal revision ile preview invalidate; gerçek event timeline ve Trial::seconds + overhead + kalan deneme aralığıyla dinamik ETA/unknown. Crash guard ve replay kuralları değişmez.

**Kaynak:** costrategy.h:previewNext; cosearchdialog.cpp:1455; costrategy.*, cosearchdialog.*

**Orijinal kabul testi (tam PASS yok):** preview çağrısı öncesi/sonrası `save()` eşit; clean replay ve crash sonrası aday gerçek dispatch ile eşleşir; binlerce fixture üzerinden crash guard sınırları ihlal edilmez, ETA sonlanan işlemde sıfır, aktif retry'da sahte tamamlanma yok.

### B03 — V/F eksik katman, validation, kalıcı history ve transaction readback / P0/P1 / 61–64, 66–68

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** `nvidiatab.cpp/.h`, `curvereadback.h`, `nvcurve-root-helper.rs`: request UUID ile işlem sonuçlarını eşleştirme, gerçek GPU nokta indeksleri ve uV düzeni, apply öncesi helper kilidi altında model adı/nokta kontrolü, eski veya geçersiz readback reddi, reset sonrası gerçek offset okuması.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Profil overlay, delta plot, linked point inspector ve profile metadata; UUID+VBIOS/driver/topology fingerprint (aynı model farklı GPU ayrımı); Rust validator salt okunur structured invalid point/reason, aynı flatten hesaplama yolu. Apply/Reset/Trial fresh readback transaction snapshot, gerçek quantization/adjusted/missing/partial ve clock limit ayrımı, kalıcı bounded operation journal, Xid boot/GPU/session ilişkisi; GUI ve helper birlikte.

**Kaynak:** curvereadback.h:Identity model+indices+voltages; nvidiatab.cpp:reportClamping; nvcurve-root-helper.rs; nvidiatab.*, vfcurvewidget.*, nvcurve crate

**Orijinal kabul testi (tam PASS yok):** helper invalid point fixture grafikte doğru nokta; eski readback yeni işlemi doğrulamaz; GPU değişimi ve başarısız/adjusted readback; Undo/Redo hiçbir donanım yazması yapmaz; mevcut flatten/safety testleri korunur.

### B04 — Firmware Power Limit Hierarchy / P2 / 84

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** systemdiagnostics.cpp: mevcut güç raporunda kaynak hiyerarşisi, birim ve missing state ayrımı.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** S08 source gruplamasını doğrudan firmware inspector’a bağla; platform profile, live/draft/default, PL/PPT/STAPM ve gerçek dependency metadata; ilişki yoksa unknown, kaynak sıralamasını enforcement hierarchy sanma. WMI hata/ünite fixture.

**Kaynak:** systemdiagnostics.cpp:Power Limit Sources; fwattrtab.cpp; fwattrtab.*, platformprofile.*, helper capabilities

**Orijinal kabul testi (tam PASS yok):** farklı model/eksik limit fixture'ı, WMI erişim hatası, birim dönüşümü; hierarchy açılırken write helper çağrısı yok.

### B05 — Intel Voltage Domain ve P/E topology / P1 / 85–86

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** inteltab.cpp/.h, systemdiagnostics.cpp: son helper mailbox verisiyle voltage domain ve CPU package/core/SMT inceleyicisi.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Mevcut Intel mailbox ve CPU topology raporunu gerçek domain coupling/support, package/core/P/E/SMT/online, domain power/voltage detail deep link ile tamamla. P/E yalnız reliable core_type/helper, fallback ve unsupported write guard.

**Kaynak:** inteltab.cpp:Voltage Domains and CPU Topology; systemdiagnostics.cpp:cpuTopology; inteltab.*, sysinfo.*, mevcut Intel helper

**Orijinal kabul testi (tam PASS yok):** P/E + SMT, hibrit olmayan ve kısıtlı helper fixture'ları; mevcut undervolt editör ve değerleri aynı kalır; unsupported domain'e yazma düğmesi açılmaz.

### B06 — AMD GPU Frequency Visualization / P2 / 90

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** amdgputab.cpp/.h: seçili kartın son örneğine bağlı core/VRAM frekans, mevcut DPM/OD noktaları, draft aralık BarChart; kart değişiminde/eski veride temizleme.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Seçili kartın frequency overview’ını shared telemetry freshness/identity ve gerçek min/max/DPM/core/VRAM/draft ile tamamla; capability varsa aynı editor validation üzerinden etkileşim, yoksa read-only; multi-GPU, min>max, sleep fixture.

**Kaynak:** amdgputab.cpp:frequencyOverview; amdgpu.*, amdgputab.*

**Orijinal kabul testi (tam PASS yok):** iki kart arasında seçim, farklı aralıklar, min>max invalid, eksik DPM, uyuyan dGPU; read-only graph hiçbir yazma başlatmaz.

### B07 — Optimization source/conflict/device/detail ve işlem journal'ı / P0/P1 / 95, 100, 103–104, 106–107, 110–111

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** `optimizetab.cpp/.h`: son helper snapshot ve gerçek policy metadata üzerinden source/scope/readback, conflict/coverage ve drift inceleyicisi; kanıtı olmayan kaynak unknown.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Source/conflict workspace korunur; stable per-device inspector/hotplug, gerçek provenance ve freshness, semantic preset vs live/boot diff, coverage/merge/replace, conflict/dependency ve birleşik filters. Gerçek per-key helper result requested/skipped/failed/applied-unverified/match/mismatch + transaction journal/boot-live scope ve kolon order/visibility/persistence; ABBA/boot guard.

**Kaynak:** optimizetab.cpp:sourceWorkspace; driftwatch.*; tune-helper; optimizetab.*, tuningviewstate.h, driftwatch.*, tune-helper

**Orijinal kabul testi (tam PASS yok):** boot preset yüklü ama checkbox boş, başka servis drift, unsupported per-NVMe, kısmi helper hata, stale readback, yeniden açılışta kolonlar; ABBA ve boot guard testleri geçer.

### B08 — Restart sonrası kalıcı optimization draft / P1 / 102

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** optimizetab.cpp/.h: manuel atomik sürümlü VM/scheduler draft save/restore/delete, fingerprint, preview, live/boot mismatch, restore sonrası checkbox boş.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Manuel VM/scheduler draft save/restore korunur; opt-in startup restore, diğer scope/per-device ancak stable fingerprint ile, revision/schema migration, invalid/missing reporting, discard/reset ve live/boot conflict. Startup/restore hiçbir ayarı uygulamaz; checkbox bağımsız.

**Kaynak:** optimizetab.cpp:draftWorkspace; B07

**Orijinal kabul testi (tam PASS yok):** reboot/restart sonrası taslak korunur, boot/live değer değişmişse mismatch gösterilir, bozuk draft'ta güvenli fallback, start'ta sıfır tuning yazması.

### B09 — Etkileşimli weight/coverage/validity ve kalibrasyon yönetimi / P1 / 113, 116–117, 122

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** `calibrationcoverage.h`, `optimizetab.cpp`: v10 gerçek y alanında sonlu ölçüm sayıları; phase/kernel/context/power-source grupları, dört hedef için ayrı kayıt sayısı ve bilinmeyen/nonpositive weight.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Coverage count tablosu üzerine modelin gerçek ölçüm gereksinimleri/freshness/quality/validity ve kernel/BIOS/hardware mismatch gerekçesi; legal weight radar/preset overlay/numeric edit. Sürüm doğrulamalı export/import, desteklenirse pasif session yönetimi, atomic backup/rollback, aktif calibration/ABBA sırasında mutation yok.

**Kaynak:** calibrationcoverage.h; optimizetab.cpp:editAutotuneWeights; optimizetab.*, autotunereport.*, signature v10

**Orijinal kabul testi (tam PASS yok):** v10 gerçek fixture, eski/bozuk/yeni bilinmeyen schema, fingerprint mismatch, eksik context, legal weight maksimumları; yönetim işlemi modele gizlice yeni ölçüm yazmaz.

### B10 — Autotune Progress Timeline / P0 / 118

**Durum:** KISMİ — yeni uygulama.

**Tur 5 dayanağı:** `optimizetab.cpp/.h`, `centurion-autotune.rs`: mevcut gerçek CENTURION_STAGE stderr olaylarını süreli zaman çizelgesinde gösterir, sıra dışı/tekrarlı aşamaları geçer, hata kuyruğunu sınırlar; CLI --progress opt-in. Yüzde uydurulmaz; sonuç ancak başarılı exit + ok JSON ile kabul edilir.

**Bu tur yapılan:** `autotune.rs`, `autotuneprogress.h`, `optimizetab.cpp`: opt-in run ID/version/sequence/phase JSON stderr, eski CENTURION_STAGE uyumu, geç/tekrarlı/yanlış ID olay reddi; sonucu yalnız exit+ok doğrular. Yeni CTest ve Rust fixture.

**Açık kalan:** Gerçek A05 registry, kullanıcı cancel, tam malformed stream stres testi ve Qt/Rust native sonuçları eksik.

**Orijinal kalan kapsam:** Mevcut CENTURION_STAGE ve elapsed timeline yerine/üzerine geriye uyumlu opt-in structured event channel: run/session ID, sequence, phase, done/total yalnız biliniyorsa. Rust gerçek solver events, bounded parser, malformed/late ignore, registry progress ve güvenli cancel; stdout result protokolü bozulmasın.

**Kaynak:** optimizetab.cpp:CENTURION_STAGE; centurion-autotune.rs; centurion-autotune.rs, autotune.rs, A05

**Orijinal kabul testi (tam PASS yok):** aşamalar gerçek solver olaylarıyla eşleşir; profiling total bilinmiyorsa indeterminate; iptal/başarısızlıkta %100 başarı göstermez; mevcut solver çıktısı ve testleri geriye uyumlu.

### B11 — Model belirsizliği, ölçümlü A/B, seçilebilir öneri ve history / P0/P1 / 119–123, 125–127

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** `optimizetab.cpp`, `autotunereport.cpp`: seçili gerçek önerileri yalnızca taslağa alma, mevcut Apply/ABBA akışını koruma, seçilebilir scope ve overlap kontrolü; hatalı/eksik skor ve uncertainty ayrımı.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Selective staging ve invalid score ayrımı korunur; gerçek objective/evidence/constraints/ranking/uncertainty metadata ve alternative-weight tradeoff graph, same-context paired A/B/game session ve metric units, measured/model ayrımı, prediction error; persistent report/run history; selected recommendation ID/scope/dependency revalidation mevcut Apply/ABBA ile.

**Kaynak:** optimizetab.cpp:Keep only selected recommendations checked; autotunereport.cpp; B09–10, Autotune report, mevcut frametime.h

**Orijinal kabul testi (tam PASS yok):** Explore ile eski draft karışmaz; bir öneri seçildiğinde yalnızca o scope değişir, karşılıklı bağımlılıkta apply engeli/uyarı; karşılaştırılmayan A/B, yetersiz ölçüm ve eski schema fixture'ları.

### C01 — Kalıcı cross-boot diagnostik oturumları ve semantik diff / P1 / 128, 130–131, 134, 136, 151

**Durum:** KISMİ — ilişkili değişiklik.

**Tur 5 dayanağı:** diagreport.cpp/.h, diagreportview.cpp/.h: rapor başına 20 snapshot/1 MiB limit, manuel kayıt ve silme, local QLockFile/QSaveFile, raw/fix/binary hariç, locale ve capture boot ayrımı; sıra bağımsız multiset diff.

**Bu tur yapılan:** BootPage seçilebilir iki boot karşılaştırması ve kısa önek çakışma koruması (X07).

**Açık kalan:** Cross-boot diagnostik session modeli, adapter, provenance, notlar, stable semantic diff ve kabul fixture’ları eksik.

**Orijinal kalan kapsam:** Mevcut per-report archive/multiset diff’i session ID + full boot/device/source/quality/event ID ile genişlet; cross-boot timeline adapters (flightrec/kernel/pstore/curve/scene/sleep), idempotent import, clock uncertainty, filtre, raw link, notlar. Stable row/finding keys ve device provenance ile semantic diff, retention ve geçmiş navigation; prefix-only candidate ayrı.

**Kaynak:** diagreportview.cpp:archiveWorkspace; diagreport.cpp snapshot schema; healthevidence.*, evidencereports.*, diagreportview.*, flightrec.*

**Orijinal kabul testi (tam PASS yok):** iki boot ve iki cihazlı fixture, prefix çakışması, duplicate event, eski/bozuk rapor, değişen tablo sırası, stale data; session yeniden açılabilir, notlar doğru olayda kalır, correlation kesin neden olarak etiketlenmez.

### C02 — Evidence Relationship Graph / P1 / 129

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** `diagreportview.cpp/.h`: mevcut raporun doğrudan bulgu/tablo/satır/limit ilişkileri, filtreli ve boyut sınırlı salt okunur ağaç, seçili detaylar; redacted snapshot, yeni tarama yok.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Mevcut bounded report relation tree’yi session/incident/device/source/tool graph’a genişlet; typed edges direct/same-device/temporal/candidate, gerekçe ve provenance, filter/zoom/group/selection deep link; source missing fallback, yeni tarama yok.

**Kaynak:** diagreportview.cpp:Evidence Relationships; C01, diagreport.*, toolcatalog.*

**Orijinal kabul testi (tam PASS yok):** ilişkisiz loglar bağlanmaz; boot prefix çakışması ayrı etiketlenir; node silinmiş/veri eksikse güvenli fallback; graph oluşturmak yeni donanım taraması başlatmaz.

### C03 — Alan düzeyinde rapor seçimi ve anonimleştirme / P0 / 142–143

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** `diagreport.cpp/.h`, `diagreportview.cpp`: bulgu/tablo seçimi, raw opt-in, orijinal verdict korunması, belirli tablo kolonları ve metinde best-effort redaction, düzenlenebilir tek snapshot, QSaveFile. İkili EDID tablosu artık dil-bağımsız privateBinary metadata ile varsayılan export ve history dışında.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Bulgu/tablo ve raw seçim/tek editable snapshot korunur; row/column/evidence/source metadata seçimi, excluded sections, source-aware hostname/path/serial/MAC/IP/UUID redaction, raw conservative + manual override ve uyarı. Preview/copy/save aynı frozen snapshot/hash, refresh race test; privateBinary hariç.

**Kaynak:** diagreportview.cpp:previewExport; diagreport.cpp; diagreportview.*, reportpreview.*

**Orijinal kabul testi (tam PASS yok):** fixture içindeki bilinen özel değerler export'ta bulunmaz, teknik Xid/MCE/PCI adresi gereksiz yere silinmez; preview/copy/save aynı hash; işlem sırasında report refresh önceki export'u değiştirmez.

### C04 — Grafiksel Hardware Device Tree ve PCIe topology / P1 / 145, 153

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** systemdiagnostics.cpp/.h, diagreport.h, diagreportview.cpp: canonical PCI/USB/NVMe/network ilişkileri, filtreli hierarchy, eski PCIe scan alanları kimlik kontrolüyle kullanılır.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Canonical PCI/USB/NVMe/net tree üzerine stable identity/hotplug, lazy expand/selection persistence, inspector deep links ve current/max links/NUMA/AER provenance. BDF reuse/cycle/detached fixture, dGPU wake yok.

**Kaynak:** systemdiagnostics.cpp:deviceTopology; diagnostictools.*, advanceddiagnostics.*, diagsources.*

**Orijinal kabul testi (tam PASS yok):** nested PCIe bridge, detached USB/NVMe, iki aynı vendor/device, link unknown, symlink cycle fixture; mevcut delta identity ve AER unknown testleri korunur.

### C05 — EDID binary inspector ve VRR zamanlama görselleştirmesi / P1 / 152

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** `diagnostictools.cpp`: salt okunur EDID hex/ASCII, byte offset, base DTD active/blank/total/clock/refresh ve CTA block boundary düzeltmesi.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Mevcut hex/ASCII/timing ve CTA bounds korunur; byte seçimi, offset/checksum/extension/descriptor inspector, connector seçimi ve doğrulanmış timing/VRR grafik; genel range VRR kanıtı değildir. İsteğe bağlı raw export privacy warning; override write yok.

**Kaynak:** diagnostictools.cpp; diagsources.cpp; diagnostictools.*, mevcut EDID parser

**Orijinal kabul testi (tam PASS yok):** 1/2/çok bloklu, checksum mismatch, truncated CTA, genel refresh range fakat VRR yok, connector disconnect fixture; byte seçimi doğru alanı gösterir.

### C06 — Memory Architecture View / P1 / 154

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** systemdiagnostics.cpp/.h: CPU package/core ve sibling bilgisi, NUMA memory/distance, unknown RAM/PSI ayrımı.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** CPU package/core/NUMA/distance üzerine cache shared_cpu_list, doğrulanmış CCD/online, güvenilir SMBIOS slot/speed/capacity ve memory architecture graph. Swap/zram ayrı; unknown ve container/partial topology fixture.

**Kaynak:** systemdiagnostics.cpp:cpuTopology; sysinfo.*, diagsources.*, memorydialog.*

**Orijinal kabul testi (tam PASS yok):** tek NUMA, iki node, AMD cache-sharing, Intel hybrid, container/eksik sysfs; toplamlar yalnızca aynı scope ve birimde, unknown değerleri sıfırla doldurma.

### C07 — ACPI / WMI Explorer / P2 / 155

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** systemdiagnostics.cpp, diagreportview.cpp: kernel exposed ACPI/WMI device tree, instance ve GUID, driver ve status; salt okunur.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Exposed ACPI/WMI tree’ye filtreli event/driver/GUID bağları ve okunabiliyorsa bounded DSDT/SSDT table list; full AML namespace iddia etme, method/EC/WMI execute veya escalation yok.

**Kaynak:** systemdiagnostics.cpp:Exposed ACPI / WMI Device Hierarchy; acpievents.cpp; systools.*, systemdiagnostics.*, acpievents.*

**Orijinal kabul testi (tam PASS yok):** eksik ACPI/WMI, permission denied, duplicate GUID, GUID formatı, detached driver; listeleme herhangi bir method execution yapmaz.

### C08 — USB-C PD Visualization / P2 / 156

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** systemdiagnostics.cpp: USB-C port/partner ve supply gerçek ölçüm/kapasite ayrımı, signed voltage/current power; bilinmeyen kontrat varsayılmıyor.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Type-C role/partner/supply ölçüm ayrımı üzerine gerçek PD capability/contract/profile ve partner ilişki görselleştirmesi; role swap/disconnect/negative current fixture; capacity ≠ contract ≠ consumption, read-only.

**Kaynak:** systemdiagnostics.cpp; sysinfo.*, diagsources.*, systemdiagnostics.*

**Orijinal kabul testi (tam PASS yok):** USB-C PD, barrel AC, disconnected port, role swap, eksik `typec` sysfs, negatif current fixture; power role veya charge state değiştiren kontrol yok.

### C09 — Eksik yapılandırılmış tool output adaptörleri / P1 / 146–147

**Durum:** KISMİ — yeni uygulama.

**Tur 5 dayanağı:** `structuredtools.h`, `systools.cpp`: Kernel lsmod ve Battery uevent için sınırlı, strict parser, raw fallback, tablo kopyalama, hassas alan whitelist ve eksik/truncated çıktı uyarısı.

**Bu tur yapılan:** `structuredtools.h`, `systools.cpp`, `structuredtools_test.cpp`: lsusb ve lscpu -e için bounded parser, katı header/duplicate kontrolleri ve raw fallback. Mevcut lsmod/Battery korunur.

**Açık kalan:** Bütün uygun tool ID’leri, tek frozen dialog toggle/model/proxy ve seçilebilir export eksik; Qt testleri çalıştırılamadı.

**Orijinal kalan kapsam:** Kernel lsmod ve Battery uevent adapter’ları korunur; uygun diğer tool ID’leri için typed bounded parsers/helper JSON, locale/version error/raw fallback; aynı frozen snapshot Raw/Structured toggle, model/proxy, sort/selection, selective export.

**Kaynak:** structuredtools.h; systools.cpp:Structured output; systools.*, diagreport.*, toolcatalog.*

**Orijinal kabul testi (tam PASS yok):** örnek çıktıda malformed field, non-English output, boş ve büyük çıktı, unknown version; raw kayıpsız erişilebilir, parser hata verirse GUI donmaz.

### C10 — Ölçüme dayalı cooling/battery/sleep/storage geçmişi ve karşılaştırma / P1 / 163–169, 170–171

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** `sleeppage.cpp/.h`, `sleepadvice.cpp`: mevcut sleep kayıtlarından süre ve HW residency geçmişi, mode filtresi, eksik ölçümlerde boşluk, tekrar kayıtları ayıklama; eksik enerji veya HW ölçümünden sonuç uydurulmaması.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Sleep duration/residency history korunur; full boot/session/end AC ve missing resume, context matching. Cooling power/ambient/temp/fan/duration comparable runs; battery charge/discharge/capacity/Wh/cycles, SMART stable device raw/normalized/threshold/NVMe/SATA history; opt-in bounded sampling, shared polling, no auto stress/self-test/wake.

**Kaynak:** sleeppage.cpp; sleepadvice.cpp; coolingpage.cpp; batterypage.cpp; smartpage.cpp; coolingpage.*, batterypage.*, sleeppage.*, smartpage.*

**Orijinal kabul testi (tam PASS yok):** karşılaştırılamayan cooling koşulu, battery şarj/deşarj geçişi, sleep duplicate/missing resume, SMART device replacement/unknown counters; yanlış eşleştirme, sahte history veya otomatik stress yok.

### C11 — Process-level network traffic charts / P1 / 179

**Durum:** KISMİ — yeni uygulama.

**Tur 5 dayanağı:** `traffic.cpp/.h`, `netpage.cpp`: program bazlı 30 günlük TX/RX/toplam/bağlantı sayısı grafiği ve tablo, saturating qint64 toplam, eksik güne grafik boşluğu.

**Bu tur yapılan:** `traffic.*`, `netpage.*`, `traffic_test.cpp`: tarih filtresi, top-N ve other, qint64 sayaç tablo/sıralama, eksik gün gap, malformed/version/bounds kontrolü.

**Açık kalan:** PID reuse/stable process identity, tam ledger quality provenance ve Qt davranış fixture’ları eksik. Grafik double koordinatı yaklaşık, sayaç tablosu integer.

**Orijinal kalan kapsam:** Mevcut 30 günlük program TX/RX/total/conns graph üzerine custom range/top-N/other, ledger malformed quality, exact qint64 presentation/overflow, missing day; process identity/PID reuse ayrı, instant rate sadece gerçek kaynak varsa.

**Kaynak:** traffic.cpp; netpage.cpp:Daily Traffic Charts; traffic.*, netpage.*

**Orijinal kabul testi (tam PASS yok):** 64-bit büyük byte, eksik gün, tarih filtresi, boş dosya, bozuk JSON, iki farklı program; chart toplamı mevcut tablodaki değerlerle aynı.

### C12 — Wi-Fi channel visualization / P2 / 183

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** `netpage.cpp/.h`: eldeki scan'den 2.4/5/6 GHz frequency marker, BSSID tablosu, received timestamp, adapter değişimi ve geç sonuç guard. Yeni otomatik tarama yok.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Mevcut scan frequency marker/BSSID/adapter guard korunur; gerçek scan timestamp/freshness, width ve RSSI varsa verified band/range overlap graph, unknown width marker, hidden SSID ve multi-adapter stale fixture. Yeni polling yok.

**Kaynak:** netpage.cpp:rescanWifi; netpage.*, mevcut Wi-Fi helper

**Orijinal kabul testi (tam PASS yok):** aynı SSID farklı BSSID, 6 GHz, unknown width, invalid signal, stale scan ve adapter switch; grafik eski adapter verisini yeniye bağlamaz.

### C13 — Firewall rule diff ve örnek eşleşme önizlemesi / P0/P1 / 181–182

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** `netrules.cpp/.h`, `netfirewall.cpp/.h`: helper’ın bildirdiği kayıtlı kuralları pin/diff; ID, sıra, policy ve alan değişiklikleri; eksik/yinelenen kimlik reddi. Açık add/edit dialogu sırasında bildirilen konfigürasyon değişirse istek gönderilmez.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Saved-rule pin/diff ve GUI revision guard korunur; canonical CIDR/family/protocol/port/order/verdict diff, effective vs draft separation, recorded connection impact preview gerçek semantics ile; unsupported chain/sets/stateful Unknown. Backend revision revalidation ve no-write fixture; future access garantisi yok.

**Kaynak:** netrules.cpp; netfirewall.cpp; netfirewall.*, netrules.*, netpage.*

**Orijinal kabul testi (tam PASS yok):** IPv4/v6, port aralığı, CIDR, first-match/order, invalid rule, unknown expression; preview sırasında sıfır firewall write; eski kurallar ve explicit apply davranışı korunur.

### D01 — Visual Scene Builder, tam dependency ve execution/step inspector / P0/P1 / 189–192, 195, 197–198

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** `scenes.cpp/.h`, `scenestab.cpp/.h`: gerçek transaction callback’lerinden running/returned/failed/soft failure/skipped/restore sonuçları; bounded son yürütme geçmişi. Mevcut yürütme sırası ve rollback korunur.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Mevcut SceneEngine preview ve step log korunur; aynı draft’ı kullanan visual builder, gerçek ordered structured plan/old-new/scope/skip/dependency, semantic Choice diff, bütün profile/capability refs, trigger overlap/priority gerçek engine, read-only simulated event. Step result transaction ID + verified/partial/rollback distinction ve external daemon events; A05.

**Kaynak:** scenestab.cpp:Execution Log, eng_->preview; scenes.cpp; scenes.*, scenestab.*, A05

**Orijinal kabul testi (tam PASS yok):** missing profile, conflicting trigger, Custom dependency, Reset/Unchanged, dry-run with command ve bir adımı başarısız scene fixture'ı; preview sıfır hardware/user command exec; kısmi sonuç başarı olarak gösterilmez.

### D02 — Lighting effect timeline/preview ve kayıtlı renk paleti / P2 / 200, 202, 204

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** `lightingtab.cpp`: 32 kayıt sınırı olan HEX boya renk paleti, kayıt/temizleme/renk seçimi, sıralı effect numarası.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Mevcut HEX palette, effect numarası, key canvas/draft preview/undo korunur; ordered sequence editor ve reorder aynı draft, yaklaşık software animation (hidden stop), only supported speed/direction/color modes, capacity/encodedLen validation, no host-side unsupported claims, palette import validation.

**Kaynak:** lightingtab.cpp:refreshPreview; lighting::Effect; kblayout.cpp; lighting.*, lightingtab.*, kblayout.*

**Orijinal kabul testi (tam PASS yok):** Static/multi-color/speed/direction ve unsupported host effect, kapasite sınırı, undo/reorder, kapalı preview; animasyon sırasında HID/helper write yok, palette import invalid ise reddedilir.

### D03 — Dosya düzeyinde Restore Preview, dependency tree, configuration diff / P0/P1 / 210–211, 213–215

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** `backuppage.cpp`: mevcut config_inspect helper ile configuration restore manifest scope ağacı, bildirilen sayılar ve hedefler; user_state ve system_profile restore edilmez. Sistem image file-level preview unsupported açıkça gösterilir.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Mevcut config_inspect scope manifest, config_diff, sidecar chain ve restore guard korunur; helper-supported bounded file manifest/hash/permission/target create/replace/delete semantics (image unsupported kalabilir), path traversal/symlink/decompression bounds. Chain graph stable IDs/cycle/missing/ambiguous/authentication; verification identity/timestamp; preview sonrası archive/target revalidate ve no restore/mount/write tests.

**Kaynak:** backuppage.cpp:previewRestore; bundle.*; configpage.cpp; backuppage.*, bundle.*, configpage.*, mevcut backup helper

**Orijinal kabul testi (tam PASS yok):** full/incremental chain, missing parent/cycle, malicious archive path, eski verification, changed target ve unsupported image fixture'ları; preview hiçbir restore/mount/write başlatmaz; yanlış seçili arşivde işlem engellenir.

### D04 — Yalnızca kalan layout/çeviri/mantık regresyonları / P0/P1 / 30, 28, FIX-01–21

**Durum:** KISMİ — ilişkili değişiklik.

**Tur 5 dayanağı:** Önceki 78 TR katalog girdisi + bu tur 18 yeni Simulation çevirisi ve structured header i18n marker’ları; önceki çeviriler korunur.

**Bu tur yapılan:** Yeni arayüz metinleri için 50 yeni TR katalog girdisi; iki değişen navigasyon kaynak sözleşmesi güncellendi.

**Açık kalan:** FIX-01–21 native davranış doğrulaması, tüm visible string/placeholder/plural ve DPI/shell kapsamı eksik.

**Orijinal kalan kapsam:** FIX-01–21 her biri için native davranış sonucu; unresolved blocking/layout/cached style ve selection/scroll regression; tüm visible strings i18n context/placeholder/plural/title case, iki shell/dil/DPI ve keyboard accessibility. Önceki düzeltmeleri yeniden yazma, warning suppression yok.

**Kaynak:** gui/src; gui/tests; i18n.cpp; bütün GUI; A01/A08

**Orijinal kabul testi (tam PASS yok):** TR/EN ve iki UI modu ekran testlerinde kırpılan label/yanlış active tab yok; mevcut favoriler ve kullanıcı seçimi korunur; tamamlanmamış bir özellik “hazır” olarak gösterilmez.

### D05 — Native build/test, güvenlik denetimi ve experimental branch / P0 / Teslimat

**Durum:** ENGELLİ — doğrulama.

**Tur 5 dayanağı:** Debug+Release CMake yapılandırması yeniden denendi; Qt6Config.cmake yok. Paket sunucusu DNS çözülemiyor; apt paketleri bulamadı, Cargo yok.

**Bu tur yapılan:** 28 test runner PASS; Qt bağımsız telemetry ve integer conversion 3 yapılandırmada PASS. Debug/Release yapılandırma denendi; Qt6Config.cmake yok. Cargo yok, apt DNS/indeks sorunu.

**Açık kalan:** Qt Debug/Release build, CTest, GUI ASan/UBSan, Rust locked build/test/Clippy/deny, CI, hardware fixture, reviewed PNG, experimental HEAD/commit/push tamamlanmadı. CTest sıfır test buldu; exit 0 PASS değildir.

**Orijinal kalan kapsam:** Gerçek Qt6 Debug+Release, CTest, ASan/UBSan, Rust locked build/test/Clippy/cargo-deny, mevcut Python runners ve CI; güvenlik/readback/restore regressions, test artifacts. Source ZIP temizliği, GitHub HEAD/branch doğrulama ve ancak kabul testleri geçtiğinde experimental’a normal commit/push; main/old untouched.

**Kaynak:** gui/CMakeLists.txt; Cargo.lock; .github/workflows/ci.yml; Cargo workspace, gui/CMakeLists.txt, .github/workflows/ci.yml

**Orijinal kabul testi (tam PASS yok):** Debug/Release, CTest, Rust locked/Clippy/cargo-deny, CI ve güvenli fixture; doğrulanmadan push yok.

### X01

**Durum:** KISMİ — yeni uygulama.

**Tur 5 dayanağı:** Tur 5 planındaki ilgili mevcut parçalar.

**Bu tur yapılan:** `mainwindow.*`, `hometab.*`: QSplitter rail genişliği/kapatma, stable-ID manuel sayfa ve Health favorileri, iki shell için startup seçimi ve eski tercihin okunması.

**Açık kalan:** Tam restart, DPI, unsupported hedef ve klavye davranış fixture’ı yok. Tüm alt araçları kapsayan global favori indeksi henüz yok.

**Orijinal kalan kapsam:** Navigasyon: resize rail, manuel global favoriler, açık startup page selector.

**Kaynak:** mainwindow.cpp, hometab.cpp, toolcatalog.cpp

**Orijinal kabul testi (tam PASS yok):** Restart, unsupported page fallback, empty favorites, TR label ve collapse restore.

### X02

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** Tur 5 planındaki ilgili mevcut parçalar.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Ortak sürümlü workspace state store (per shell/view) ve dirty interface A05 ile; selection/filter/sort/splitter/column/scroll persistence.

**Kaynak:** mainwindow.cpp, netpage.cpp, cosearchdialog.cpp, bootpage.cpp

**Orijinal kabul testi (tam PASS yok):** Bozuk/eski settings, hidden/vanished selection, dirty cancel ve restart.

### X03

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** Tur 5 planındaki ilgili mevcut parçalar.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Kullanıcı tipografi tercihleri, mono numeric font, preview ve reset; A01 density ile ortak.

**Kaynak:** theme.cpp, hometab.cpp, main.cpp

**Orijinal kabul testi (tam PASS yok):** Eksik font fallback, büyük/küçük font ve DPI.

### X04

**Durum:** KISMİ — yeni uygulama.

**Tur 5 dayanağı:** Tur 5 planındaki ilgili mevcut parçalar.

**Bu tur yapılan:** `fwattrtab.*`: tekil/toplu old→new onay, native sayı/transport gösterimi; iptalden önce write yok; sysfs tekrar okuma, eski WMI ve eksik satır guard, başarısız sysfs sonrasında WMI fazı gönderilmez.

**Açık kalan:** WMI fresh preflight/identity, timestamp/quality, tüm lock reason enum, fan factory overlay ve mock hardware testleri eksik. Onaydan önceki WMI değeri last read olarak etiketlenir.

**Orijinal kalan kapsam:** Firmware refresh/provenance, write öncesi tekil/toplu old-new/transport/units preview, reason enum ve partial result; fan factory overlay yalnız gerçek default.

**Kaynak:** fwattrtab.cpp, platformprofile.cpp, fancurvedialog.cpp

**Orijinal kabul testi (tam PASS yok):** Cancel sıfır write, WMI failure, unsupported disabled, historical ≠ stock, safety floor.

### X05

**Durum:** KISMİ — yeni uygulama.

**Tur 5 dayanağı:** Tur 5 planındaki ilgili mevcut parçalar.

**Bu tur yapılan:** `integerunits.h`, `int64spinbox.*`, `optimizetab.cpp`: VM dirty bytes ve dirty background bytes için exact integer B/KiB/MiB/GiB/TiB ayrıştırma, overflow/rounding reddi, opt-in editör. Native dönüşüm testleri PASS.

**Açık kalan:** Boot service inspector, ns/us/ms ve diğer uygun alanlar eksik; Qt editör testi çalıştırılamadı.

**Orijinal kalan kapsam:** Boot service inspector (OpenRC/systemd real enabled/active/result/last run/boot ID), unit-aware canonical integer editors overflow/rounding safe.

**Kaynak:** bootadvisor.cpp, optimizetab.cpp, int64spinbox.cpp

**Orijinal kabul testi (tam PASS yok):** Enabled ≠ applied; no service exec, INT64 unit roundtrip.

### X06

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** Tur 5 planındaki ilgili mevcut parçalar.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Report adaptive section state, capability indicators real executable/kernel/device/permission, incremental model/view stable keys.

**Kaynak:** diagreportview.cpp, systools.cpp, toolcatalog.cpp, netpage.cpp

**Orijinal kabul testi (tam PASS yok):** 10k row changed-only update, refresh selection, missing binary/permission distinguish.

### X07

**Durum:** KISMİ — yeni uygulama.

**Tur 5 dayanağı:** Tur 5 planındaki ilgili mevcut parçalar.

**Bu tur yapılan:** `bootpage.*`: görünür iki boot seçimi, full ID, duplicate/prefix guard, mevcut önceki-boot karşılaştırmasının ortak fonksiyonla kullanılması; eksik kayıt/temiz bitiş ayrımı.

**Açık kalan:** Kernel typed combined filters, guard/preset kaynakları ve Qt fixture testleri eksik; eski klog yalnız kısa önek içerdiği için tam kimlik garantisi yok.

**Orijinal kalan kapsam:** Kernel typed combined filters (context independent), seçilebilir iki boot semantic comparison, full boot IDs.

**Kaynak:** klogpage.cpp, bootpage.cpp

**Orijinal kabul testi (tam PASS yok):** Existing comparedWithPrevious reuse; missing events ≠ 0, prefix ambiguity, context target retained.

### X08

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** Tur 5 planındaki ilgili mevcut parçalar.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Network workspace deep links connection→rule→log, stable process grouping and listening bind scope/exposure evidence.

**Kaynak:** netpage.cpp, traffic.cpp, netrules.cpp

**Orijinal kabul testi (tam PASS yok):** PID reuse, loopback vs wildcard, unknown PID, no firewall write on navigation.

### X09

**Durum:** ÖNCEKİ KISMİ — yeni uygulama yok.

**Tur 5 dayanağı:** Tur 5 planındaki ilgili mevcut parçalar.

**Bu tur yapılan:** Bu tur bu paketin kalan işlevlerine özel kod eklenmedi.

**Açık kalan:** Orijinal kalan kapsam ve kabul testi açık.

**Orijinal kalan kapsam:** Adaptive keyboard canvas zoom/pan/aspect and hit-test, zone4 parity; D02 existing preview/undo preserved.

**Kaynak:** lightingtab.cpp, kblayout.cpp, zone4tab.cpp

**Orijinal kabul testi (tam PASS yok):** DPI/resize selection same key, no clipping or hardware write.

## 4. 118 GUI ID — tam izlenebilirlik matrisi

Hiçbir satırda bütün native kabul kriterleri PASS olarak kaydedilmemiştir. ID seviyesindeki K/T kaynak dayanakları planın özgün sınıflandırmasıdır. `İlişkili kısmi` etiketi yalnızca açıkça belirtilen alt davranışı ifade eder. B10 gibi matriste ayrı satırı bulunmayan paketler §3’te izlenir.

| GUI ID / özellik | Paket | Tur 5 dayanağı | Tur 6 gerçek durum | Orijinal kalan alt iş | Kaynak | Orijinal kabul testi |
|---|---|---|---|---|---|---|
| GUI-01 — Global Command Palette | A09 | **K** (eski D) — 279 optimization key ve fuzzy index var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Firmware/CPU/GPU/fan ve diğer açılmamış alt tool/setting metadata’sını indeksle; stable focus ve lazy/helper sayaçları. | mainwindow.cpp, uinavigator.cpp, toolcatalog.cpp | Açılmamış firmware/optimization ayarı bulunmalı; arama sırasında widget/helper oluşmamalı. |
| GUI-02 — Intelligent Search | A09 | **K** (eski D) — Fuzzy ranking ve Türkçe normalizasyon mevcut. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Kalan TR/EN alias/synonym sözlüğü, exact/prefix/token ordering ve duplicate/bounded search fixture; mevcut edit distance tekrar yazılmaz. | uinavigator.cpp | Türkçe/İngilizce alias ve tek karakter hatası doğru sonuç vermeli; exact match önce gelmeli. |
| GUI-03 — Adaptive Workspace | A01/A08/D04 | **K** (eski D) — Home reflow mevcut; gerçek diğer sayfa DPI doğrulaması yok. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Yoğun sayfaların responsive min-size/form/splitter düzeni ve iki shell; A01/A08. | mainwindow.cpp, hometab.cpp, experimentalui.h | Dar pencere ve %200 DPI'da ana eylemler erişilebilir; gereksiz yatay kaydırma yok. |
| GUI-04 — Resizable Navigation | X01 | **K** (eski D) — mainwindow.cpp:624 rail setFixedWidth kullanıyor. | İLİŞKİLİ KISMİ — Rail QSplitter ve ayarlı genişlik/collapse; Qt restart/DPI açık. | Kullanıcı ayarlı genişlik, min/max ve persist/collapse restore. | mainwindow.cpp | Sürükle, kapat/aç ve restart sonrası genişlik korunmalı; uzun Türkçe başlık kırpılmamalı. |
| GUI-05 — Navigation Favorites | X01 | **T** (eski A) — Health favorileri mevcut; global rail favorisi bu tur raporlarında gösterilmiyor. | İLİŞKİLİ KISMİ — Manuel stable-ID sayfa ve Health favorileri; diğer alt araçlar ve native test açık. | Global manuel stable-ID favorilerini kaynakta kontrol edip eksikse ekle; boş varsayılan, silinen hedef temizliği. | toolcatalog.cpp, healthtab.cpp, mainwindow.cpp | Yeni config boş; yalnızca kullanıcı seçtiği araç eklenmeli ve doğru araca açılmalı. |
| GUI-06 — Workspace Memory | X02 | **K** (eski D) — Bazı QSettings kayıtları var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Ortak sürümlü workspace store, scroll/filter/splitter/selection ve shell izolasyonu; GUI-231 ile tek altyapı. | mainwindow.cpp, cosearchdialog.cpp, netpage.cpp | Restart ve lazy sayfa oluşturma sonrası doğru durum dönmeli; eski/bozuk kayıt güvenle atlanmalı. |
| GUI-11 — Global Operation Center | A05 | **K** (eski D) — 200 satır polling operation history; outcome unknown. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Gerçek operation registry, owner/source/phase/progress/result/cancel/restoration ve conflict lock. | mainwindow.cpp, cpustressbox.cpp, coolingpage.cpp | Başarısız/cancel/restoring ayrı; terminal duruma gelmeden kaynak serbest kalmamalı. |
| GUI-13 — Unsaved Changes Indicator | A05/X02 | **K** (eski D) — Scenes/Lighting dirty ve ayrı Optimization draft mevcut. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Ortak dirty interface, page marker ve quit/restart loss summary, applied ile draft ayrımı. | scenestab.cpp, optimizetab.cpp, lightingtab.cpp | Taslak değişince yalnız ilgili sayfa dirty; revert/save/apply doğru geçiş; cancel taslağı korur. |
| GUI-14 — Custom Startup Workspace | X01 | **K** (eski D) — hometab.cpp:Restore last page seçeneği. | İLİŞKİLİ KISMİ — Home/last/explicit per-shell tercihi, fallback; Qt restart testi açık. | Home/last/explicit stable page selector, Classic ve unsupported fallback. | mainwindow.cpp, hometab.cpp | Seçilen sayfaya restart sonrası gitmeli; silinen/unsupported sayfa Home'a düşmeli. |
| GUI-21 — OLED Appearance Mode | A01/A02 | **K** (eski D) — OLED Black mevcut. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Paletten bağımsız OLED yüzey/parlaklık tercihleri ve kontrast; burn-in garantisi yok. | theme.cpp, hometab.cpp | OLED görünümünde metin/selection contrast korunmalı; diğer 56 tema değişmemeli. |
| GUI-22 — Typography Controls | X03 | **T** (eski A) — Font-aware ölçüler mevcut; bağımsız font tercihinin durumu teyit edilecek. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Sistem/özel font family, point size, monospace ve fallback/preview/reset. | theme.cpp, hometab.cpp, main.cpp | Büyük/küçük fontlarda erişilebilir kontrol ve tablo hücresi; font bulunmazsa fallback. |
| GUI-23 — Compact / Standard Density | A01 | **K** (eski D) — Compact density başlangıçta uygulanıyor. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Sabit ölçüleri font-aware tokenlara geçir, Standard/Compact tüm kontroller. | theme.cpp, experimentalui.h | Compact/Standard tüm sayfalarda tutarlı; ikon/metin kesilmemeli. |
| GUI-25 — Contrast Validation | A01/A08 | **K** (eski D) — 3990 token kontrolü, 737 eşik altı, 57 istisna. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Rendered state matrix ve düşük kontrast düzeltmeleri/istisna gerekçeleri; custom ve grafik testleri. | theme.cpp, gui/tests | Bütün palet × durum çiftleri test edilmeli; düşük contrast açıkça raporlanmalı. |
| GUI-26 — Semantic Color System | A01 | **T** (eski A) — Tema renk tokenları ve bazı durum etiketleri mevcut; tam semantik standardizasyon belgelenmemiş. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Status enum → palette token + ikon + erişilebilir metin; yerel hardcoded renkleri taşı. | theme.cpp, experimentalui.h | Aynı durum Home, Health ve tuning'de aynı anlama gelsin; Unknown başarı rengi olmasın. |
| GUI-28 — High-DPI Layout Audit | A08/D04 | **T** (eski D) — Bazı sabit ölçüler font-aware yapıldı; gerçek DPI matrisi çalıştırılmadı (S1,S2,F). | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Tüm GUI'de min-size, scroll, table header, tooltip, popup ve chart label denetimi; DPR varyantları. | mainwindow.cpp, charts.cpp, ryzentab.cpp, fwattrtab.cpp | %100/125/150/175/200, iki dil ve iki shell; metin ve ana buton kırpılmamalı. |
| GUI-29 — Colorblind-Friendly Mode | A01 | **T** (eski D) — CCD tile'larında metin + renk; ortak renk körlüğü modu yok (C). | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Renkten bağımsız ikon/desen/metin; seçilebilir güvenli palet ve grafikte çizgi stili. | cosearchdialog.cpp, theme.cpp | Grayscale simülasyonda pass/fail/unknown ayrılmalı. |
| GUI-30 — Translation & Terminology Audit | D04 | **T** (eski D) — Her aşamada yeni Türkçe mesajlar eklendi; bütün kaynak kapsamı denetlenmedi (S1–F). | İLİŞKİLİ KISMİ — 50 yeni TR girdi; tüm GUI dil denetimi açık. | Visible string envanteri, context/placeholder/plural ve title-case; teknik log/anahtarları koru. | i18n.cpp, gui/src, gui/tests | Türkçe/İngilizce placeholder ve çeviri kapsamı testleri; kullanıcı arayüzünde istenmeyen İngilizce kalmamalı. |
| GUI-31 — Configurable Dashboard | A10 | **K** (eski D) — Experimental panel order/reset ve iki panel visibility var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Classic bağımsız tercih, eksik güvenli panel görünürlüğü, reset/restore ve gizli polling. | hometab.cpp, experimentalui.h | Sıra ve panel tercihi restart sonrası korunmalı, min-size ihlali olmamalı. |
| GUI-39 — Compact Device Controls | A01/A10 | **K** (eski A) — Home bazı kompakt satırlar var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Ortak capability-aware label/value/action satırı; fan/MUX/panel/battery/core parking. | hometab.cpp, experimentalui.h | Unsupported kontrol yer tutmamalı, uzun etiket ve birim taşmamalı. |
| GUI-40 — Adaptive Dashboard Grid | A10 | **K** (eski D) — Experimental reflow hysteresis ve order mevcut. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Classic ve tüm visibility kombinasyonları/dar geniş layout testleri. | hometab.cpp | Panel gizlenince boş grid hücresi bırakmamalı; resize sırasında flicker olmamalı. |
| GUI-43 — CCD Topology View | B01 | **K** (eski D) — Arşiv CCD/slot/logical CPU gösteriyor. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Live topology SMT/offline/CPU fingerprint doğrulaması, X3D kanıtı ve heatmap bağlantısı. | cosearchdialog.cpp, sysinfo.cpp | CPU ID boşlukları, SMT kapalı, offline core ve bilinmeyen CCD fixture'ları. |
| GUI-44 — Core Stability Heatmap | B01 | **K** (eski D) — Arşiv arama/son fail/verify/reset durumları var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Journal kanıtı ile live heatmap ve contradictory state; exclusion ≠ pass. | cosearchdialog.cpp, coevidence.cpp | Test edilmemiş core yeşil olmasın; reboot exclusion geçer sonucu sayılmasın. |
| GUI-45 — Per-Core Candidate History | B01 | **K** (eski D) — 40 run/1 MiB archive ve history var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Stable event/session IDs, core/candidate filters, multi-session diff, archive export/delete ve fingerprint. | cosearchdialog.cpp, coevidence.cpp, coinsights.cpp | Eski oturumlar yeni arama başlatınca silinmemeli; eksik journal kayıtları Unknown. |
| GUI-46 — Reboot Recovery Inspector | B01/B02 | **K** (eski D) — Const preview ve kesilen aday sonrası aday var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Typed guard reason, last trusted value, exclusions ve preview invalidation. | cosearchdialog.cpp, costrategy.cpp | Preview gerçek state'i mutate etmemeli; tekrar reboot senaryosunda dışlanan aday yeniden önerilmemeli. |
| GUI-51 — CO Profile Diff | B01 | **T** (eski D) — Ortak read-only JSON diff (C). | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Core identity ile semantik eşleme, all-core vs per-core açık ayrım, doğrulama bilgisi ve donanım uyuşmazlığı. | ryzentab.cpp, profilediff.h | Dizi sırası değişince yanlış core diff oluşmamalı; eksik değer sıfır olmamalı. |
| GUI-59 — Resume Safety Preview | B02 | **K** (eski D) — Search copy preview mevcut. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Revision invalidation ve gerçek resume replay/guard reason; crash fixture eşitliği. | cosearchdialog.cpp, costrategy.cpp | Preview/resume aynı state'te aynı adayı seçmeli; journal ve guard yazımı yok. |
| GUI-61 — Multi-Curve Overlay | B03 | **T** (eski D) — Base reference, historical driver readback ve draft katmanları toggle edilebilir (S2). | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Kayıtlı profil katmanı ekle; voltage/point ID eşlemesi, bağımsız legend ve karşılaştırılabilirlik kontrolü. | nvidiatab.cpp, vfcurvewidget.cpp | Farklı GPU/point topology eşleşmediğinde çizme veya uyar; reconstructed base factory stock denmemeli. |
| GUI-62 — Curve Delta View | B03 | **T** (eski A) — Point Comparison tablosunda draft offset var (S2); ayrı delta chart belgelenmemiş. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Aynı voltage ID'leri için base/readback/profile farkını ayrı alt grafik yap; birim MHz. | vfcurvewidget.cpp, nvidiatab.cpp | Eşleşmeyen nokta boş; negatif ve pozitif fark doğru; sıfır yalnız gerçek eşitlikte. |
| GUI-63 — Curve Point Inspector | B03 | **T** (eski D) — Point Comparison tablosunda voltaj, referans, draft, cached frequency ve timestamp (S2). | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Grafik seçimiyle bağlı nokta inspector; çoklu seçim özeti ve validation status. | vfcurvewidget.cpp, nvidiatab.cpp | Zoom/selection değişince inspector aynı point ID'yi izlemeli; cached data canlı gösterilmemeli. |
| GUI-66 — Curve Editing History | B03 | **T** (eski D) — 32-entry draft undo/redo; helper sınırında temizleniyor (C). | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Kalıcı operation journal: edit action, timestamp, before/after, transaction sonucu; undo geçmişinden bağımsız. | nvidiatab.cpp, vfcurvewidget.cpp | Undo GPU'ya yazmamalı; journal başarısız işlemleri de göstermeli; veri sınırı olmalı. |
| GUI-68 — Hardware Readback Comparison | B03 | **K** (eski D) — Request ID ve model+point layout, reset readback var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Gerçek UUID/fingerprint, full transaction scope, backend quantization, adjusted/partial/missing status ve kalıcı history. | nvidiatab.cpp, mevcut nvcurve helper | Eski readback yeni transaction'ı doğrulamasın; farklı GPU kimliği kabul edilmesin. |
| GUI-71 — Curve Profile Metadata | B03 | **K** (eski A) — JSON profile diff var; saveProfileAs gerçek point IDs ile yazıyor; tam GPU metadata inspector belgelenmedi. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | GPU UUID/model, VBIOS/driver, curve topology, created/verified tarihleri; eski profil için unknown. | nvidiatab.cpp, profilediff.h | Donanım/driver uyuşmazlığı belirgin; metadata eksikliği güvenilirlik garantisi sayılmasın. |
| GUI-74 — Trial Session Monitor | A05/B03 | **K** (eski D) — Operation Center active polling var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Gerçek trial deadline/restore/verification events, global registry ve sonuç geçmişi. | mainwindow.cpp, trialbar.cpp, nvidiatab.cpp | Trial sona erince restore callback bitene kadar busy; force close garantisi verme. |
| GUI-76 — Xid Event Integration | C01/B03 | **K** (eski D) — Timeline chronological merge var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Full boot/GPU UUID/request ID ilişkisi ve editörden deep-link; korelasyon nedensellik değil. | evidencereports.cpp, nvidiatab.cpp, flightrec.cpp | Yanlış boot/cihaz eşleşmez; yakın zaman ilişkisi nedensellik olarak sunulmaz. |
| GUI-79 — Firmware Parameter Inspector | A03/X04 | **K** (eski D) — Firmware last-known/draft inspector mevcut. | İLİŞKİLİ KISMİ — WMI per-row known ve sysfs readback/guard; tam timestamp/freshness açık. | Explicit refresh, source timestamp, freshness/readback quality ve WMI unavailable. | fwattrtab.cpp | WMI readback yoksa eski değer live/verified değil. |
| GUI-80 — Firmware Change Summary | A05/X04 | **T** (eski A) — Mevcut Apply All değişen satırları seçiyor (kaynak); inspector draft ve snapshot ayırıyor (S2). | İLİŞKİLİ KISMİ — Tekil/toplu old/new onay, direct sysfs readback; WMI fresh ve fixture açık. | Tekil/toplu firmware yazma öncesi old/new/units/device/transport ve uyarıların toplu önizlemesi. | fwattrtab.cpp | Cancel hiçbir write yapmamalı; partial WMI/sysfs sonucu ayrı raporlanmalı. |
| GUI-81 — Locked Control Explanation | X04 | **T** (eski D) — Custom olmayan profilde kilit açıklaması ve WMI/acpi_call erişim tooltip'i var (kaynak). | İLİŞKİLİ KISMİ — WMI bilinen değer ve busy guard; ortak reason enum açık. | Bütün disable nedenlerini ortak reason enum ve açık metinle göster; permission/device/busy/unsupported ayrımı. | fwattrtab.cpp, platformprofile.cpp | Custom değişiminde lock güncellenmeli; read-only parametre sebepsiz etkinleşmemeli. |
| GUI-91 — Fan Curve Overlay | A06/X04 | **T** (eski D) — Historical EC table overlay ve draft temperature/RPM tablosu (S2). | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Doğrulanabilen factory/default katmanı, timestamp, ayrı legend ve per-point karşılaştırma; yoksa unknown. | fancurvedialog.cpp | EC snapshot stock diye etiketlenmemeli; safety floor ve write yolu değişmemeli. |
| GUI-94 — Optimization State Matrix | B07 | **K** (eski D) — State matrix ve source workspace mevcut. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Restore-original yalnız backend varsa; true verification timestamp ve stale mismatch. | optimizetab.cpp, tuningviewstate.h | Snapshot factory default sayılmaz; unreadable/stale boot match olmaz. |
| GUI-95 — Setting Source Indicator | B07 | **K** (eski A) — sourceWorkspace kanıtlı source gösteriyor. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Per-device/source provenance, real drift attribution ve unknown external writer, fixture. | optimizetab.cpp, driftwatch.cpp | TLP benzeri servis açık diye değer kaynağı otomatik atanmaz. |
| GUI-96 — Smart Selection Filters | B07 | **K** (eski D) — Filter ve conflict metadata var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Birleşik conflict/covered/verified filtre, per-section persist ve hidden selected count. | optimizetab.cpp | Filtre include checkbox/draft değiştirmemeli; hidden selected sayısı açık olmalı. |
| GUI-99 — Setting Detail Inspector | B07 | **K** (eski A) — Source/conflict inspector var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Tek setting detail pane: key/path/unit/range/effect/risk/dependency ve gerçek current/original values. | optimizetab.cpp | Tooltip'teki teknik bilgi eksiksiz taşınmalı; unknown range uydurulmamalı. |
| GUI-100 — Optimization Profile Diff | B07 | **K** (eski D) — JSON preset diff mevcut. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Semantik live/boot diff ve coverage/missing/stable device normalization. | optimizetab.cpp, profilediff.h | Ayar key sırası fark yaratmamalı; unsupported ve no-op ayrı. |
| GUI-101 — Boot Service Inspector | X05 | **T** (eski A) — Boot label, saved preset/guard/live mismatch ve OpenRC yardım metni var (S1, kaynak). | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | OpenRC/systemd gerçek enabled/active/result, service last run, boot ID ve guard verisi için salt okunur inspector. | optimizetab.cpp, bootadvisor.cpp | Enabled ≠ başarılı apply; service absent/permission denied ayrı; dış servis çalıştırma yok. |
| GUI-102 — Persistent Edit Drafts | B08 | **K** (eski D) — Manuel VM/scheduler draft save/restore/delete var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Opt-in restart restore, diğer scope/per-device identity, migration, mismatch/discard; startup no writes. | optimizetab.cpp | Restart hiçbir ayarı otomatik uygulamamalı; incompatible draft açıkça işaretlenmeli. |
| GUI-103 — Conflict Relationship View | B07 | **K** (eski A) — Policy metadata ve conflict/coverage inspector var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Relationship graph/detail, dependency cycles ve operation scope/unknown reason. | optimizetab.cpp, driftwatch.cpp | Conflicting rows gizlenmemeli; graph cycle güvenli; existing guard korunmalı. |
| GUI-104 — Per-Device Tuning Inspector | B07 | **K** (eski A) — Source workspace scope var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Stable device identity/hotplug/inspector/filter; stale target write guard. | optimizetab.cpp | Yok olan aygıt üzerinde write seçimi kalmamalı; cihaz ID değişimini tespit et. |
| GUI-105 — Apply Scope Selector | B07 | **K** (eski D) — Live/boot ayrı ve preview var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Scope/preset merge/remove etkilerini onay öncesi gerçek helper semantiği ile göster. | optimizetab.cpp | Live-only boot'u değiştirmez; boot-only live write yapmaz (mevcut helper semantiğine göre doğrula). |
| GUI-106 — Apply Result Report | B07 | **K** (eski D) — Komut sonuç ve source/readback görünümü var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Per-key structured helper result + transaction ID, actual/verify/partial/skipped/restore states. | optimizetab.cpp, tuning helper | Timeout/partial/rejected success gibi görünmesin; failed restore ayrımı. |
| GUI-107 — Preset Coverage View | B07 | **K** (eski D) — Coverage ilişkileri var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Preset coverage included/not specified/unsupported/shadowed ve merge/replace/hidden selection. | optimizetab.cpp | Eksik key default olarak uygulanmaz; hidden checked kapsamı gösterilir. |
| GUI-109 — Unit-Aware Editors | X05 | **T** (eski A) — 64-bit değer sunumu testi eklendi (S1); birim dönüşümlü editor tamamlandığı belgelenmedi. | İLİŞKİLİ KISMİ — Exact integer byte parser PASS; ns/us/ms ve Qt davranışı açık. | Canonical integer değeri koruyarak KiB/MiB/GiB, ns/us/ms uygun dönüşümler; overflow/rounding validation. | int64spinbox.cpp, optimizetab.cpp | INT64 sınırı ve unit roundtrip; dönüşüm yüzünden ayar sessiz değişmemeli. |
| GUI-111 — Table Column Customization | B07 | **T** (eski D) — State matrix grid sütunları mevcut (S1). | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | İsteğe bağlı sütun görünürlüğü/sıra/genişlik, stable ID ve reset; kritik status/selection erişilebilir kalmalı. | optimizetab.cpp | Restart layout korunmalı; filtre/selection verisini değiştirmemeli. |
| GUI-112 — Autotune Workspace | B11 | **K** (eski A) — Üç tablı report, timeline ve selective staging var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Run ID ile settings/execution/report/history workspace bağları; Explore ayrımı. | optimizetab.cpp, autotunereport.cpp | Explore ve Apply ayrımı korunmalı; running işlem sırasında yeni sonuç yanlış oturuma bağlanmamalı. |
| GUI-113 — Weight Studio | B09 | **K** (eski D) — Mevcut sayı editörü/default weights ve bar. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Radar/preset overlay ve legal weight range normalization açıklaması; mevcut sayı editörünü tekrar yazma. | optimizetab.cpp, autotunereport.cpp | Radar değerleri solver'ın gerçek weights'ine eşit; default/restore korunmalı. |
| GUI-114 — Candidate Ranking Explorer | B11 | **K** (eski A) — Mevcut candidate score ve invalid score ayrımı var; full ranking/constraints explorer açık. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Aday listesini sıralama/filter, seçilen ile alternatifler, constraint nedeniyle elenenler; yalnız model verisi varsa. | optimizetab.cpp, autotunereport.cpp | Skor karşılaştırmaları aynı goal/context içinde; eksik skor uydurulmaz. |
| GUI-115 — Autotune Explainability | B11 | **K** (eski A) — Rationale/trace ve unknown score ayrımı mevcut; structured objective/evidence breakdown açık. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Yapılandırılmış decision breakdown: objective contributions, evidence, constraints, fallback; backend sağlamıyorsa açıklanmıyor. | optimizetab.cpp, autotunereport.cpp | Gösterilen açıklama aynı run/setting/adaydan; skor yeniden tahmin edilmesin. |
| GUI-117 — Calibration Data Manager | B09 | **K** (eski D) — v10 metadata + coverage count tablosu var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Validated import/export, supported passive session management, atomic backup ve active calibration guard. | optimizetab.cpp, signature v10 inspector | GUI doğrudan signature değiştirmesin; bozuk/gelecek schema read-only hata; model corpus korunmalı. |
| GUI-119 — Confidence Visualization | B11 | **K** (eski A) — Eksik skor/uncertainty ayrımı var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Gerçek model interval/confidence varsa visualization ve evidence sufficiency; no fabricated confidence. | autotunereport.cpp, optimizetab.cpp | Model interval yoksa sahte yüzde üretilmez; measurement/model ayrımı korunur. |
| GUI-120 — Trade-off Explorer | B11 | **K** (eski D) — Explore alternative weight yeniden hesaplama var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Same-context alternate candidates/weights interactive graph, draft preview. | optimizetab.cpp, autotunereport.cpp | Alternatif seçimi donanıma yazmaz; yeniden hesaplama ve mevcut koşul belirgin. |
| GUI-121 — Before / After Benchmark | B11 | **K** (eski A) — A/B records viewer var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Paired measured context/unit/repeat/quality karşılaştırması ve mismatch warning. | optimizetab.cpp, autotunereport.cpp | Farklı workload/kernel/power context eşleştirme uyarısı; n/a fark sıfır değil. |
| GUI-122 — Calibration Validity Inspector | B09 | **K** (eski D) — Coverage kernel/context/power grupları var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Gerçek model requirements, fingerprint compatibility/reason, freshness, BIOS/hardware comparison. | optimizetab.cpp, signature v10 inspector | Sadece tarih farkı invalid dememeli; desteklenmeyen imza alanı unknown. |
| GUI-123 — Game A/B Session Explorer | B11 | **K** (eski D) — Game session records ve frametime aracı mevcut. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Run/game/context-key paired selection, measured coverage/diff graph; birimler aynı. | optimizetab.cpp, frametime.h | Farklı oyun/çözünürlük/ayarlar eşleşmez; P99 hesabı ve units aynı tanımdan. |
| GUI-125 — Selective Recommendation Apply | B11 | **K** (eski A) — Keep only selected recommendations checked var; donanıma değil draft’a. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Stable recommendation ID, dependencies/conflicts ve stale result revalidation, mevcut Apply/ABBA sonucu ile scope doğrulama. | optimizetab.cpp | Unselected uygulanmaz; stale result yeniden doğrulanır; rollback helper korunur. |
| GUI-127 — Estimated vs Measured | B11 | **K** (eski D) — Invalid/unknown prediction ve observed ayrımı var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Same run/context paired estimate/measure, model error/uncertainty, missing ≠ 0. | autotunereport.cpp, optimizetab.cpp | Ölçülmemiş tahmin başarı olarak sunulmaz; karşılaştırılamayan metrik boş. |
| GUI-128 — Unified Incident Timeline | C01 | **K** (eski D) — Per-report persistent snapshots var; cross-boot session timeline yok raporda. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Stable event/session/full boot/device IDs, adapters, clock uncertainty, dedup ve timeline filters. | evidencereports.cpp, healthevidence.cpp, flightrec.cpp | Farklı bootlar karışmaz; saat geri alınırsa chronology uncertainty gösterilir. |
| GUI-131 — Evidence Comparison | C01 | **K** (eski D) — Persistent snapshots ve unordered multiset diff var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Stable finding/table row/device keys, semantic changes, severity/provenance ve unknown. | diagreportview.cpp, diagreport.cpp | Satır sırası fark sayılmaz; değişen severity ve unavailable ayrı; snapshot restart sonrası korunur. |
| GUI-132 — Diagnostic Confidence Levels | C01/C02 | **K** (eski A) — Severity/unknown ve relationship tree mevcut. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Severity’den ayrı evidence quality/confidence typed states, heuristic/candidate/confirmed ayrımı. | diagreport.cpp, diagreportview.cpp, diagnostictools.cpp | Eksik veri normal sayılmaz; correlation confirmed cause olarak görünmez. |
| GUI-133 — Findings Detail Inspector | C02 | **K** (eski A) — Evidence Relationships tree selected detail var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Finding-specific raw/source/limitation/tool deep link, gerçek snapshot references. | diagreportview.cpp, diagreport.cpp | Inspector filtrelenmiş rapordan değil gerçek snapshot'tan beslenmeli; eksik source açık. |
| GUI-134 — Incident Correlation | C01 | **K** (eski D) — Selected incident merge ve report relationship tree var; cross-session typed correlations açık. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Çoklu olay ilişkisi, source/device/boot/time toleransı ve bağ güveni; cross-boot veri sınırları. | evidencereports.cpp, healthevidence.cpp | Benzer timestamp nedensellik değildir; prefix-only eşleşme ayrı. |
| GUI-136 — Diagnostic Notes | C01 | **K** (eski D) — Incident Notebook var, archive not modeli tamamlanmadı. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Persistent session/event-key note drafts/explicit save/discard; 2000 limit korunur. | healthevidence.cpp | Filtre ve restart not kaybetmemeli; cancel/discard doğru; eski backend sınırı korunmalı. |
| GUI-137 — Advanced Report Filters | C01 | **K** (eski A) — Text/severity filter mevcut. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Source/device/time/review combined filters, counts ve export bağımsızlığı. | diagreportview.cpp | Filtre verdict'i ve full export'u değiştirmemeli; count doğru. |
| GUI-138 — Diagnostic Report Layout | X06 | **K** (eski A) — Section navigation/scroll retention var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Stable section collapse/selection per report, adaptive table/detail ve refresh regression. | diagreportview.cpp | Rapor yenilendiğinde stable section state korunur; farklı rapora taşınmaz. |
| GUI-139 — Health Navigation Unification | A09 | **K** (eski A) — Global Health key ve lazy catalogue var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Overview/Investigation/Evidence Lab/System Tools aynı stable route/deep-link; mevcut tool key’leri korunur. | healthtab.cpp, overviewpage.cpp, toolcatalog.cpp, uinavigator.cpp | Semptom/arama/favori yolları aynı tool'a gider; arama read-only. |
| GUI-141 — Diagnostic Freshness | A03/C01 | **K** (eski D) — Bazı timestamp/stale ve archive capture boot var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Collection vs render time, source/boot/error/freshness standardizasyonu bütün raporlar. | diagreportview.cpp, smartpage.cpp, netpage.cpp | Failed refresh eski successful time'ı güncellemez; boot değişimi işaretlenir. |
| GUI-142 — Evidence Export Selection | C03 | **K** (eski D) — Bulgu/tablo/raw seçimi ve snapshot preview var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Row/column/evidence/metadata selection, excluded sections ve full verdict açıklaması. | diagreportview.cpp, reportpreview.cpp | Seçilmeyen alan bytes olarak export'ta yok; tam rapor verdict değişmez. |
| GUI-143 — Privacy Preview | C03 | **K** (eski D) — Best-effort structured/raw redaction ve editable preview var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Kalan source-aware fields, UUID/host/path vb. ve explicit override, same frozen preview/copy/save hash. | diagreportview.cpp, reportpreview.cpp | Sentinel secret fixture preview/copy/save'da kalmaz; redaction kapalıysa açık uyarı; kusursuz anonimlik iddiası yok. |
| GUI-146 — Structured Tool Output | C09 | **K** (eski A) — Kernel lsmod/Battery uevent adapter var. | İLİŞKİLİ KISMİ — lsusb ve lscpu -e typed adapter, raw fallback; diğer araçlar ve Qt testleri açık. | Diğer uygun raw tool ID’leri için bounded typed parser ve locale/version error/raw fallback. | systools.cpp, diagreport.cpp, diagnostictools.cpp | Kısmi/bozuk çıktı sıfır değer üretmez; mevcut raw veriye erişim kaybolmaz. |
| GUI-147 — Raw / Structured View | C09 | **K** (eski A) — Structured… ayrı dialog ve raw snapshot var. | İLİŞKİLİ KISMİ — Yeni adapter’lar aynı captured çıktıyı kullanır; tek dialog toggle ve model açık. | Aynı snapshot Raw/Structured toggle, sort/search/selection, model/proxy/export. | systools.cpp, diagreportview.cpp | Toggle yeni donanım sorgusu yapmaz; raw ve structured aynı snapshot'a ait. |
| GUI-150 — Tool Capability Indicators | X06 | **T** (eski A) — Katalog, tool risk ve unsupported/unknown durumları mevcut; yeni ortak capability paneli belgelenmedi. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Tool başına executable/kernel/device/permission requirements ve precise missing reason; read-only capability check. | toolcatalog.cpp, systools.cpp, diagreport.cpp | Eksik binary, permission ve unsupported ayrı; helper çalıştırma yalnız açık işlemde. |
| GUI-151 — Read-Only Tool Comparison | C01 | **K** (eski D) — DiagReport history var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Raw-only araç snapshot/semantic or bounded line diff; stable tool/device and source identity. | diagreportview.cpp, systools.cpp | Farklı tool/device snapshot eşleşmez; restart sonrası pin korunur. |
| GUI-152 — EDID / VRR Visual Inspector | C05 | **K** (eski D) — EDID hex/ASCII, timing ve CTA sınırı var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Byte field/descriptor inspector ve connector/verified VRR timing graph; unknown range ayrı. | diagnostictools.cpp, diagsources.cpp | Corrupt/truncated extension crash olmaz; genel refresh range VRR olarak gösterilmez; yazma yok. |
| GUI-153 — PCIe Topology Visualization | C04 | **K** (eski D) — Canonical tree ve PCIe same-boot delta var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Lazy tree/selection/deep link, stable identity hotplug/BDF reuse, current/max/AER details. | diagnostictools.cpp, advanceddiagnostics.cpp, diagsources.cpp | Unknown parent tahmin edilmez; hotplug/BDF reuse eski delta sayılmaz. |
| GUI-170 — Storage Health Dashboard | C10 | **K** (eski A) — SMART last-success/stale mevcut; C10 bu tur yalnız Sleep. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Storage health history stable NVMe/SATA identity ve capability, replacement fixture. | smartpage.cpp | NVMe/SATA farkı korunmalı; unsupported SMART normal sayılmaz. |
| GUI-171 — SMART Attribute Inspector | C10 | **K** (eski A) — Mevcut SMART tablo. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Vendor-specific raw/normalized/worst/threshold, NVMe ayrı, unknown units/threshold. | smartpage.cpp | Eksik threshold unknown, vendor-specific raw değer yanlış birime çevrilmez. |
| GUI-172 — Kernel Log Severity Filter | X07 | **K** (eski D) — klogpage text/level/boot scope ve context var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Combined typed driver/device/code/boot/time filters ve match count; context independent. | klogpage.cpp | Severe-only filtre context penceresinde hedefi gizlememeli; kaydedilmemiş kayıt uydurulmaz. |
| GUI-175 — Boot Comparison | C01/X07 | **K** (eski A) — bootpage.cpp:comparedWithPrevious curve/event farkı zaten var. | İLİŞKİLİ KISMİ — İki boot seçimi, tam ID, eski önek ambiguity guard; guard/preset history ve fixture açık. | Seçilebilir iki boot; kernel/driver/guard/preset/pstore ve full boot IDs/unknown counts. Önceki boot diff’i tekrar yazma. | bootpage.cpp | Tam boot ID ile eşleştir; filtre dışı boot seçili kalmaz; eksik kayıt '0 hata' sayılmaz. |
| GUI-177 — Network Workspace | X08 | **T** (eski A) — Çoklu alt sekme ve kompakt çok satırlı kontrol layout (S3). | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Ortak workspace navigation, capability/state banner, ilişkili connection→rule→log deep link; mevcut araçlar korunmalı. | netpage.cpp, netfirewall.cpp, netrules.cpp | Bütün alt araçlar erişilebilir; navigasyon firewall state değiştirmez. |
| GUI-178 — Process-Centric Connections | C11/X08 | **K** (eski A) — Program traffic ledger ve socket tuple var. | İLİŞKİLİ KISMİ — C11 tarih/top-N/other trafik sayaçları; PID reuse ve stable grouping açık. | PID+start/exe process groups, socket child/totals; unknown and PID reuse. | netpage.cpp, traffic.cpp | PID reuse yanlış gruplanmaz; unknown PID ayrı; action yalnız unique socket üzerinde. |
| GUI-186 — Listening Port Inspector | X08 | **T** (eski A) — Listening-port grouping mevcut, snapshot zaman ve stale state (S3). | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Bind scope, IPv4/IPv6, process/exe/UID ve port exposure detail; source evidence, unknown process. | netpage.cpp | 0.0.0.0 ve loopback ayrımı; socket identity doğrulanmadan action yok. |
| GUI-187 — Network Table Persistence | X02 | **K** (eski D) — Tablo width ve selection persistence var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Order/visibility/sort/filter/restart, migration ortak GUI-231. | netpage.cpp | Refresh ve restart filtre kaybetmez; vanished/ambiguous selection temizlenir. |
| GUI-190 — Scene Diff | D01 | **K** (eski D) — JSON diff ve execution log var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Scene component/Choice semantics ve scene vs live/operation effects. | scenestab.cpp, profilediff.h | Eksik component no-op ile default ayrılmalı; secret command args loglanmamalı. |
| GUI-191 — Scene Conflict Inspector | D01 | **K** (eski D) — Missing refs/preview var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Engine priority ve overlap/component conflict relationships; static vs runtime. | scenes.cpp, scenestab.cpp | Çakışma sonucu priority gerçekten engine ile eşleşir; dry-run side effect yok. |
| GUI-192 — Scene Execution Preview | D01 | **K** (eski D) — Text dry-run ve gerçek step log var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Structured actual ordered plan old/new/skip/blocked/reason/dependency/revision. | scenes.cpp, scenestab.cpp | Preview/execute plan aynı input'ta uyuşur; preview write/command launch yapmaz. |
| GUI-193 — Automation Timeline | C01/D01 | **K** (eski A) — Scene log gerçek callback ve 200 satır son-run history. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Cross-boot persistent automation trigger/rule selected/rejected, step/rollback, secret redaction. | scenes.cpp, evidencereports.cpp | Clock/boot boundaries korunur; secret arguments yok; unknown trigger uydurulmaz. |
| GUI-195 — Scene Dependency Checker | D01 | **K** (eski D) — CPU/GPU/tune missing refs var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Bütün scene refs, capabilities/identity/support/ambiguity ve dependency revalidate. | scenes.cpp | Silinen profile göre apply disabled veya mevcut policy açık; preview no writes. |
| GUI-197 — Scene Apply Result | D01/A05 | **K** (eski A) — Running/returned/failed/soft/skip/restore step log mevcut. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Transaction ID, verified vs helper-returned, per-component requested/result/partial, rollback and daemon events. | scenes.cpp, scenestab.cpp | Partial apply success olarak gösterilmez; rollback başarısızlığı açık; diğer scene sonucu karışmaz. |
| GUI-198 — Automation Dry Run | D01 | **K** (eski D) — Read-only text preview var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Simulated AC/game/time/trigger input, actual engine priority/conditions step plan, zero command/write. | scenes.cpp, scenestab.cpp | Simulated AC/game/time event'leri engine kararını eşler; command/privileged call yok. |
| GUI-199 — Adaptive Keyboard Canvas | D02/X09 | **K** (eski A) — KeyboardView preview mevcut. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Content/aspect adaptive layout, zoom/pan/hit test/HiDPI, zone4 parity. | lightingtab.cpp, kblayout.cpp, zone4tab.cpp | Dar/geniş ve DPI değişiminde aynı tuş seçilir; clipping yok. |
| GUI-206 — Unsaved Lighting Preview | A05/D02 | **K** (eski D) — refreshPreview ve not applied dirty label mevcut. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Draft/applied/readback/pending/failed/unknown explicit status, shared dirty registry; mevcut preview tekrar yazılmaz. | lightingtab.cpp, lighting.cpp | Undo/Redo/preview donanıma yazmaz; failed apply draft'ı applied olarak işaretlemez. |
| GUI-209 — Backup History Explorer | D03 | **K** (eski A) — Archive table, sidecar and verification info mevcut. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | History filter type/size/chain/encryption/verified/stale, stable archive identity. | backuppage.cpp | Eksik sidecar unknown; yanlış selection restore action'a dönüşmez. |
| GUI-210 — Restore Preview | D03 | **K** (eski D) — config_inspect scope tree ve image unsupported var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Helper-supported path manifest/target metadata/hash and overwrite semantics, path security/revalidate; unsupported image korunur. | backuppage.cpp, mevcut backup helper | Preview hiçbir dosya değiştirmez; path traversal/symlink ve yanlış target reddedilir; confirmation korunur. |
| GUI-211 — Backup Dependency Tree | D03 | **K** (eski D) — Oldest-first sidecar chain preview var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Graph stable archive identity/cycle/missing/ambiguous, auth/verify distinction. | backuppage.cpp | Metadata tek başına restore authorization değil; cycle/ambiguous chain fail-closed. |
| GUI-212 — Backup Progress Inspector | A05/D03 | **K** (eski A) — backuppage.cpp:progress event handler ve QProgressBar zaten var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Gerçek byte/file/phase/elapsed kapsamını kontrol et; eksik total indeterminate, verify/cancel/restore registry entegrasyonu. Mevcut progress’i tekrar yazma. | backuppage.cpp, mevcut backup helper | Yüzde total olmadan uydurulmaz; cancelled/verify/restore phase ayrı. |
| GUI-213 — Backup Integrity Report | D03 | **K** (eski D) — Local verification state ve sidecar var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Archive identity/hash/time freshness, supported per-file/chain/auth distinction; cancel/password ≠ corrupt. | backuppage.cpp | İptal/password denied corrupt sayılmaz; readable ≠ authenticated/restore-ready. |
| GUI-216 — Non-Blocking GUI | A04 | **K** (eski D) — Autotune weights async ve guards var; CO sync çağrılar var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Kalan sync call graph, async worker/QProcess, timeout/cancel/restore/QPointer. | privileged.cpp, userhelper.cpp, cosearchdialog.cpp, optimizetab.cpp, systools.cpp, cpustressbox.cpp | Fake helper yavaş/timeout/malformed iken event loop responsive; late result ignore. |
| GUI-217 — Extended Lazy Initialization | A07 | **K** (eski A) — LazyWidget/Health catalogue var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Diğer heavy pages create-on-demand ve busy lifecycle, startup construction counters. | lazywidget.h, mainwindow.cpp, healthtab.cpp | Startup tüm heavy widgets oluşturmamalı; lazy switch aktif işlemi kesmemeli. |
| GUI-219 — Adaptive Polling | A03 | **K** (eski A) — 6 display TTL slots var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Shared visibility/subscriber/freshness/dGPU sleep-aware scheduler ve bounded intervals. | hometab.cpp, sysinfo.cpp, netpage.cpp | Hidden tab poll düşer; aynı query çoğalmaz; uyuyan GPU sırf polling ile uyanmaz. |
| GUI-221 — Incremental Table Updates | X06 | **K** (eski A) — Refresh selection/scroll restore var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Incremental typed model updates stable row keys; large table fixture. | netpage.cpp, diagreportview.cpp, bootpage.cpp | 10k row refresh sadece değişen satırları günceller; vanished selection güvenli temizlenir. |
| GUI-222 — Unified Error Presentation | A05 | **K** (eski A) — Operation observation history ve helper messages mevcut. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Structured error model source/op/code/retryable/cancel/stale + redacted bounded log. | mainwindow.cpp, privileged.cpp, diagreportview.cpp | Permission/timeout/malformed/cancel ayrı; secret arguments gösterilmez. |
| GUI-224 — Unified Operation State | A05 | **K** (eski D) — Polling busy history mevcut. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Event registry lifecycle transition, real verify/restore/result, duplicate finish guard. | mainwindow.cpp, cpustressbox.cpp, coolingpage.cpp | Impossible transition reddedilir; restoring'de idle olmaz; duplicate finish tek kez. |
| GUI-225 — Concurrent Operation Guard | A05 | **K** (eski D) — Known active guards var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Atomik central resource lock, transaction owner ve independent read policies; no lock leak. | mainwindow.cpp, coolingpage.cpp, cpustressbox.cpp | Çakışan işlem bloke; independent read-only işlem uygun olduğunda serbest; failure lock leak yok. |
| GUI-226 — Safe Restart Coordination | A05 | **K** (eski D) — Tray/window busy guard var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Kalan config/restart/preferences/close yolları ve common dirty check; OS force limits. | mainwindow.cpp, hometab.cpp, configpage.cpp | Active write/restoring normal quit ile kesilmez; cancel sonrası state korunur; zorla kill garantisi yok. |
| GUI-228 — Unified Chart Engine | A06 | **K** (eski D) — LineChart zoom/pan/gap/CSV/downsampling var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Ortak chart API diğer chartlar/VF, units/axes/legend/accessibility and zoom hit-test. | charts.cpp, charts.h, vfcurvewidget.cpp | NaN/gaps doğru, zoom selection/hit-test doğru; V/F write/edit semantics korunmalı. |
| GUI-229 — Stale Data Detection | A03 | **K** (eski D) — TTL quality model display slots var. | İLİŞKİLİ KISMİ — Firmware stale WMI ve doğrudan sysfs okuma ayrımı; ortak telemetry kapsamı açık. | Bütün read-only consumers source/boot/device timestamps, stale/unknown/error display; post-write direct. | sysinfo.cpp, hometab.cpp, netpage.cpp, smartpage.cpp, fwattrtab.cpp, optimizetab.cpp | Failed refresh eski veriyi fresh göstermez; stale boot match ve hardware verification yasak. |
| GUI-230 — Keyboard Accessibility | A01/A08 | **K** (eski A) — Bazı shortcut/accessibility ve chart keyboard pan var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Tüm GUI tab order/focus/name/description, keyboard-only actions/custom chart accessible fallback. | mainwindow.cpp, hometab.cpp, vfcurvewidget.cpp, tüm GUI | Mouse olmadan tüm read-only araçlar ve normal işlemler erişilir; focus trap yok. |
| GUI-231 — Persistent Widget State | X02 | **K** (eski D) — Bazı geometry/splitter/width persistence var. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Ortak sürümlü per-shell stable view ID store, migration/reset/selection; GUI-06 ile tek uygulama. | mainwindow.cpp, cosearchdialog.cpp, netpage.cpp, bootpage.cpp | Eski corrupt setting safe fallback; 2 shell state çakışmaz; hidden selection restore edilmez. |
| GUI-232 — Asynchronous Result Protection | A04 | **K** (eski D) — Scene/Wi-Fi/Autotune revision guards var. | İLİŞKİLİ KISMİ — WMI row revision ve Autotune run ID guard; ortak async altyapı açık. | Tüm helper callback request generation/target/lifetime/cancel guard, A→B→A races. | scenestab.cpp, privileged.cpp, userhelper.cpp | A→B→A ve reversed replies fixture; eski result hiçbir yeni editörü mutate etmez. |
| GUI-235 — Automated Visual Regression Tests | A08 | **K** (eski D) — Optional baseline + repeat-render test mevcut; baseline yok. | ÖNCEKİ KISMİ — bu ID için yeni alt işlev doğrulanmadı; kabul açık. | Gerçek main app fake fixture ve reviewed PNG baselines, DPI/dil/shell matrix, failure artifact, intentional regression. | gui/tests, gui/CMakeLists.txt, .github/workflows/ci.yml | Hardware helper erişimi yok; CI native test çalıştırır, intentional layout regression yakalanır. |

## 5. Yeniden üretim / release kapısı

Kaynak ağacında çalıştırılacak komutlar (Qt6 >= 6.4, Rust/Cargo ve mevcut proje bağımlılıkları kurulmuş bir ortamda):

```sh
cmake -S gui -B build-debug -DCMAKE_BUILD_TYPE=Debug -DCENTURION_BUILD_TESTS=ON
cmake --build build-debug -j$(nproc)
QT_QPA_PLATFORM=offscreen ctest --test-dir build-debug --output-on-failure
cmake -S gui -B build-release -DCMAKE_BUILD_TYPE=Release -DCENTURION_BUILD_TESTS=ON
cmake --build build-release -j$(nproc)
QT_QPA_PLATFORM=offscreen ctest --test-dir build-release --output-on-failure
cargo build --locked
cargo test --locked
cargo clippy --locked --all-targets -- -D warnings
cargo deny check
python3 gui/tests/integerunits_native_test.py
```

Bu komutlar kalan işlevleri kendiliğinden uygulamaz; önce §3–4 tamamlanmalı. Fixture, ASan/UBSan ve reviewed baseline doğrulaması yapılmadan release/push yapılmamalı.

## 6. Değişen kaynak dosyaları

| Dosya | Durum |
|---|---|
| `crates/centurion-helpers/src/autotune.rs` | Değişti |
| `gui/CMakeLists.txt` | Değişti |
| `gui/i18n/centurion_tr.ts` | Değişti |
| `gui/src/autotuneprogress.h` | Yeni |
| `gui/src/bootpage.cpp` | Değişti |
| `gui/src/bootpage.h` | Değişti |
| `gui/src/fwattrtab.cpp` | Değişti |
| `gui/src/fwattrtab.h` | Değişti |
| `gui/src/hometab.cpp` | Değişti |
| `gui/src/hometab.h` | Değişti |
| `gui/src/int64spinbox.cpp` | Değişti |
| `gui/src/int64spinbox.h` | Değişti |
| `gui/src/integerunits.h` | Yeni |
| `gui/src/mainwindow.cpp` | Değişti |
| `gui/src/mainwindow.h` | Değişti |
| `gui/src/netpage.cpp` | Değişti |
| `gui/src/netpage.h` | Değişti |
| `gui/src/optimizetab.cpp` | Değişti |
| `gui/src/structuredtools.h` | Değişti |
| `gui/src/systools.cpp` | Değişti |
| `gui/src/traffic.cpp` | Değişti |
| `gui/src/traffic.h` | Değişti |
| `gui/tests/autotuneprogress_test.cpp` | Yeni |
| `gui/tests/gui_stage4_static_test.py` | Değişti |
| `gui/tests/int64spinbox_test.cpp` | Değişti |
| `gui/tests/integerunits_native_test.py` | Yeni |
| `gui/tests/structuredtools_test.cpp` | Değişti |
| `gui/tests/studio_v4_static_test.py` | Değişti |
| `gui/tests/traffic_test.cpp` | Değişti |

Toplam 29 kod/test/katalog dosyası; 0 silinen dosya. Aşağıdaki rapor/matris ayrıca ZIP içine eklenmiştir. Kanıt paketi ham logları, SHA manifestini ve unified diff’i içerir.
