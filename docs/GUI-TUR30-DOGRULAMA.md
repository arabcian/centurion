# Centurion Experimental — Tur 30 kısmi doğrulama ve devir

Temel: `Centurion-experimental-tur29-kismi.zip` (SHA-256 `d7b792d9b0faf856fd6c6c413291736d8e65c1944f551c7bd41d35f92be4f772`). Tur29 özel 11 istek ve build düzeltmesi korunur. Yeni kabul: **0/48 tam, 47 kısmi, D05 engelli.** Özgün kapsam daraltılmadı.

## Gerçek kaynak değişiklikleri

- C01/C02: Cross-Boot History, mevcut v1 archive (128 dosya/32 MiB, dosya başına 1 MiB/20 entry), tam boot UUID, eksik/prefix UUID aynı boot sayılmaz; malformed/duplicate archive ID reddi, capture boot ve olay boot ayrı. Kayıtlı health.log ve kernel.log (her biri 1000 tail olayı); kaynak/event detail, aynı kaynak duplicate hash; yeni scan yok. Aynı code/locale arşiv çiftinde multiset diff, snapshot notu atomik optimistic lock, duplicate capture engeli. Bu gerçek kalıcı incident/device graph veya rolling log importu değildir.
- X06/X02: Kernel Parameters QAbstractTableModel/proxy, path stable key (cihaz yaşam döngüsü değil), changed-only value notifications, grouped insert/remove, >128 structural intervals reset fallback. Aynı scope selection/top anchor korunması, başarısız capture temizlenmesi, eski snapshot pending işareti, scope/filter/sort/path width per shell; scope açılışta otomatik taranmaz. Mevcut tam frozen capture JSON export korunur. Cross-Boot filter/splitter de per shell.
- C10 SMART: Recorded History (30/90/all days) mevcut geçmiş üzerinden; seri/model/kapasite eksikse yeni stable ID oluşturulmaz, duplicate ID aynı capture atlanır, malformed store üzerine yazılmaz; 16 MiB/512 devices/730 daily rows, lock ve atomik owner-only write. Var olan güvenilir ID algoritması değişmedi. Mevcut otomatik SMART kontrolleri devam eder, yeni self-test veya hardware poll eklenmedi.
- C10 cooling: karşılaştırılan testlerin süre eşitliği zorunlu; missing values unknown, capture context tooltip; Rust assessment Inconclusive. Fan/stress safety korunur.
- C10 battery/sleep: mevcut günlük battery sample'a gerçek UTC, boot, status ve geçerli charge/Wh/W eklenir; boşluklar saklanır. Sleep hook inflight boot, after-unit/status; başka boot'taki post marker'ı tüketmez. Farklı sayaç birimi ve bilinen AC/şarj bağlamı karşılaştırılmaz; eski eksik context unknown. Rust overview aynı guard. Yeni hardware stress/wake yok; fiziksel no-wake kanıtlanmadı.
- D04: 67 yeni TR girdisi. D05: iki yeni bağımsız runner ve iki yeni Qt fixture, sleep Qt fixture genişletildi; CI tanımı güncellendi, CI çalıştırılmadı.

## Testler

| Test | Sonuç |
|---|---|
| Tur29 baseline | 29/29 tamamlanan PASS; koşu kesildi, tam baseline değil |
| İlk genişletilmiş runner | 56/56 PASS |
| Son kaynak runner | 57/57 PASS |
| Incremental planner / boot UUID | 20.000 rastgele reconciliation ve 10k key, O2/LTO -Werror/ASan+UBSan PASS (bağımsız fixture) |
| Sleep hook | İzole fake path gerçek hook PASS; ilk test harness path substitution hatası düzeltildi |
| Qt6 Debug/Release configure | FAIL: Qt6Config.cmake yok |
| Apt update | DNS çözümleme başarısız |
| Rust build/test/clippy/deny | Cargo yok, çalıştırılmadı |
| Qt CTest (kernelparamsmodel, diagnostichistory, sleepadvice dahil) | Çalıştırılmadı |
| Gerçek GUI, Gentoo PGO/LTO, fiziksel donanım, CI | Çalıştırılmadı |
| git diff --check | PASS |

Bağımsız runner'lar statik ve native C++ karışımıdır. Qt/Rust programının derlendiği sonucunu desteklemez. Yeni C++/Rust kodu sistemde native build gerektirir.

## Kalan özgün kapsam

- C01: Kalıcı rolling olay importu, diğer kaynak adaptörleri, semantik stabil finding/row ID, cihaz/session kimliği ve saat belirsizliği.
- C02: Gerçek incident/device/tool graph, typed edge modeli, confidence/zoom/deep link ve kaynak eksikliği davranışı.
- C10: Ortak opt-in scheduler, battery stable identity, cooling ambient/fan/power context, SATA vendor raw/normalized/worst, bütün session history ve fiziksel doğrulama.
- X06: Diğer tablolar, report adaptive state/capability, Qt gerçek 10k performans ve selection fixture.
- X02: Tüm lazy view state, common dirty interface, header order/visibility ve migration/selection/scroll native test.
- D04: FIX-01–21 native kabul, iki shell TR/EN/DPI ve gerçek layout.
- D05: Qt6/Rust/Cargo/CTest, CI, Gentoo LTO/PGO ve fiziksel güvenlik doğrulaması.
- Tüm diğer 48 paket / 118 GUI / ek ID / FIX / Wanted kriterleri önceki haliyle açık. Orijinal hücreler aynen korunmuştur.
- Sonraki öncelik: C04/C06/C07/C08/C09 ve A07/A08 için mevcut altyapının yeniden denetimi; C01/C02/C10/X02/X06 kalan işlerden vazgeçilmedi.
- GitHub push yok.
