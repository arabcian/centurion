# Centurion Experimental — Tur 36 doğrulama raporu

**KISMİ teslimat.** Tur35 tabanı doğrulandı; 13 mevcut kaynak/CI/test dosyası değişti, 3 yeni, 0 silinen. Bu sayılar T36 rapor ve matris dosyaları eklenmeden önceki gerçek diff'tir. Tüm kaynak ağacı yeniden satır satır denetlenmedi. 0/48 tam kabul, 47 kısmi, D05 engelli; 118 GUI ID açık/kısmi. FIX-01–21 ve WANTED 1–11 + BUILD açık. 48 paket ve GUI ID kapsamları mükerrer iş değildir.

## Gerçek değişiklikler ve kalan kapsam

### A03

- İlerleme: TelemetryService per-waiter deadline kontrolü; zamanında alınan diğer metrikler korunur; aynı uçuşun geç yanıtı kısa bekleyene timeout, uzun bekleyene geçerli olabilir. Overview CPU sıcaklık son geçerli örnek/TTL ve görünürlük koruması.
- Sınır: Diğer Health/Stress/SMART/AMD/NVIDIA tüketicileri, stable device lifetime, tüm quality/error schema, AC/suspend scheduler, stale chart gap/no-wake; yeni Qt kısmi teslimat ve expiry testleri çalıştırılmadı.
- Test: telemetryservice_test.cpp yeni boottime-jump fixture (Qt BLOCKED); mevcut bağımsız flight runner ve kaynak wiring PASS.

### A04

- İlerleme: CO 4 SMU mutation çağrısında runUntilExit: timeout bildirimi ayrı, sonuç gerçek süreç çıkışında; geç başarı unknown. Recovery Cancel: bekleyen helper terminal olunca resume/discard yapılmaz, journal korunur; ikinci evidence çağrısı açılmaz. Normal reset QTimer ile önceki helper çıkışını bekler.
- Sınır: Zorunlu destructor blockingReset/msleep korunuyor; bütün dosya/sysfs/IPC, signal/forced shutdown ve A→B→A lifecycle tam değil. Native Qt 60s mock, Cancel/late callback ve CO gerçek UI testi açık.
- Test: operationlease_native_test, operationreceipt_native_test, cohistory_sceneplan_native_test, CO reboot/walk runner, static wiring PASS. privileged-lifecycle CTest yazıldı ancak Qt BLOCKED.

### A05

- İlerleme: CPU eğrisi GUI-process oturum lease; kendi işlemine yalnız kendi lease bypass, normal yabancı CPU write/Scene reset engelli; emergency override korunur. Active helper timeout/owner loss sonrası release ertelenir; stock throughput boyunca lease, önceki Scene uygulanmadan önce bırakılır.
- Sınır: Scene restore ownership/owner propagation, pre-scene acquisition, cross-process/root lock, ABBA/Trial/firmware/fan/cooling/backup/lighting, tüm history/dirty/quit; explicit emergency ve dış süreçlerin lease dışı işlemleri; fiziksel ve Qt kabul açık.
- Test: operationlease_native_test normal, ASan+UBSan, GNU LTO -Werror PASS; eski scoped registry runner PASS; Qt CTest BLOCKED.

### D01

- İlerleme: SceneEngine::helper CPU Ryzen/Intel normal adımları runUntilExit ile terminal çıkışı bekler; normal Scene reset, CO lease üzerinde emergency override kullanmaz.
- Sınır: Visual node builder, full dependencies, trigger overlap, restore owner propagation, diğer Scene helper adımları ve bütün rollback/partial/daemon native kabul açık.
- Test: cohistory_sceneplan_native_test ve yeni kaynak wiring PASS; gerçek Scene Qt/fiziksel test BLOCKED.

### D04

- İlerleme: T36 yeni CO recovery/timeout/lease kullanıcı metinleri TR katalogda.
- Sınır: Tüm GUI layout/çeviri/başlık/raw JSON ve FIX-01–21 özgün kriterleri açık. Yeni GUI mesajları native görsel olarak doğrulanmadı.
- Test: translation_tr_test 5 test PASS; native Qt BLOCKED.

### D05

- İlerleme: SHA/CRC doğrulandı. Gerçek apt update/install denendi; baseline ve final Qt Debug/Release configure denendi; Cargo locked build/test/clippy/deny denendi. 64 Python runner, hedefli 5 rerun, yeni lease sanitizer/LTO PASS; CTest mock eklendi.
- Sınır: Qt6Config.cmake yok, Cargo yok, DNS çözümleme başarısız. Native Qt/Rust/CTest/CI, Gentoo gerçek PGO/LTO, visual baseline ve fiziksel kabul yok; experimental push yapılmadı.
- Test: apt update exit 0 fakat DNS error; apt install exit 100; Qt configure exit 1; Cargo exit 127. Ayrıntılı loglar evidence ZIP içinde.

## Test ve ortam kanıtı

| İş | Sonuç | Ayrıntı |
| --- | --- | --- |
| Tur35 kaynak SHA-256 / kaynak ve devir CRC | PASS | SHA rapordaki değerle birebir aynı, iki ZIP CRC hatasız. |
| İlk bağımsız Python sweep | 63/63 PASS | Son T36 kaynak düzenlemelerinden önce; eski testler + yeni lease. |
| İkinci bağımsız Python sweep | 64/64 PASS | Son küçük CO/registry/TR düzenlemeleri sweep sırasında yapıldı; etkilenen 5 test son düzenlemelerden sonra ayrıca çalıştırıldı. |
| Final hedefli rerun | 5/5 PASS | operationlease, operationreceipt, cohistory_sceneplan, tur36_lifecycle_static, translation_tr. |
| Lease ASan+UBSan | PASS | -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -no-pie, -Wall -Wextra -Werror. |
| Lease GNU LTO | PASS | -O3 -flto=auto -fno-fat-lto-objects, -Wall -Wextra -Werror. Gentoo tam proje LTO/PGO değildir. |
| apt-get update | ENGELLİ | exit 0 ancak Temporary failure resolving 'deb.debian.org'; index alınamadı. |
| apt-get install | ENGELLİ | exit 100; Qt6, Cargo/Rust ve ilgili paketler bulunamadı. |
| Baseline Qt Debug/Release configure | FAIL (ortam) | exit 1; Qt6Config.cmake bulunamadı. |
| Final Qt Debug/Release configure | FAIL (ortam) | exit 1; Qt6Config.cmake bulunamadı. Derleme aşamasına geçilemedi. |
| Rust locked build/test/clippy/deny | ENGELLİ | exit 127, cargo: command not found. |
| Qt CTest / 60s mock / boottime fixture | ÇALIŞTIRILMADI | Native test hedefleri yazıldı, Qt bulunamadı. |
| CI / Gentoo PGO/LTO / gerçek visual baseline / fiziksel | ÇALIŞTIRILMADI | Donanım yazımı/stres/reboot/restore yapılmadı. |
| GitHub experimental push | YAPILMADI | Devirdeki native kabul kapısı sağlanmadı. |

## Güvenlik ve açık kararlar

- `runUntilExit` yalnız CO SMU mutation ve Scene CPU helper yollarında kullanılıyor; normal diğer helper timeout davranışı korunuyor. Terminal timeout, helper başarı JSON'u döndürse bile sonucu Unknown. `CENTURION_PRIVILEGED_FIXTURE` yalnız yeni CTest hedefinde derleme makrosu; üretimde pkexec sabit yolları korunuyor. Bu fixture gerçek MainWindow simülasyonu veya fiziksel güvenlik kanıtı değil.
- CO lease aynı GUI sürecindeki sınıflandırılmış CPU yazmalarını kapsar; ayrı CLI/root/daemon süreçleri, acil reset ve dış yazmalar için sistem-geneli kilit değildir. Scene ön hazırlığı lease öncesi, eski Scene restore ise lease sonrası başlatılır. Bunlar A05'in açık kapsamıdır. Stock throughput bitene dek lease tutulması amaçlandı. Helper gerçek çıkışını bekleme süresinin üst sınırı yok; event loop bloke edilmez ancak root helper çıkmazsa oturum busy kalır.
- Mevcut CO journal/guard ve emergency destructor blocking reset yolu kaldırılmadı. Force kill/OS kapanışında güvenli restore garantisi verilmez; destructor bloklama ve 1.5s ikinci reset varsayımı hâlâ A04 açığıdır. Yeni normal recovery cancellation journalı korur; Qt UI davranışı derlenip test edilmedi.
- Shared telemetry deadline düzeltmesi tüm metriklerdeki waiter davranışını değiştirir; mevcut cache, no-wake ve kritik doğrudan safety readback yolları korunur. Native fixture çalıştırılmadı.
- Özgün FIX-01–21 bireysel kabul metinleri sağlanmadığı için uydurulmadı. T36 kapsamında diğer paketler kapatılmadı.

## Kaynak ve kanıt

- `Centurion-Tur36.patch`: T35 → T36 unified diff (rapor ve matris ekleri hariç).
- `Centurion-Tur36-source-changes.tsv`: dosya bazında eski/yeni SHA-256.
- `GUI-TUR36-48-PAKET.tsv`, `GUI-TUR36-118-KABUL.tsv`, `GUI-TUR36-FIX-KABUL.tsv`, `GUI-TUR36-WANTED-KABUL.tsv`: önceki bütün sütunlar + Tur36 kayıtları.
- `GUI-TUR36-RUNNERS.tsv`: ikinci sweep; ayrı final hedefli loglar kanıt paketinde.
