# Centurion Experimental — Tur 24 kısmi teslimat

## Baseline ve kapsam
Tur 23 kısmi ZIP (Opus düzeltilmiş Tur22 soyundan): `Centurion-experimental-tur23-kismi.zip`. SHA-256 `06dade43ce734818f09879ca449e57e9ac235453b8a06a23688a5e4a1fd77e00`. ZIP CRC PASS, 652 dosya. Tur23 49/49 baseline runner sonucu önceki teslimat kaydıdır; bu tur taze native baseline derlemesi yapılmadı. Özgün devir ve önceki matrisler aynen korunmuştur; Tur24 matrisleri eski sütun ve satırların tamamını taşır (48/118/21). Ek GUI ID'ler özgün paket başlıklarında kalır. 0/48 tam kabul, 47 kısmi, D05 engelli. Bütün özgün GUI/FIX kabul koşulları açık.

## Gerçek kaynak değişiklikleri
### A02 — canlı tema
Normal Home seçim kutusu, galeri ve custom palet Save artık restart istemek yerine aynı doğrulanmış snapshot'ı persistent seçim ile canlı uygular. Custom RGB doğrulaması ve QSettings atomik sync kontrolü; geçersiz palet veya sync hatasında running theme değişmez. Reentrant/GUI-thread guard. Widget başına değiştirilebilir semantic local QSS callback, mevcut QObject yaşam döngüsüne bağlı. Health, Ryzen, NVIDIA, NVReg, Optimization, AMD GPU, firmware, Scenes, CO Search, Lighting/Zone4 ve TrialBar seçili sunum yolları yenilenir. CO table brush rolü üzerinden değiştirilir; refreshTable çağrılmaz. Lighting gerçek RGB renkleri korunur. TrialBar restore request başarılmış restore olarak gösterilmez; callback ve zamanlayıcı akışı aynı kalır. Tipografi/yoğunluk/dil/shell değişimlerinin restart yolları korunur. Her cache/icon/rich-text/chart/lazy view kapsamı ve sıfır yeni donanım sorgusu native olarak kanıtlanmadı. 20 geçiş, QLineEdit undo, callback replacement ve invalid preference fixture eklendi; Qt runtime test çalıştırılmadı.

### B11 — ölçülmüş ABBA ve game session inceleyicileri
Optimization mevcut calibration signature v10 viewer: Measured ABBA History, filtrelenebilir en son 200 kaydın tablosu ve seçim detayları (gerçek round sayısı, gain mean/SE, decision interval, context, skipped, OOM, early termination, verified values). Eksik, çelişkili, geçersiz schema/context ve recorded pass ayrımı; geçmiş bir başarı güncel apply yetkisi değil. Bağımsız abverify.h classifier ve CI runner. Measured Game Sessions en son 400 kayıt: game/start, recorded config, median/tail/pacing ve frame count; reference ve changed configuration ayrı. Aynı koşul ve pairing identity backend'de kayıtlı olmadığı için game kayıtları kontrollü çift kabul edilmez, FPS kazancı türetilmez. Raw JSON tabları ve snapshot export korunur. Gerçek same-context game A/B orchestration, kalıcı autotune run report history, recommendation-level evidence, alternative workspace ve dependency/revalidation açık. Rust solver/ABBA/boot uygulama mantığı değiştirilmedi.

### A05 / D04
TrialBar yalnız revert talep edildiğini bildirir, merkezi registry değildir. 40 yeni TR katalog mesajı. Tüm FIX-01–21 özgün native kabul açık. CO crash guard, SMU, firmware, fan, NV V/F helper, Optimization/ABBA, boot ve restore yazma yolları bu tur yeniden yazılmadı; native fiziksel doğrulama yapılmadı.

## Testler
| Test | Gerçek sonuç |
|---|---|
| İlk bağımsız runner seti | 50/50 PASS |
| Son kaynak bağımsız runner seti | 50/50 PASS (statik + bağımsız native) |
| ABBA classifier ASan + UBSan | PASS, EXIT=0 |
| ABBA classifier GNU LTO / -Werror | PASS, EXIT=0 (Gentoo tam uygulama değil) |
| TR katalog kontrolü | İlk placeholder hatası düzeltildi; son PASS |
| git diff --check | PASS |
| Qt6 Debug / Release configure | FAIL — Qt6Config.cmake yok |
| apt update / install | DNS hatası / paketler bulunamadı |
| Cargo | Yok; Rust build/test/clippy/deny çalıştırılmadı |
| Qt CTest, GUI sanitizer/visual, gerçek shell 20 geçiş | Çalıştırılmadı |
| Gentoo native LTO/PGO, GitHub CI, fiziksel fixture | Çalıştırılmadı |

## Öncelikli kalanlar
A02: tam cache migration, gerçek iki shell ve donanım çağrı sayaçları. A03: Tur23 ortak altı slot dışında typed device/subscriber, Health/stress/SMART ve scheduler. A04: CO start/resume/reset/teardown/destructor non-blocking ve crash recovery. A05: atomik operation registry, scene owner, restore/resource locks. A07/A08: gerçek MainWindow injected sim ve reviewed visual baselines. B03: Tur23 validator native Rust doğrulaması, UUID, overlay ve transaction history. B07: gerçek helper per-key receipts ve ABBA-aware transaction journal. B09: calibration validity/session/rollback. B11: yukarıda belirtilen ölçümlü A/B ve run history. Diğer bütün 48 paket, 118 GUI ID, ek ID ve FIX koşulları matrislerde korunur. Bu teslimat tam kabul değildir. Qt/Rust native doğrulanmadan üretim veya experimental push önerilmez. GitHub push yapılmadı.
