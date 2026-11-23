# Health Center / System Tools arayüz denemesi — 2026-11-23

Bu paket **23 Kasım 2026 20:39:11** tarihli kullanıcı kaynağı üzerinde yapılan **gerçek Qt/C++ kod değişikliklerini** içerir; görsel illüstrasyon veya uygulamadan alınmış ekran görüntüsü değildir.

## Neler değişti?

- System Tools: belirgin başlık ve semptom/hata kodu arama alanı; `Ctrl+Shift+F` ile hızlı odak.
- System Tools: Health Overview'ün **son kayıtlı verisinden** dört durum kutusu (müdahale, inceleme, normal, bilinmeyen). Veri yokken sayılar uydurulmaz, `—` gösterilir. Her kutu Overview'ü açar.
- System Tools: mevcut **H18 Guided Triage** kodunu gerçekten başlatan altı semptom kartı (GPU, disk, Wi-Fi/BT, uyku, CPU/CO, çökmeler).
- System Tools: mevcut favoriler başta olmak üzere dört hızlı erişim kartı; tüm araçlarda ad, açıklama, katalog meta verilerinden uyarılar ve ayrı favori yıldızı.
- System Tools: filtreler (tümü, bildirilen özel uyarısı olmayanlar, ayar değiştirebilenler, son kullanılanlar, favoriler), donanım kategorisi seçimi ve açılır/kapanır alanlar.
- System Tools: en fazla iki sütunlu araç listesi, pencere genişliğine göre uyarlanabilen yerleşim, kategorilerin açık/kapalı durumunu hatırlama.
- Health Overview: "Browse System Tools" bağlantısı.
- Gezinme: System Tools sol menü maddesine yeniden basıldığında son araçta takılma yerine araç dizinine dönüş; favori/filtre seçimlerinde eski Qt bileşenlerini güvenli silme.

## Korunan davranışlar

- Sisteme müdahale eden tanı/ayar uygulamalarının hiçbirinin iş mantığı değiştirilmedi.
- Önceki sayfalar, toolKey eşlemeleri ve System Tools araçları korunmuştur.
- Araçların orijinal `LazyWidget` davranışı sürer: katalog açılınca hepsi çalıştırılmaz. **Kullanıcı bir aracı açarsa**, o aracın mevcut davranışı geçerlidir.
- Uyarılar `toolcatalog` meta verisine dayanır. "No special warnings" yazısı **mutlak güvenlik garantisi değildir**.
- İstatistikler son taramada gerçekten kaydedilen Overview verilerini gösterir; taranmamış bileşenler normal sayılmaz.

## Testler / dürüst doğrulama durumu

Bu ortamda aşağıdaki kaynak tabanlı statik testler çalıştırıldı ve geçti:

```
python3 gui/tests/health_catalog_redesign_static_test.py
python3 gui/tests/health_ui_static_test.py
python3 gui/tests/toolcatalog_static_test.py
python3 gui/tests/experimental_ui_static_test.py
python3 gui/tests/studio_v4_static_test.py
python3 gui/tests/experimental_layout_static_test.py
```

**Tam GUI derlemesi yapılamadı:** çalıştırma ortamında Qt6 Widgets 6.4+ geliştirme paketleri bulunmuyor. CMake `find_package(Qt6)` noktasında duruyor. Ekran görüntüsüyle görsel birebirlik veya Gentoo üzerinde gerçek çalışma bu ortamda doğrulanamadı.

## Gentoo'da deneme

Çalışan önceki sürümü yedekleyin. Bu deneme için tercihen ayrı bir checkout/branch kullanın:

```bash
# Paket içindeki centurion-main klasöründe:
cmake -S gui -B build-ui-test -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-ui-test -j "$(nproc)"
./build-ui-test/centurion
```

Derleme Qt6/QtSvg/QtDBus ve projedeki diğer ön koşulların sistemde zaten kurulu olmasını gerektirir. Paket kodu değiştirilmiş kaynak sürümdür; kurulmuş eski sürümün üzerine otomatik yazmaz.

## İlgili dosyalar

- `gui/src/healthtab.cpp`
- `gui/src/healthtab.h`
- `gui/src/overviewpage.cpp`
- `gui/tests/health_catalog_redesign_static_test.py`

