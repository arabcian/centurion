# Centurion Experimental — Tur 26 kısmi teslimat

## Baseline ve kapsam
Tur25 ZIP SHA-256 `12884ad4a81e5a1f2102ddd7943a457349e9e1aaa49fa806fce9465054c155f4`; 669 dosya; CRC PASS. Opus düzeltilmiş Tur22 soyunu korur. Bu tur 20 mevcut kaynak/konfigürasyon dosyası değişti ve 7 yeni kaynak/test dosyası eklendi; kaynak silinmedi. Özgün 48 paket / 118 GUI ID / 21 FIX matrislerinin tüm hücreleri korunarak Tur26 sütunları eklendi; ek ID'ler önceki satırlarda korunur. 0/48 tam kabul; 47 kısmi, D05 engelli. Bu tur baseline'ın ayrı tam native derlemesi yapılamadı; Tur25 raporundaki 51 runner sonucu önceki teslimata aittir.

## A03 — Ortak asenkron telemetri
- `telemetryservice.*` ve `telemetrybroker.h`: altı metrik için en çok bir fiziksel flight, 64 bekleyen istek, her istek için gerçek boottime deadline ve yaşam süresine bağlı iptal; timeout eski uçuşu serbest bırakmaz. Geç gelen sonuç ve TTL sonu teslim anında reddedilir. QTimer waiter tamamlandığında ya da owner yok olduğunda temizlenir. Fiziksel sysfs I/O kesilemez; yeni istek aynı flight'a katılabilir, sınırsız retry başlatılmaz.
- Home ve metinsel/sayısal D-Bus aynı asenkron dağıtıcıyı kullanır; Health Overview görünürken CPU sıcaklığını aynı kaynaktan alır. Home Live sensör satırları artık constructor getter'ı çalıştırmaz; ilk geçerli sonuçta gösterilir. Eski değerler TTL dolduğunda polling sırasında temizlenir. Home'un EC dGPU okumaları ve doğrudan güvenlik/post-write okumaları ayrı kalır.
- Sayısal örnekler unit, kaynak, canonical aygıt yolu (kanıt yoksa boş), capture/TTL, kalite, boot ID ve generation taşır. Fan ve güç için kaynak seçimi mevcut davranışa bağlı. Canonical path tek başına aygıt ömrü kimliği değildir. Batarya kaynağı aggregate dizinidir. D-Bus mevcut imzaları korunur; timeout hatası ayrı.
- Kalan: tam subscription scheduler (AC/suspend/hotplug), stable per-device lifetime identity ve invalidation, Health'in diğer tüketicileri, SMART/stress/AMD/NVIDIA telemetri entegrasyonu, gerçek Qt ve fiziksel no-wake doğrulaması. Home'un yardımcı non-metric sweep'i ayrı worker'da ve kesilemez; görünürlük/polling tam kabulü açık.

## B03 — GPU kimliği ve V/F işlem geçmişi
- `nvml.rs`, `hal/gpu.rs`, `ops.rs`: NVML fresh handle, UUID, full PCI BDF, driver/VBIOS. NVAPI/NVML index eşitliğine dayanarak çoklu kartı eşleştirme yapılmaz. Şimdilik her iki API'nin yalnızca tek fiziksel görünür GPU bildirdiği, model adının eşleştiği, handle ve kimliğin yeniden doğrulandığı durum kabul edilir. Tam çoklu-GPU eşleştirme veya fiziksel hotplug garantisi değildir. Eksik/uyumsuz kimlikte edited Apply/Validate fail-closed; eski helper ile yeni GUI birlikte edited Apply yapamaz. Yeni GUI/helper birlikte güncellenmelidir.
- Düzenlenmiş GUI Apply frozen UUID/PCI/driver/VBIOS ve point layout'u root helper'a iletir; preliminary check'ten sonra gerçek Rust preflight ve write öncesi kimlik/CT tekrar kontrol edilir. Read-only validator aynı kimliği kontrol eder. Existing named profile/CLI/autoload ve emergency reset yollarının yazma algoritması korunur; bu yollar yeni kimlik şartını otomatik olarak kazanmadı. Restore güvenliği için trial reset'e yeni NVML şartı eklenmedi; trial kökeni journal'da kaydedilir.
- `curvejournal.h`: actual Apply / named profile / Reset ve trial-origin reset dispatch öncesi pending transaction, aynı request ID ile frozen root result. `tunejournal` bounded atomic storage altyapısı ayrı V/F history dosyasında kullanılır (100 işlem, 4 MiB, QLockFile/QSaveFile, owner-only, bozuk dosyayı koru). Kullanıcıya V/F History tablosu ve detay. Exact non-flatten per-point offset ve memory karşılaştırması; layout/device mismatch, missing readback, unknown/helper failure ayrı. Flatten için GUI draft delta exact preflight sonucu olarak gösterilmez; clock cap/lock/stability nokta eşleşmesiyle doğrulanmaz. History hiçbir write/restore yetkisi sağlamaz; PGO'da kayıt yok. Root helper raporu fresh GUI readback ile eşdeğer değildir.
- Kalan: cross-API çoklu kart eşleştirme, profil metadata, overlay/delta chart ve linked inspector, tam flatten/readback receipt, clock cap ve lock kanıtı, Xid/boot/scene/session bağlama, trial restore sahipliği ve native Rust/Qt/fiziksel testler. Yeni Rust ve Qt yolu derlenmedi; üretim için doğrulanmış sayılmaz.

## A07 — MainWindow simülasyonu
Mevcut izole `--simulate` dispatch ve fixture workspace incelendi. Gerçek MainWindow backend injection kapsamı henüz tamamlanmadı; mevcut izole çalışma yolu değiştirilmedi. A08 reviewed baselines de açık.

## Test sonuçları
| Test | Sonuç |
|---|---|
| İlk genişletilmiş bağımsız runner | 52/52 PASS |
| Son dondurulmuş kaynak bağımsız runner | 52/52 PASS (hedef 52; eksik varsa çalıştırılmamış/bitmemiş) |
| Shared flight token ASan/UBSan | PASS (EXIT=0) |
| Shared flight token GNU LTO + -Werror | PASS (EXIT=0) |
| Mevcut telemetrycache.cpp bağımsız test (telemetrycache_test.cpp) | PASS |
| Telemetrycache ASan/UBSan ve GNU LTO + -Werror | PASS (EXIT=0) |
| Türkçe katalog / NVIDIA static safety | PASS (seçili rerun; son tam koşu ayrıca yukarıda) |
| Git diff --check | PASS |
| Qt6 Debug/Release configure | FAIL — Qt6Config.cmake yok |
| apt update | DNS başarısız, exit 0 başarı değil |
| apt install simulation | FAIL — paketler bulunamadı |
| Cargo test --workspace --locked | FAIL — cargo yok (EXIT=127) |
| Yeni Qt telemetryservice/curvejournal CTest | Eklendi; çalıştırılmadı |
| Yeni Rust UUID testi, Clippy, cargo-deny | Çalıştırılmadı |
| Tam Gentoo native LTO/PGO, GUI sanitizer/visual, fiziksel test ve CI | Çalıştırılmadı |

16 yeni TR katalog girdisi; özgün FIX-01–21 tam kabulü açık. Önceki bütün 48 paket ve 118 GUI ID kapsamı korunur. GitHub push yapılmadı. Yeni sürüm tam kabul değildir.
