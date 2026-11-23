# Centurion — UI Title Case denetimi (24 Kasım 2026)

## Amaç

Bu paket, **Centurion_Health_SystemTools_Refined_20261124.zip** sürümünü temel alır. Health Center, System Tools, teşhis raporları ve diğer Qt ekranlarında görünür İngilizce başlıkların yazımını tutarlı hale getirir. Donanım kontrolü, komutlar, kernel işlemleri, ağ ve firmware algoritmalarına müdahale edilmedi.

## Yapılanlar

- **Health:** `System Overview`, `Events & Alerts`, `Boot History`, `Curve Stability`, `Kernel Log`, `Kernel Parameters`, `Sleep & Wake`, `Battery Use`, `dGPU Power`, `Drive Health`, `Network Protection`, `System Tools`.
- **Semptom kartları:** `GPU & Display`, `Storage`, `Wi-Fi & Bluetooth`, `Power & Sleep`, `CPU & Stability`, `Crashes & Kernel`.
- **System Tools:** `CPU & Memory`, `Graphics & Display`, `Network & Wireless`, `Power & Thermal`, `Devices & Buses`, `Firmware & Kernel`, `System & Configuration`.
- **Araç isimleri:** `Hardware Snapshot`, `NVIDIA Module Parameters`, `PCIe Detail`, `SMART Details`, `Firmware Updates`, `Configuration Files`, `Network Tuning`, `Storage Tuning`, `Kernel Tuning`.
- **Diğer UI etiketleri:** İlgili tanılama raporlarının tablo/bölüm adları, Health başlıkları, görsel sekme başlıkları ve pek çok pencere/diyalog başlığı düzenlendi.
- **Türkçe:** `gui/i18n/centurion_tr.ts` içindeki değişen kaynak metinler güncellendi, mevcut Türkçe çeviriler korundu, eksik bazı Health başlıklarının Türkçe karşılıkları eklendi. Türkçe için İngilizce başlık büyük/küçük harf kuralı körü körüne uygulanmadı.

## Uyumluluk notu — ÖNEMLİ

Bazı sistem araçlarının eski görünür adı, **kalıcı `toolKey` kimliğiydi**. `systools.cpp` içindeki `stableToolKey()` işlevi yeni İngilizce görünür adı eski kimliğe eşler. Örneğin:

| Görünen başlık | Eski (korunan) kimlik |
|---|---|
| Hardware Snapshot | `Hardware snapshot` |
| NVIDIA Module Parameters | `NVIDIA module parameters` |
| PCIe Detail | `PCIe detail` |
| SMART Details | `SMART details` |
| Firmware Updates | `Firmware updates` |

`Configuration files` ve Optimize kaynaklı `Network tuning`, `Storage tuning`, `Kernel tuning` kimlikleri de korunmuştur. Bu sayede daha önce yıldızlanan araçlar, son kullanılanlar, H18 ilişkili araç bağlantıları ve `actionBar` işlevleri eski kimliklerle çalışmaya devam eder.

## Kontroller ve sınırlamalar

- `python3 gui/tests/title_case_static_test.py`
- `python3 gui/tests/toolcatalog_static_test.py`
- `python3 gui/tests/health_catalog_redesign_static_test.py`
- `python3 gui/tests/studio_v4_static_test.py`
- `python3 gui/tests/translation_tr_test.py`
- `cmake -S gui -B build-ui-test -G Ninja -DCMAKE_BUILD_TYPE=Debug`
- `cmake --build build-ui-test -j "$(nproc)"`

Statik kontroller Türkçe TS/XML, başlık kaynakları ve değişmeyen kimlikleri denetler. **Bu ortamda Qt6 geliştirme paketleri bulunmadığından GUI derlemesi doğrulanamadı**; Gentoo'da gerçek derleme ve ekran kontrolü gereklidir.

Değiştirilmeyen alanlar: `crates/`, donanım güvenlik kuralları, `sysfs`/ACPI anahtarları, JSON kayıtları, araç işlevleri ve kullanıcı ayar dosyasındaki kalıcı kimlikler.
