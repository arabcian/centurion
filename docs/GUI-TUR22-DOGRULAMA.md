# Centurion Experimental — Tur 22 doğrulama ve kalanlar

Temel: `Centurion-experimental-tur21-kismi.zip` — SHA-256 `70ba4c33c1fef5314c7e650a1f4bbd277e7903e6dd0fb05666dea250d0c3b0b7`. Önceki tüm matrisler ve Tur 14 özgün devir raporu korunmuştur.

## Gerçek uygulama
- C07 / ek özgün GUI-155: S11 ACPI/WMI ortak envanteri, GUID ve sayısal instance suffix biçim kontrolü, yinelenen GUID sayısı, kopuk aygıt ve çözümlenemeyen sürücü ayrımı. ACPI ve WMI ağacı için mevcut HierarchyViewModel, ayrı seçim/genişleme state; salt okunur captured snapshot.
- S11 mevcut savedKernel() kaynağını kullanır; 4000 kayıt sınırı, 256 en yeni eşleşen kayıt ve olay başına 16 görüntülenen aday; tam boot UUID zorunlu, kısa eski boot prefix dışlanır; boot/seq tekrarları ayrılır. Exact instance, GUID/mixed identifier, driver prefix ve class-only ayrımı; güncel candidate ve driver sütunları, gerçek kaynak zaman/seq/mesaj. Mevcut report araması ve export kullanılır. Aday ilişkisi tarihsel binding, gerçek ACPI bildirimi veya nedensellik kanıtı değildir. Yeni tarama/polling veya method/EC/WMI execute eklenmedi.
- ACPI tablo başlıkları korunur: dinamik dizin dahil erişilemeyen dizin ve kesilme açıklaması; symlink dizin/table atlanır; bildirilen uzunluk dosya boyutu veya checksum doğrulaması değildir.
- D04: 31 yeni TR girdisi. D05: native runner CI'ya ekli.

## Test ve sınırlar
- 49/49 runner PASS (48 önceki + yeni native). Tam runner çalıştırması son Qt model state ve çeviri düzenlemeleriyle örtüşmüştür; ilgili testler ayrıca çalıştırıldı, final-runners.txt ve logları ekte. Qt fixture gerçek geçici dosya sistemi üzerinde test etmek üzere eklendi, çalıştırılamadı.
- acpievidence: Debug, O2, ASan/UBSan; strict GUID, exact token, duplicate/mixed, boot prefix, candidate cap, 30.000 malformed/random fixture her modda PASS.
- Qt6 Debug/Release CMake configure BLOCKED: Qt6Config.cmake yok. Cargo test BLOCKED: cargo bulunamadı. apt paket deposu DNS erişim hatası ve paket bulunamadı. Gentoo LTO/PGO, gerçek Qt GUI, Rust, CI ve fiziksel donanım doğrulanmadı.
- Bu turda donanım yazma/helper/CO/NVIDIA güvenlik yolları değiştirilmedi. Değişen kaynak/test/CI dosyaları: 7; yeni: 2; silinen: 0. Main/old/experimental branch push yok.

## Kalan kapsam
48 paket ve 118 orijinal GUI ID satırı korunur; GUI-155 özgün 48 paket içindeki ek ID'dir, 118 satıra yeni/mükerrer ID eklenmez. 0 tam kabul, 47 kısmi, D05 doğrulama engelli. C07 gerçek ACPI netlink olay geçmişi, doğrudan bağlantılar, permission/detached/hotplug native Qt ve fiziksel testleri açık. Envanter ve kernel logu aynı anın atomik snapshot'ı değildir; sysfs erişimi ve hotplug yarışı mümkün. A01–A10, B01–B11, C01–C13, D01–D05 ve X01–X09 kapsamındaki önceki tüm açık alt işlevler özgün matrislerde korunmuştur; tüm kaynak çağrı zinciri denetimi tamamlanmamıştır.

## Değişen dosyalar
- `.github/workflows/ci.yml`
- `gui/i18n/centurion_tr.ts`
- `gui/src/diagreportview.cpp`
- `gui/src/diagreportview.h`
- `gui/src/systemdiagnostics.cpp`
- `gui/src/systemdiagnostics.h`
- `gui/tests/diagnostictools_test.cpp`
- `gui/src/acpievidence.h`
- `gui/tests/acpievidence_native_test.py`
