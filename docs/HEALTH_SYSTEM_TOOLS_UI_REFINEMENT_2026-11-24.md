# Centurion Health / System Tools — 24 Kasım 2026 düzenlemesi

Bu güncelleme, **bir önceki `Centurion_Health_SystemTools_UI_Experiment_20261123.zip`** kaynak paketi üzerine uygulanmıştır. Health Center / System Tools kataloğunun görünümünü ve gezinmesini değiştirir; donanım ayarı uygulama kodu, H18 araştırma algoritmaları, araç listesi ve LazyWidget mekanizması korunmuştur.

## İstenen değişikliklerin karşılığı

1. **Durum panelleri kaldırıldı.** System Tools içindeki dört panel (`Needs attention`, `Review`, `Normal`, `Not checked`) ve altlarındaki son tarama açıklaması artık oluşturulmuyor. Health > System overview ekranı hâlâ mevcut.
2. **Troubleshoot by symptom korunup okunur hale getirildi.** Altı semptomun tamamı mevcut H18 Guided Triage giriş noktalarını açar. İçerik alanı 1110 piksel ve üzerindeyken **3 sütun**, 710–1109 piksel arasında **2 sütun**, daha darsa **1 sütun** gösterilir. Kartlar gerçek etiket/açıklama yüksekliği kadar büyür; kesilmeye yol açan `QPushButton` iç içe layout kullanımı kaldırıldı. Kartın tüm yüzeyi fareyle; Enter, Return ve Space ile klavyeden açılır.
3. **Quick access sadece gerçek favorilerden oluşuyor.** Önceden varsayılan olarak eklenen `EDID / VRR`, `Wi-Fi / Bluetooth Firmware Doctor`, `Power Limit Source Map`, `Hardware snapshot` artık otomatik listelenmiyor. Kullanıcının `health/pinned_tools` ayarında kayıtlı favorileri gösteriliyor. Hiç favori yoksa araç kutusu oluşturulmaz; yalnızca kısa açıklama kalır. Yıldızla ekleme/çıkarma ve araca girdiğinde `☆ Pin` işlevi sürer.
4. **Kart açıklamaları kırpılmıyor.** Quick Access ve All Tools kartlarında açıklama için belirlenmiş maksimum yükseklik kaldırıldı. İçerikleri esas alan `QFrame` ve ayrı favori yıldızıyla tam açıklamalar, veri kaybı olmadan satır kaydırılır.
5. **Alan ekonomisi iyileştirildi.** Başlık/arama, Troubleshoot, Quick Access ve kategori filtresi yerli yerinde. Gereksiz durum şeridi kalktı; semptomlar dengeli iki sıra halinde ve kullanılabilir genişliğe göre gösteriliyor. Quick Access favori yokken yer işgal eden kutu satırı üretmiyor.

## Devam eden davranışlar

- `Health -> System overview` sağlığı hesaplamaya devam eder; System Tools açılması yeni tarama başlatmaz.
- `System Tools -> All tools` kısmındaki kategoriler, arama, filtreler, yıldızlar, son kullanılanlar ve açılır-kapanır grup tercihleri korunur.
- Bir araç açıldığında eski orijinal `LazyWidget` devreye girer. Araçların güvenlik bayrakları, dGPU uyandırma/ayarlara yazma davranışı değişmedi.
- Favoriler `gui.ini` içindeki `health/pinned_tools` üzerinden okunup kalıcı olarak saklanır. Eski favoriler otomatik sıfırlanmaz.

## Testler ve doğrulama

Kaynak tabanlı Python testleri çalıştırıldı. Qt6 geliştirme başlıkları ve CMake `Qt6Config.cmake` **bu ortamda bulunmadığından gerçek GUI derlemesi burada yapılamadı**; ekran görüntüsü veya çalışma garantisi iddia edilmiyor. Kullanıcı kendi Gentoo sisteminde derlemeyi doğrulayabilir:

```bash
cmake -S gui -B build-ui-test -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-ui-test -j "$(nproc)"
./build-ui-test/centurion
```

Doğrulama için üç senaryo: (1) Favoriler boşken Quick Access kutusuz görünmeli. (2) All tools içinden bir yıldıza basınca Quick Access'e eklenmeli, kaldırınca kaybolmalı. (3) Pencere daraltılıp genişletildiğinde H18 semptom kartlarının bütün başlık ve açıklamaları okunmalı.
