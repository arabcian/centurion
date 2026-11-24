# Centurion Experimental — Tur 31 kısmi doğrulama ve devir

Temel: `Centurion-experimental-tur30-kismi.zip` (SHA-256 `683cfc143d8be5c73e86f462e83938116553d1a02830ca0593c0f4734e437dd3`). Tur29 özel 11 istek ve build düzeltmesi korunur. Kabul: **0/48 tam, 47 kısmi, D05 engelli.** Özgün kapsam daraltılmadı.

## Gerçek kaynak değişiklikleri

- C04: PCIe upstream gerçek current/max link ve runtime, UTC; canonical parent alias+boot-scoped identity revalidation, aktif olmayan port okunmaz; endpoint fallback korunur.
- C06: L3 reciprocal CPU/cache declaration tree; eksik/çelişkili bildirimler unknown; bounded capture ve graph. Mevcut NUMA/SMBIOS/zram tabloları korunur.
- C07: Mevcut acpi_event generic-netlink alıcısı üzerinden opt-in canlı 512 satırlık görüntü; gerçek sequence/class/busId/type/data, capture gaps, benzersiz frozen inventory candidate deep link. Wire parser atomik reject, kernel sender ve truncated datagram guard.
- C08: Battery PDO maximum_power, variable/fixed/PPS ayrımı, power-limited PPS max unknown; Type-C power_operation_mode, role/partner/mode stability guard; mevcut frozen graph ve bağımsız supplies.
- C09: Memory free -b structured UINT64 bytes adapter ve mevcut free -h raw korunur; S06 gerçek modinfo parametre tablosu; bounded streaming diag output, C locale ve malformed fallback.
- D04: 45 yeni TR girdisi (önceki 44 + kernel sequence).
- D05: Qt Debug/Release configure denendi; yeni independent protocol fixture, Qt test kaynakları ve CI tanımı.

ACPI alıcısı mevcut Spectrum Fn+Space signal imzasını korur; wire format ve socket sender/truncation kontrolleri değiştiği için gerçek Spectrum donanım regresyonu açık. Viewer yalnız açıkken çalışır; kayıp sayacı gözlenen gap sayısıdır, gerçek kayıp olay sayısı değildir. Gerçek netlink socket aboneliği çalıştırılmadı. L3 ağacı karşılıklı cache bildirimi olup fiziksel CCD/NUMA kanıtı değildir. USB-PD operating mode mevcut moddur, negotiated voltage/current contract değildir. S06 modinfo metadata izin/yazma ya da running value kanıtı değildir. Hiçbir yeni hardware write eklenmedi.

USB-PD kaynak doğrulaması: https://github.com/torvalds/linux/blob/master/Documentation/ABI/testing/sysfs-class-usb_power_delivery ; https://github.com/torvalds/linux/blob/master/Documentation/ABI/testing/sysfs-class-typec . ACPI generic netlink yapı/command: kernel drivers/acpi/event.c (https://cos.googlesource.com/third_party/kernel/+/refs/heads/contrib/main-R101-cos-5.15-spr-emr-support-2/drivers/acpi/event.c).

## Testler

| Test | Sonuç |
|---|---|
| Tur30 kaynak ZIP CRC | PASS |
| Tur30 tam baseline runner | Bu tur ayrı çalıştırılmadı; önceki 57/57 raporu |
| Tur31 ilk tam runner | 58/58 PASS |
| Son etkilenen rerun | 4/4 PASS |
| Yeni native protocol fixture | O2, LTO -Werror, ASan+UBSan PASS; 30000 malformed input |
| Qt6 Debug/Release configure | FAIL: Qt6Config.cmake yok |
| Apt update | DNS çözümleme başarısız |
| Rust build/test/clippy/deny | Cargo yok, çalıştırılmadı |
| Qt CTest (diagnostictools, structuredtools dahil) | Çalıştırılmadı |
| Gerçek GUI/ACPI socket, Gentoo PGO/LTO, fiziksel donanım, CI | Çalıştırılmadı |

Bağımsız runner'lar statik ve native C++ karışımıdır. Qt/Rust uygulamasının derlendiği veya fiziksel güvenlik kabulü anlamına gelmez. Yeni GUI ve ACPI bağlantısı native derleme gerektirir.

## Kalan özgün kapsam

- C04: Diğer inspector deep links, bridge AER/NUMA kapsamı, bütün hotplug fixture, fiziksel no-wake ve Qt test.
- C06: Tam NUMA/CCD/SMBIOS ilişkisi, cgroup scope, swap alias ve stable device identity, CPU hotplug atomik snapshot, Qt/fiziksel doğrulama.
- C07: Kalıcı cross-boot arşiv, kesin device lifecycle binding ve WMI GUID event semantics; family resolve hâlâ 0.5s bounded sync, gerçek socket/Qt/hardware doğrulama.
- C08: Gerçek bildirilen negotiated voltage/current contract kaynakları, AVS/EPR schema, port supply binding, Qt/fiziksel fixture; mode tek başına contract değil.
- C09: Diğer uygun raw tool adapter, Qt clipboard/export/filter davranışı, tüm tool capture cap kapsamı.
- D04: FIX-01–21 native kabul ve iki shell TR/EN/DPI layout.
- D05: Qt6/Cargo eksik, native Qt/Rust/CTest/CI/Gentoo PGO/LTO/fiziksel doğrulama.
- A07 gerçek MainWindow fail-closed simulation ve A08 gerçek native görsel regression bu tur tamamlanmadı; izole simulation korunur.
- Tüm diğer 48 paket / 118 GUI / ek ID / FIX / Wanted kriterleri önceki haliyle açık. Orijinal hücreler aynen korunmuştur.
- GitHub push yok.
