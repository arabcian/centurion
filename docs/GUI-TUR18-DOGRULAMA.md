# Centurion Experimental — Tur 18 kısmi geliştirme ve devir raporu

Temel: `Centurion-experimental-tur17-kismi.zip`; SHA-256 `9020f8ca7415b39aedfe85eed6d33726b765fae7f0d115059d62ee43fc67c7cb`; CRC PASS. Önceki 48 paket / 118 ID matrisleri ve Tur 14 özgün raporu değişmeden korunur. 0 tam kabul, 47 kısmi, D05 doğrulama engelli. Bu tur C04, D04 ve D05 üzerinde çalışıldı. Tüm 48 paketin kaynak çağrı zinciri denetimi tamamlanmadı.

## C04 — PCIe / Hardware Device Tree
- Kaynak denetimi: önceki link delta yalnız boot + BDF + vendor/device ile karşılaştırıyordu; aynı model yeniden takılırsa ölçüm karışabilirdi. PCIe link register okumaları runtime_status öncesindeydi. Eski hierarchy explorer bütün QTreeWidgetItem nesnelerini oluşturuyordu.
- `deviceidentity.h`: gerçek canonical `/sys/devices` altındaki directory dev/inode kimliği; cycle/detached/escape reddi. `systemdiagnostics.cpp`: boot + bus + canonical relative path + dev/inode + PCI vendor/device/subsystem/class/revision veya USB vendor/product ve network ifindex hash; NVMe için sysfs instance. Seri/MAC okunmaz. Kimlik geçicidir; fiziksel UUID veya inode reuse garantisi değildir. Eksik boot/PCI kimliğinde eski ölçüm bağlanmaz. PCI delta ayrıca eski vendor/device doğrulamasını korur. Newly readable or added ayrı gösterilir, kayıp okuma unknown, boot değişince delta yok.
- PCIe config/link okumaları yalnız runtime `active` görüldüğünde yapılır; uyuyan/bilinmeyen aygıtlar unknown. Upstream maksimumu ancak parent PCI adresi ve active durumuyla okunur; yoksa endpoint maksimumu olduğu açıklanır. PCIe nesli ve gerçek GT/s birlikte gösterilir; eksik hız/genişlik unknown. Kimlik/runtime tekrar kontrol edilir; yarışta veri atılır. Aktif durum kontrolü atomik PM referansı değildir, fiziksel dGPU no-wake garantisi verilmez. PCIe bridge kendi link okumaları hâlâ önceki davranıştaki gibi kapsam dışı. S04 dışında diğer tanı araçlarının uyandırma davranışı değiştirilmedi.
- `HierarchyViewModel`: frozen table ve indeks listesi, QTreeView + recursive filter; per-row item oluşturulmaz, görüntüleme sırasında hardware query yok. Seçim ve expansion aynı view instance içinde refresh/reopen boyunca benzersiz key ile korunur; duplicate/empty key restore edilmez. Flat row seçimiyle explorer ilgili cihazı açar; captured columns detail panel, UTC timestamp; filtre sırasında geçici expansion ve selection korunur. Export'ta transient rowKeys temizlenir. Diğer generic hierarchy yolları korunur. Qt runtime testi yapılmadığından UI davranışı kabul edilmedi.
- Kalan: gerçek donanım no-wake, inode reuse/hotplug yarışlarının kesin çözümü, bridge link kapsamı, tüm USB/NVMe/network identity ve detach fixture, diğer inspector deep-links, native Qt visual/model test. GUI-153 ilerleme; 145 paket kapsamı korunur (118 listesinde ayrı satır yok).

## D04 / D05 — Testler
- 13 yeni TR katalog metni. 45/45 runner PASS (44 önceki + yeni deviceidentity native test). Yeni test gerçek geçici filesystem üzerinde Debug/O2/ASan+UBSan: aynı BDF replacement, detached, duplicate alias, escape, cycle, runtime gate. Qt diagnostictools test fixture genişletildi (same-model replacement, suspended, model parent/ambiguous key); çalıştırılmadı.
- İlk static regression turunda eski literal vendor/device assertion başarısız oldu; gerçek kaynakta mevcut vendor/device koşulu ve eski vendor/device equality tekrar açıkça korunarak yeni identity guard ile birleştirildi, static test yeni koşulu da arar. İlk başarısızlık logu saklandı. Son 45 runner PASS; final dialog lifetime değişikliği sonrası iki ilgili static test tekrar PASS.
- Debug ve Release CMake Qt6Config.cmake bulunamadığı için BLOCKED. Apt tekrar denendi; deb.debian.org DNS çözülemiyor, qt6/cargo/rustc paketleri bulunamadı. Qt native GUI/CTest/Rust/Clippy/Gentoo PGO-LTO/visual/uzak CI/fiziksel donanım doğrulaması yapılmadı. GitHub push yok, main/old değişmedi.
- CO Search crash guard, NVIDIA V/F Apply/Reset, SMU ve firmware/Rust helper dosyaları değiştirilmedi; güvenlik kabulü fiziksel olarak doğrulanmadı.

## Envanter
8 mevcut kaynak/test/CI dosyası değişti, 3 yeni kaynak/test dosyası, 0 silinen kaynak.
- `.github/workflows/ci.yml`
- `gui/i18n/centurion_tr.ts`
- `gui/src/diagreport.h`
- `gui/src/diagreportview.cpp`
- `gui/src/diagreportview.h`
- `gui/src/systemdiagnostics.cpp`
- `gui/tests/diagnostictools_test.cpp`
- `gui/tests/gui_completion_static_test.py`
- `gui/src/deviceidentity.h`
- `gui/src/hierarchyviewmodel.h`
- `gui/tests/deviceidentity_native_test.py`

## Kapsam takibi
`GUI-TUR18-48-PAKET.tsv` ve `GUI-TUR18-118-KABUL.tsv` önceki bütün sütunları ve orijinal kabul şartlarını korur. `GUI-TUR18-RUNNERS.tsv` son koşu sonuçlarıdır. Bu teslimat kısmi kaynak paketidir.
