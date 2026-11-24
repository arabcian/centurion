# Tur 41 — geliştirme, derin kaynak denetimi ve doğrulama

Bu teslimat Tur40 üzerine gerçek kaynak değişikliği içerir. Tam proje denetimi veya native kabul değildir. Kullanıcının onay beklemeden ilerleme talebine uygun olarak A04/A05/B07/B08/B09/B11/D04/X05 üzerinde öncelikli işlem ve ölçüm güvenliği düzeltildi. D05 doğrulama engeli giderilemedi. Fiziksel donanıma yazma, benchmark, firmware ayarı, remote push yapılmadı. 

## İncelenen ve düzeltilen hata yolları

- Root optimizasyon state yüklemesi: bozuk/okunamayan veya yeni sürüm kayıtların boş state olarak değerlendirilmesi engellendi; state yazma üst sınırı okuyucu ile uyumlu. Yan etkisi olabilecek başarısız sysfs yazmalarında orijinal korunur.
- Restore scope: eksik, boş veya bilinmeyen restore_keys bütün ayarların restorasyonuna düşmez. Geri yükleme sonucu tüm batch sonrası readback ile sınıflanır; başarısız/deferred/policy-skipped kayıtlar korunur. Boot-defaults öncesi tamamlanmamış restore işlemi yeni apply'i engeller.
- Game layer: eksik restore sonrasında pre-game değerleri kaybolmaz; normal mutasyon öncesi stale layer kontrolü, wireless master root lock, pressure guard lock sonrası karşılaştırması. Tam cross-process registry yok; ABA tespit garantisi yok.
- Scoped storage: NVMe queue/F2FS/zram/genel disk queue için boot-local canonical path, inode, parent inode, NVMe WWID ve açık FD metadata eşleşmesi. Legacy alias çakışması veya eski kayıtta stamp yoksa yazma/restore reddi; kaydı silme yok. Bu, fiziksel aygıtın kusursuz veya cross-boot tanımı değildir. Slot yeniden sıralanırsa restore reddedilebilir; kernel/hardware hotplug ile yarış bütünüyle önlenmiş değildir. Calibration Journal ve SATA/controller vb. kapsam bu yazma yoluna henüz geçirilmemiştir.
- PCI latency status: runtime active bilinmiyorsa okunmaz; GPU ağacı hariç; hiç okunabilir aktif hedef yoksa tuned gösterilmez. Bu bir no-wake fiziksel ölçümü değildir.
- GUI: terminal exit beklenir, timeout geç success olarak yorumlanmaz, skipped/partial/missing/mismatch draft korunur; per-key receipt, pending journal eviction guard, boş array index guard; TLP provenance candidate olarak gösterilir.
- Calibration journal: malformed root journal artık fresh state değildir; restore readback doğrulaması, eksik kayıt korunması; normal run restore hatasında devam etmez. Bazı recovery/teardown yolları, cross-process restore ownership ve gerçek device identity hâlâ açık.
- ABBA: eksik ABBA blokları, fazı kaybolmuş run ve duplicate slotlar doğrulama kanıtı olamaz; en az 3 tam blok, planlanan veya sequential stop noktasına kadar tam kapsam, OOM yok, restoration success ve boot-bound schema v2 istenir. Legacy schema yalnız geçmiş görünümü. Ölçülen objective coverage eşitliği ve context drift revalidation halen açık; boot ID tek başına yeterli same-context proof değildir.
- Calibration signature: backup hatası propagate edilir; aşırı büyük kayıt eski imzayı bozmadan reddedilir. Tüm import/backup mutation registry çözülmedi.

## Çalıştırılan doğrulamalar

| İşlem | Sonuç |
|---|---|
| Kaynak SHA-256 | T40 devirle birebir eşleşti |
| apt update | exit 0, fakat DNS çözümleme hataları; paket listeleri yenilenemedi |
| apt install Qt6/Cargo/Rust/bubblewrap | exit 100, paketler bulunamadı |
| Qt Debug configure | exit 1, Qt6Config.cmake yok |
| Qt Release configure | exit 1, Qt6Config.cmake yok |
| cargo build/test/clippy/deny | her biri exit 127 (cargo yok) |
| 70 önceki bağımsız runner | 70/70 exit 0, kaynak geliştirilirken çalıştırıldı; final full build değildir |
| Son kaynak üzerinde hedefli runner | 10/10 exit 0; C++ bağımsız primitive testleri ve static source contracts |
| git diff --check | exit 0 |
| Yeni Rust fixture | state decode, restore scope, storage incarnation, ABBA incomplete/duplicate; yazıldı, çalıştırılmadı |
| Qt tunejournal fixture | pending eviction; yazıldı, çalıştırılmadı |
| Tam Qt/Rust native, Qt CTest, CI remote, Gentoo PGO/LTO, visual / fiziksel | çalıştırılmadı |

TSV kayıtları ve ham loglar ayrı kanıt ZIP'inde. CI adımı kaynakta eklendi, CI çalıştırılmış sayılmadı. 70 runner sweep son değişikliklerin bir kısmından önce gerçekleşti; 10 final targeted check bu yüzden ayrı gösterildi. Statik test kaynak sözleşmesini kontrol eder, Qt/Rust çalıştırdığını ispatlamaz. Kaynak teslimatı derleme garantisi vermez.

## Açık ve riskli yollar / kabul

0/48 tam kabul; 47 kısmi, D05 engelli. B03 multi-GPU, A03 kalan subscription, A07/A08 injection/baseline ve diğer tüm özgün paket kriterleri açık. Önceki T40 ve T36 kabul koşullarını silme. Kısmi source audit bütün programın derin denetiminin tamamlandığını göstermez. Öncelik native Qt/Rust ortamı, compile/clippy/CTest, scoped restore/readback fixture, calibration journal identity ve process lock, tam ABBA coverage, ardından bütün kalan iş paketleri. Donanıma otomatik yazma veya remote push yok.
