# Centurion Experimental — Tur 20 kısmi geliştirme ve devir raporu

Temel: `Centurion-experimental-tur19-kismi.zip`; SHA-256 `0bf4e7d1329618cd1d0f670871b2a403844b72e403320d966fd1e5e57984d8d1`; ZIP CRC PASS. Tur 14 özgün devir raporu ve Tur 15–19 matrisleri korunmuştur. 48 paket, 118 orijinal GUI ID; 0 tam kabul, 47 kısmi, D05 engelli. Tur 20 X05, C06, D04, D05 üzerinde çalışıldı. Bütün 48 paketin kaynak çağrı zinciri denetimi tamamlanmadı; kapsam daraltılmadı.

## X05 / GUI-109 — Unit-aware canonical integer editors
- Kaynak denetimi: `integerunits.h` yalnızca byte parser içeriyordu. `Int64SpinBox` iki dirty_bytes satırına birim izni veriyordu; zaman satırları canonical tamsayı olarak düzenleniyordu. `tune.rs` 41 seçili anahtarın birimi ve türü için incelendi; isim son ekinden tahmin edilmiyor (BT 1.25 ms ticks ve LLC epoch gibi özel birimler kapsam dışında).
- 41 açıkça eşlenmiş optimizasyon zaman satırında ns, us, µs/μs (GUI), ms, s girişi. Bare integer mevcut canonical birim; suffix kullanıldığında exact decimal dönüşüm. `1.5 ms` → 1500000 ns; `1000 ns` → 1 us; `1 ns` → us dönüşümü reddedilir. Negatif sentinel yalnızca bare integer. Signed int64 sınırı, decimal shift ve bölünebilirlik kontrolü; ara nanosaniye değeri oluşturulmaz. Float/yuvarlama yok. Giriş 128 byte sınırı, bozuk/taşan birim reddi. Byte davranışı korunur. Range kontrolü, incomplete prefix, focus/Enter commit ve açıklama; `editorValue`/preset JSON ve helper canonical tamsayı yolu korunur. Helper aralık ve güvenlik doğrulaması değiştirilmedi. Yeni bir donanım yazma yolu yok.
- Native C++ Debug/O2/ASan+UBSan: 480000 zaman dönüşümü/boundary kontrolü ve önceki 200000 byte roundtrip/her mod PASS. Gerçek Qt editor testleri genişletildi (µs, exact fraction, negative sentinel, INT64_MAX JSON, range rejection, partial input, byte mode), çalıştırılmadı. Full Qt keyboard/focus/Apply davranış doğrulaması ve GUI-101 service apply receipt, OpenRC/systemd/permission fixtures açık. Diğer canonical birimler (ör. centiseconds/BT ticks) özel doğrulama gerektirir. X05 tam kabul yok.

## C06 — Memory Architecture View
- Kaynak denetimi: `/proc/meminfo` sayıları unit doğrulamasız double ile dönüştürülüyordu; duplicate alanlar son değerle eziliyordu. zram mm_stat başarısız parse sıfır olabiliyordu. Sadece disksize>0 olması zram'ın swap olarak aktif kabul edilmesine ve zswap ile çift sıkıştırma uyarısına yol açıyordu.
- `memoryfields.h` gerçek bounded parser: exact kB→byte unsigned integer, duplicate/malformed/missing/overflow unknown; RAM ve swap alanları ayrı, subtraction yalnızca geçerli çiftlerde. zram stored/compressed/resident ayrı, ratio unknown if compressed=0/missing, küçük değerler 0 GiB'ye yuvarlanmaz. `/proc` toplamları fiziksel DIMM veya cgroup limitleri olarak sunulmaz.
- `systemdiagnostics.cpp` mevcut S10 raporuna Active Swap Entries tablosu (KiB), kernel-reported type ve nominal canonical /dev/zramN ayrımı eklendi. Diğer path'ler gizlilik için gösterilmez; alias çözülmez, hiçbir aygıt açılmaz. zswap/zram önerisi yalnızca geçerli swap listesinde canonical zram adı ve karşılık gelen sysfs disksize>0 kanıtı varsa açılır. Gerçek çift sıkıştırma ölçülmüş gibi gösterilmez. Eski kullanıcı onaylı fix korunmuştur, otomatik çalışmaz. Missing/truncated snapshots unknown; bağımsız snapshot'lar arasında yarış olabilir. Kernel adı + sysfs eşlemesi fiziksel kimlik ispatı değildir. Mevcut CPU/NUMA/cache/SMBIOS tabloları korunmuştur.
- Native C++ Debug/O2/ASan+UBSan: 50000 rastgele/malformed ve exact roundtrip fixture/her mod, duplicate/missing/bad unit/overflow, swap header/used>size/priority ve zram testleri PASS. Qt diagnostictools fixture aktif zram, yalnız yapılandırılmış zram, bozuk swap, yanlış RAM unit durumlarını kapsayacak şekilde genişletildi; çalıştırılmadı. C06 architecture graph, doğrulanmış CCD/online/NUMA ve container scope fixture açık; GUI-154 orijinal paket kapsamındadır, 118 ID matrisinde ayrı satır yoktur. Tam kabul yok.

## D04 / D05
- 16 yeni Türkçe katalog girdisi (Int64SpinBox 1, systemdiagnostics 15); placeholder ve katalog testi PASS.
- Önceki 46 runner + yeni `memoryfields_native_test.py`: 47/47 PASS. Son GUI kaynak/katalog değişikliklerinden sonra `translation_tr_test.py`, `advanced_17_static_test.py`, `test_health_diagnostics_contract.py` tekrar PASS. Bunlar tam Qt davranış testinin yerine geçmez. CI workflow yeni memoryfields ve mevcut integerunits native runner'ını içerir; uzak CI çalıştırılmadı.
- Apt update: deb.debian.org DNS hatası; Qt6/cargo/rustc paketleri bulunamadı. Debug/Release CMake Qt6Config.cmake bulunamadığı için BLOCKED; cargo test BLOCKED. Native Qt/CTest, Rust/Clippy, Gentoo PGO-LTO, görsel ve gerçek donanım doğrulaması yapılmadı.
- CO Search crash guard, NVIDIA V/F Apply/Reset, SMU, firmware ve Rust helper kaynakları değiştirilmedi. GitHub push yok. Main/old branch değişmedi.

## Kaynak değişiklik envanteri
10 mevcut kaynak/test/CI dosyası değişti; 2 yeni kaynak/test dosyası, 0 silinen dosya.
- `.github/workflows/ci.yml`
- `gui/i18n/centurion_tr.ts`
- `gui/src/int64spinbox.cpp`
- `gui/src/int64spinbox.h`
- `gui/src/integerunits.h`
- `gui/src/optimizetab.cpp`
- `gui/src/systemdiagnostics.cpp`
- `gui/tests/diagnostictools_test.cpp`
- `gui/tests/int64spinbox_test.cpp`
- `gui/tests/integerunits_native_test.py`
- `gui/src/memoryfields.h`
- `gui/tests/memoryfields_native_test.py`

## Kalan kapsam
`GUI-TUR20-48-PAKET.tsv` ve `GUI-TUR20-118-KABUL.tsv` bütün özgün koşulları, önceki tur ilerlemelerini ve açık işleri korur. `GUI-TUR20-RUNNERS.tsv` gerçek test sonuçları. Hiçbir paket tam kabul edilmedi. A01–A08, B03/B07/B11, C01/C02/C10, D01–D03 ve diğer açık işler özgün kapsamıyla devam etmektedir. Bu teslimat kısmi kaynak paketidir, tamamlanmış release değildir.
