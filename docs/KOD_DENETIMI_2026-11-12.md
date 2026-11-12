# Centurion — 12 Kasım 2026 kod inceleme ve doğrulama raporu

## Kapsam ve doğrulama düzeyi

Kaynak: kullanıcının yüklediği `centurion-main(10).zip`.

Bu sürümün Rust yardımcı araçlarında ve NVIDIA eğri yöneticisinde tespit edilen mantık, sınır koşulu ve hata raporlama sorunlarına yönelik düzeltmeler yapıldı. Orijinal Qt arayüzü, çeviriler, yapılandırmalar ve diğer kaynaklar pakette korunmuştur. Kaynak taraması ve mevcut otomatik testlerin çalıştırılması **tüm programın doğrulandığı** anlamına gelmez. Özellikle sistem düzeyinde yetki, Lenovo firmware/EC, NVIDIA NVAPI/NVML ve Ryzen SMU için ilgili gerçek cihaz üzerinde son kontrol zorunludur.

## Uygulanan değişiklikler

| Alan / dosya | Problem | Düzeltme |
|---|---|---|
| `crates/centurion-helpers/src/iorate.rs` | Diskin pasif yazma bant genişliğine okuma sektörleri de ekleniyordu; bozuk `/sys/block/*/stat` alanları atlanarak indeksler kayabiliyordu; büyük çarpımlar taşabiliyordu. | Yalnız yazma sektörleri hesaba katılıyor, alanlar konum kaydırılmadan ayrıştırılıyor, `u128` ile taşma koruması ve testler eklendi. |
| `crates/centurion-helpers/src/bench.rs` | p99 hesabı bazı tam pencere boylarında bir sonraki sıralı değeri seçiyordu (100 örnekte 100. yerine 99. değer). | `ceil(0.99 * n) - 1` yakın-sıra kuralı ve regresyon testi. |
| `crates/centurion-helpers/src/par.rs` | Paralel satır işleme, tam satır oluşturmayan son elemanları sessizce yok sayabiliyordu. | İşleme öncesi matris boyutu doğrulaması ve hatalı girdi testi. |
| `crates/centurion-helpers/src/calib.rs` | Kalibrasyon makine imzasında RAM miktarları `i64` olarak çıkarılıp mutlak değer alınırken büyük/güvensiz JSON değerlerinde taşma/panic riski. | `u64::abs_diff()` + `u128` karşılaştırma ve sınır testleri. |
| `crates/centurion-helpers/src/bin/centurion-autotune.rs` | `--weights` girdisindeki bozuk, bilinmeyen, tekrarlanan veya sonlu olmayan değerler sessizce atlanıp farklı/varsayılan ağırlıklarla işleme devam edebiliyordu. | Hatalı ağırlıkları açıkça reddeden `Result` ayrıştırıcısı, CLI argüman doğrulaması ve testler. |
| `crates/nvcurve/src/hal/limits.rs` | Güç limitinde doygun çarpma, çok alanlı offset yazımında geç yapılan overflow kontrolü, bazı reset hatalarının başarı gibi görünmesi. | Limit kontrolü ve checked math; her iki alana yazmadan önce bellek değerini doğrulama; P-state sayım, yazım ve geri okuma hatalarının raporlanması. |
| `crates/nvcurve/src/hal/vfcurve.rs` | Okunamayan GPU nokta maskesi/frekans tabanı durumunda doğrulamayı atlayabilen V/F yazımları, güncelliğini yitirmiş tablo üzerine yazma, sürücü başarı döndüğünde gerçek sonucu doğrulamama. | Maskeyi ve negatif delta için frekans tabanını zorunlu doğrulama, gelen başlangıç tablosunu yeniden okuma, yazımdan sonra nokta bazlı driver read-back ve uyuşmazlık kodları. |
| `verify-build.sh` | Tek komutta statik testler, Rust ve GUI derlemesi arasında izlenebilir doğrulama eksikliği. | Sonuçları ve ayrı logları `build-verification/` altında toplayan doğrulama aracı. |
| `tests/test_algorithm_guardrails_static.py` | Yukarıdaki hataların kaynak düzeyinde yeniden oluşmasını önleyen kontroller eksikti. | 6 statik regresyon kontrolü. Bunlar Rust'ı çalıştırmaz. |

## Bu ortamda gerçekten yapılan testler

- **28 Python testi geçti** (`python3 -m unittest discover -s tests -p '*test*.py' -v`). Bu testlerin bir bölümü yalnız kaynak sözleşmesi/statik kontrollerdir.
- **5 GUI Python testi geçti** (`gui/tests`).
- **GUI kaynak yapısı kontrolleri geçti** (çeşitli mevcut statik kontrol betikleri).
- **Shell betiklerinde `bash -n` kontrolü geçti**.
- **Git diff boşluk/hunk kontrolü geçti** (`git diff --check`).
- **Rust `cargo check`, `cargo test`, `cargo fmt`, `cargo clippy` çalıştırılamadı:** `cargo`, `rustc`, `rustfmt`, `clippy` mevcut değil. Paket yöneticisiyle yükleme denemesi DNS/ağ erişimi nedeniyle başarısız oldu.
- **Qt/C++ derlemesi yapılamadı:** CMake çalışıyor ancak Qt6 (>=6.4) geliştirme paketi yok (`Qt6Config.cmake` bulunamadı).
- **Gerçek GPU/CPU/EC testi yapılamadı:** bu ortamda desteklenen Legion donanımı, NVIDIA kapalı kaynak sürücü ve fiziksel fan/firmware/SMU arabirimleri yok.

**Sonuç:** Bu teslim, düzeltilmiş **kaynak kodu** ve geçen kaynak seviyesi regresyon testleridir. Tam derlendiği, gerçek donanımda çalıştığı veya tüm algoritma hatalarının giderildiği bu koşullarda doğrulanamamıştır. Kaynak düzeyi test geçmesi binary runtime garantisi değildir.

## Kullanıcının Gentoo Linux makinesinde zorunlu doğrulama

Centurion README'sine göre en az Rust **1.89**, C++20, CMake **3.19**, Qt **6.4** (Widgets, Network, Svg) gerekiyor. Gentoo'da dağıtımınızın paket/USE seçeneklerine göre Rust/Cargo, C++ derleyici, CMake, `dev-qt/qtbase` (Qt6 Widgets, Network), `dev-qt/qtsvg` ve gerektiğinde `dev-qt/qttools` kurulmalıdır. Paket adlarını ve SLOT/USE seçeneklerini sisteminizde `emerge --search` ile doğrulayın.

İndirdiğiniz arşivi bir dizine açıp proje kökünde:

```bash
./verify-build.sh
```

komutunu çalıştırın. Kontrol logları `build-verification/` klasörüne kaydedilir; başarısız adımlarda betik **sıfır olmayan** durum koduyla çıkar. Derleme aşamalarının başarısızlığı giderilmeden `install.sh` ile sisteme kurmayın. Donanım denemelerinde önce yalnızca okuma/dry-run ve düşük riskli geçici değişiklikleri deneyin; NV eğrisi, fan/EC, BIOS ve undervolt kalıcı ayarlarına geçmeden önce mevcut profilleri yedekleyin. `DISCLAIMER.md` ve `docs/HANDBOOK.md` dosyalarını izleyin.

## Bilinen kısıtlamalar

Bu inceleme, 100 bin satır ölçeğindeki depoda formal verification veya bütün yolların fonksiyonel simülasyonunu kapsamaz. Düşük seviyeli sürücü davranışları, yarış koşulları ve üçüncü taraf sürücü limitlerinin tamamı gerçek cihaz testi olmadan güvenceye alınamaz. NVIDIA read-back kontrolünün daha katı olması bazı daha önce sessizce kabul edilen sürücü davranışlarını artık **hata** olarak raporlayabilir; bu kasıtlı güvenlik kararıdır.
