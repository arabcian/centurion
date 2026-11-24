# Centurion Experimental — Tur 40 doğrulama

**KISMİ teslimat: 0/48 tam kabul, 47 kısmi, D05 engelli.** 118 GUI ID ve ek ID, FIX-01–21, WANTED 1–11 ve BUILD açık. T39 SHA-256 `40a7542d2ae684bf2479ca0d90e2a4c3d28599bf070b6da6b530b44186be17b7` ve CRC doğrulandı. Raporlar hariç 18 mevcut dosya değişti, 3 yeni kaynak/test dosyası, 0 silinen. Kaynak patch temiz T39 ağacına uygulandı, byte eşitliği PASS.

## Gerçek değişiklikler ve sınırlar

- B03 NVAPI `GetBusId` çağrı ID'si NVIDIA resmi interface başlığından (`https://github.com/NVIDIA/nvapi/blob/main/nvapi_interface.h`) doğrulandı. Envanterde NVAPI/NVML sıra varsayımı kaldırıldı; multi-GPU için yalnız tüm PCI bus'lar eşsiz, sayılar eşit, isim/UUID/BDF ve iki yakalama tutarlı olduğunda eşleme üretilir. BDF NVML alanlarıyla doğrulanır. Bu bus-only envanter kanıtı tam cross-API domain/slot/function doğrulaması değildir. Tek-GPU önceki strict identity schema korunur. CLI `gpus` tam NVML BDF ve kanıt türünü gösterir. Belirsiz kimlikte UUID/BDF yok. V/F readback NVML offsetlerini yalnız önce/sonra aynı strict tek-GPU identity varsa ilişkilendirir.
- B03: Named/edited/CLI/boot autoload karışık CT+NVML profile Apply için tek fiziksel NVIDIA GPU şartı ve yazma öncesi handle kontrolü eklendi. Çoklu GPU Apply ve snapshot restore fail-closed; bu bir işlev kısıtıdır. Snapshot metadata artık indeksle tahmin edilen UUID yazmaz; yeni snapshot için verified UUID yoksa restore reddedilir. Eski name-only snapshot uyumu tek GPU ile sınırlı; eski hatalı UUID'ler geriye dönük doğrulanmış değil. Bağımsız acil reset yolu ve mevcut flatten/rollback algoritması değiştirilmedi; diğer multi-GPU doğrudan setter/reset/sensor yolları bu turda tam eşlenmedi, hotplug TOCTOU bütünüyle kapanmadı. Hiçbir fiziksel yazma testi yapılmadı.
- B03: `ApplyOutcome.point_plan` gerçek checked profile transaction'ın GPU point delta/base/projected kHz ve µV değerlerini taşıyor. Root helper request-ID ile atomik sonuçta planı ekliyor. GUI, device+layout+integer+arithmetic+complete coverage eşleşmesi olmadan kesin plan karşılaştırması yapmaz. Journal ve GUI exact kHz karşılaştırır, plan olmayan eski helper flatten'da belirsiz kalır; diğer eski helper karşılaştırması korunur. Masked/eksik plan tam kabul edilmez. Projected frekans gerçek frekans veya stabilite kanıtı değildir; kilitler not_verified. Native Rust ve Qt entegrasyon fixture'ları ekli fakat çalıştırılmadı. Yeni GUI ve helper birlikte kullanılmalı.
- A03: hwmon path için dev/inode ve canonical path ile scan/read öncesi/sonrası replacement kontrolü, cache'de bilinen cihaz değişiminde yeniden tarama, cached directory ve hwmon yaşında CLOCK_BOOTTIME. Salt metadata sorgusu eklenir; performans/fiziksel no-wake ölçülmedi. Bu best-effort TOCTOU azaltımıdır; stat/kanonik kontrol atomik FD koruması veya tüm cache-delivery device lifetime garantisi değildir. Diğer tüketiciler/scheduler açık.
- D04: 3 yeni TR metni. Önceki T39 simülasyon, CO/Scene, SMART, firmware/fan, ABBA, boot guard değişiklikleri korunur. FIX özgün bireysel kriterleri temin edilmedi.

## Test sonuçları

| Kontrol | Sonuç |
|---|---|
| 70 bağımsız runner (son Rust/contract düzenlemeleri öncesi sweep) | 70 exit0 |
| Final hedefli 3 runner: yeni primitive, NVIDIA source contract (18 test), TR catalogue (5 test) | 3 exit0 |
| Yeni primitive GNU LTO `-Werror` | exit0 |
| Yeni primitive ASan+UBSan | exit0 |
| Patch temiz uygulama ve byte eşitliği | PASS |
| apt-get update | exit0 ancak deb.debian.org DNS failure |
| apt-get install Qt6/Cargo/bubblewrap | exit100, paketler bulunamadı |
| Qt Debug / Release configure | exit1, Qt6Config.cmake yok |
| cargo build --workspace --locked | exit127, cargo yok |
| Qt build/CTest/Rust tests/Clippy/deny/Gentoo PGO/LTO/CI/visual/physical | ÇALIŞTIRILMADI |
| GitHub push | YAPILMADI |

Yeni bağımsız C++ test gerçek `curveplan.h` ve `sensorincarnation.h` kullanır: kHz plan aritmetik/overflow/missing/duplicate/reorder/voltage mismatch ve geçici dizinlerde symlink/replacement/detach. Qt JSON bağlama, Rust PCI fixture ve gerçek GPU/NVML/NVAPI çalışması değildir. Sweep ve final hedefli komut/exit/log kanıt paketinde.

## Sonraki öncelik

D05 native ortamı; B07 gerçek per-device optimization transaction/restore/provenance ve B09/B11 calibration + kontrollü A/B; A03 kalan ortak abonelikler, A04/A05 ownership, A07/A08 tam typed injection ve gerçek baseline. B03 tam çoklu aygıt write/restore, lock ve trial sahipliği açık; T40 fail-closed davranışı doğrulanmadan genişletilmemeli.
