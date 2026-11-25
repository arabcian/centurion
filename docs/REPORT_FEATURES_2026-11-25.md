# Ürün raporu önerileri → kod (2026-11-25)

"Centurion ürün ve geliştirme raporu" tablolarındaki her satırın kodda nasıl karşılandığı.
Tüm yeni mantık, donanımsız test edilebilen başlıklarda (`gui/src/*.h`) durur; arayüz yalnızca bunları çağırır.

## 1. Günlük kullanım ve donanım yönetimi

| Satır | Uygulanan | Dosyalar |
|---|---|---|
| Ana sayfa — ne zaman etkili | Güç profili değişikliği artık geri okunarak doğrulanıyor ("in effect now (read back)" / "read-back shows …"). MUX'un "after reboot (running …)" işareti zaten vardı ve helper'ın `reboot_pending` alanı eşleşince kalkıyor. | `hometab.cpp` |
| Sahneler — bileşen özeti + son tetikleyici | `SceneEngine::apply(name, trigger)`: her uygulama tetikleyicisini (manual, tray, shortcut, dbus, `power: on battery`, `login`, `resume from sleep`, `game ended`, `co-search`, …) ve her bileşenin son durumunu tutar. Sahneler sekmesinde *Last apply* bloğu ✓/✗/⏳ satırlarıyla; GPU reddedilince başarılı fan/CPU adımları ayrı görünür ve "N uygulandı, M başarısız — uygulananlar etkin" uyarısı çıkar. Gecikmeli GPU adımı sonuçlanınca blok güncellenir. `scene-transactions.jsonl` kaydına `trigger` ve `components` eklendi. Oyunun başlattığı sahneler (centurion-gamemode) "game start/exit" tetikleyicisiyle görünür. | `sceneapplysummary.h`, `scenes.cpp/.h`, `scenestab.cpp/.h`, `tray.cpp`, `shortcuts.cpp`, `dbusapi.cpp`, `driftwatch.cpp`, `cosearchdialog.cpp` |
| Fanlar — iki eğriyi eş koşullarda karşılaştır | `centurion-cooltest` artık her çalıştırmada *sıcaklık limitinde geçen süreyi* (`limit_secs`, hedefin 2 °C yakınında geçen saniye) kaydediyor. Sağlık → Soğutma'da iki satır seçilip *Compare two runs*: aynı firmware bağlamı, ikisi de temiz, aynı süre ve başlangıç sıcaklığı farkı ≤ 3 °C değilse karşılaştırma reddedilir ve nedeni söylenir; aksi hâlde kararlı/tepe sıcaklık, RPM, limitte geçen süre ve saat (tamamlanan iş) farkı gösterilir. | `cooling.rs`, `centurion-cooltest.rs`, `coolcompare.h`, `coolingpage.cpp/.h` |
| Firmware — yeniden başlatma öncesi/sonrası | BIOS CPU OC değerleri yazılınca istenen değerler açılış kimliğiyle `~/.local/state/centurion/fw-nextboot.json`'a kaydedilir. Yeniden başlatma sonrası ilk okumada istenen ↔ firmware'in geri okuduğu değer yan yana: eşleşti ✓ / firmware sınırladı / okunamadı. Hepsi görülünce kayıt silinir. Çalışma anındaki limitler zaten doğrudan geri okumayla doğrulanıyordu. | `nextbootcheck.h`, `fwattrtab.cpp` |
| Ekran, klavye — profil taşınabilirliği | Aydınlatma (Spectrum) → *4-zone preview…*: profilin statik tuş renkleri dört sütun bandında ortalanır; renksiz tuşlar ve 4 bölgeli klavyenin gösteremeyeceği efektler sayılır; renkler panoya kopyalanabilir. Desteklenmeyen panel ayarı sahnenin diğer adımlarını zaten durdurmuyordu (`SceneEngine::next()` başarısız adımdan sonra devam eder); yeni özet bunu görünür kılar. | `spectrumzone4.h`, `lightingtab.cpp/.h` |

## 2. Performans, eğri ayarları ve doğrulama

| Satır | Uygulanan | Dosyalar |
|---|---|---|
| Ryzen CO — ilk hatayı yeniden üretilebilir kıl | `centurion-cpustress` hata satırı artık `kernel_index`, `data_seed` ve (`--load co` uyanış aşamasında) hatadan önceki boşta bekleme `idle_ms` alanlarını içeriyor. CO Search denemenin ilk hatasını çekirdek, CO değeri, test çekirdeği (wake bloğu dahil), veri tohumu, bekleme süresi, son aşama/döngü ve tam test komutuyla `first_failure` olarak deneme kaydına yazar ve günlükte gösterir. | `crates/centurion-cpustress/src/{main,co}.rs`, `cofirstfailure.h`, `cosearchdialog.cpp/.h` |
| Intel Undervolt — bağlam geçişleri | Servis (daemon) her tam uygulamayı (boot, AC ↔ pil, resume, sahne) alan bazında geri okuma sonucu ve hata nedeniyle `/run/centurion/intel-uv-transitions.json`'a yazar (son 20). Firmware kilidi / engellenen MSR yazımı `locked` olarak işaretlenir. Intel sekmesinde *Context transitions…* penceresi. | `intel_uv_daemon.rs`, `uvtransitions.h`, `inteltab.cpp/.h` |
| NVIDIA Curve — sınanan bölüm | *Record used region*: açıkken grafik saati her saniye (pencere gizliyken de) örneklenir ve o saate ulaşan en düşük voltajlı eğri noktasına atanır; eğride yeşil bantlar. NVML voltaj vermediği için voltaj, eğri eşlemesinden çıkarılır (sürücünün davranışı). | `vfcoverage.h`, `vfcurvewidget.cpp/.h`, `nvidiatab.cpp/.h` |
| AMD GPU — geri dönüşü doğrula | Deneme zaman aşımı / *Revert now* sonrası sürücü değerleri yeniden okunur; alanlar eşleşti / farklı / okunamadı (sürücü sıfırlanmış olabilir) olarak listelenir. | `fieldverify.h`, `amdgputab.cpp/.h` |
| Optimize/autotune — hedefler arası bedel | Autotune puan izine her aday için hedef bazında etki (`fx`) ve ölçülülük cezası (`modesty`) eklendi. Raporda ipucu: her adayın gecikme/iş hacmi/güç etkisi tek satırda; kullanıcının şimdiki ağırlıkları rapordakinden farklıysa hangi adayın kazanacağı ve kararı hangi ağırlığın değiştirdiği. | `autotune.rs`, `autotunereport.cpp/.h`, `optimizetab.cpp` |
| Kalibrasyon ve ABBA — kapsam | ABBA geçmişi ayrıntısının başına *Coverage*: tamamlanan/planlanan blok, dışlananlar (atlanan ayar, erken bitiş nedeni), ölçülen hedefler, ağırlığı olup ölçülemeyen hedefler ("hakkında iddia yok", ör. güç sensörü yoksa güç), karar aralığı. | `abcoverage.h`, `optimizetab.cpp` |
| Oyun entegrasyonu — ortanca/p99 | Kare süresi karşılaştırması zaten ortanca (P50), P95, P99 ve ortalamayı gösteriyordu; artık ortalama iyileşip P99 kötüleşirse (veya tersi) bunu açıkça yazar. | `frametimepair.h`, `optimizetab.cpp` |
| CPU/GPU testleri — "Geçti"ye kapsam | CPU testi kaydına sınanan CPU listesi ve test sırasında görülen sıcaklık aralığı eklendi. Geçmişte ipucu: CPU'lar, yük, °C aralığı, süre × döngü, "short scan" / "long verification" (CPU başına ≥ 20 dk). Kısa taramalar satırda etiketlenir. | `stresscoverage.h`, `cpustressbox.cpp/.h` |

## 3. Sağlık ve teşhis — araçlar arasında bağlam

Evidence Lab'a *Incident focus* eklendi: CPU, PCI adresi, Xid veya serbest metin. Beş raporun (H02 MCE, H03 AER, H04 Xid,
H05 F2FS, H09 Wi-Fi/BT) hepsi aynı odağa göre süzülür; odak sekme geçişlerinde, yeniden taramada ve yeniden açılışta
korunur. Öneriler son taramanın kendi metninden çıkarılır (`CPU 5`, `0000:01:00.0`, `Xid 79`, `bank 5`); alanlı PCI
adresi alansız yazan raporlarla da eşleşir. Dosyalar: `incidentfocus.h`, `healthevidence.cpp/.h`, `diagreportview.cpp/.h`.

## Testler

- `gui/tests/report_features_test.cpp` (ctest `report-features`): soğutma karşılaştırması, CO ilk hata, Intel geçişleri,
  AMD geri okuma, autotune bedel/ne-olurdu, ABBA kapsamı, test kapsamı, olay odağı.
- `gui/tests/report_features_native_test.py` (CI): sahne özeti, sonraki açılış kontrolü, Spectrum → 4 bölge, V/F
  kapsamı, kare süresi yönü — normal, `-O2`, ASan+UBSan.
- Rust: `cooling::tests` (limit süresi), `intel_uv_daemon::tests::transition_entry_marks_firmware_locks`,
  `centurion-cpustress` `wake_error_is_reported_on_the_pinned_cpu` (bekleme süresi tutuluyor).

## Sınırlar

- Fiziksel cihazda çalıştırılmadı; donanım okumaları yalnızca birim testleriyle ve derlemeyle doğrulandı.
- Yeni arayüz metinleri İngilizce; Türkçe çeviri kataloğuna (`gui/i18n/centurion_tr.ts`) henüz eklenmedi.
- Sentetik autotune tahmininin oyun sonucuyla otomatik karşılaştırılması yapılmadı: oyun kayıtları eşleştirilmiş bağlam
  kimliği taşımıyor (mevcut uyarı metni bunu söylüyor). Kare süresi karşılaştırması kullanıcı beyanlı kalıyor.
- NVIDIA kullanılan bölge kaydı bellekte tutulur (oturum boyunca), diske yazılmaz.
