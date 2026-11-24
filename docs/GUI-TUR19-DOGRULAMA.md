# Centurion Experimental — Tur 19 kısmi geliştirme ve devir raporu

Temel: `Centurion-experimental-tur18-kismi.zip`; SHA-256 `338bcf79acbad6d44458c777c4d5a15b0f644bc047af719779f94d1a087aa0f6`; ZIP CRC PASS. Önceki 48 paket ve 118 GUI ID matrislerinin bütün sütunları korunur; özgün Tur 14 devir raporu değiştirilmedi. 0 tam kabul, 47 kısmi, D05 doğrulama engelli. Tur 19 C12, D04 ve D05 üzerinde çalışıldı. Tüm 48 paketin kaynak çağrı zinciri denetimi henüz tamamlanmadı.

## C12 — Wi-Fi Channel View
- Kaynak denetimi: `wifi.rs` kernel `iw dev <iface> scan dump` çıktısındaki frekans, RSSI ve göreli seen_ms değerlerini işliyordu. HT/VHT operation bilgileri ve kernel last-seen boottime alanı atılıyordu. GUI yalnız noktalar çiziyor ve tüm genişlikleri Unknown gösteriyordu. `pick` seçili adaptör bulunmadığında başka adaptöre geçebiliyordu.
- `wifi.rs`: HT 20/40 MHz, VHT contiguous 80/160 MHz (eski width=2 ve revised width=1 + iki merkez) operation alanları ayrıştırılır. Primary channel, frequency, center segment, width ve band sınırları kontrol edilir. Duplicate operation/field, uyumsuz veya eksik alan, 80+80, görülebilen HE/EHT operation, 6 GHz ve bilinmeyen formatlar için interval yok. Bu değerler bildirilen nominal aralıktır; RF occupancy veya actual PHY bandwidth ölçümü değildir. Eski iw sürümü üst nesil operation bilgisini hiç yazmayabilir; HT/VHT kaynak etiketi bu yüzden korunur.
- Kernel `last seen: ...s [boottime]` tamsayı milisaniyeye çevrilir; helper `CLOCK_BOOTTIME` dump completion zamanı ekler. GUI observed timestamp'i receipt/dump/current sıra kontrolü ile kullanır, aksi halde seen_ms + elapsed alt sınırı (eski helper için receipt tabanı) kullanır. UTC sadece receipt etiketi; kernel last-seen wall-clock değildir. Milisaniye çözünürlüğü nedeniyle observed yaş yaklaşık gösterilir.
- `netpage.cpp`: mevcut cached-snapshot dialogu nominal kanal span çizgileri, center, source, width, nominal overlap counts, bilinen/bilinmeyen satır sayısı ve yaş kaynağı gösterir. Unknown width noktası korunur; yaş bilinmeyen/eski marker hollow ve span dashed. Aynı BSSID/frekans duplicate satırları kesin karşılaştırılmaz. Snapshot'taki 64 satır sınırı korunur. Nominal overlap, eski veya güncel satırlara dayanabilir; RF interference/channel utilization değildir. Eski 2.4 GHz primary-channel heuristic artık açıkça tahmin diye etiketlenir. Yeni polling/auto-scan yok.
- Scan/rescan açıkça seçilmiş adaptör bulunamazsa başka adaptöre geçmez. Cached scan öncesi/sonrası sysfs ifindex karşılaştırılır. GUI mismatch/absent sonucu cache'i temizler; mevcut generation guard korunur. Nearby Networks seçim anahtarı BSSID+frequency; duplicate anahtar geri yüklenmez. ifindex reuse ve komut ile kimlik kontrolü arasındaki yarış tamamen giderilmemiştir. Mevcut kullanıcı tarafından başlatılan NetworkManager rescan yolu korunmuştur.
- Kalan: HE/EHT/6 GHz ve 80+80/puncturing parser desteği (veri varsa), gerçek iw sürümleri/çoklu adaptör fixture ve Qt görsel davranışı, gerçek ağ kartı doğrulaması; full C12 kabulü verilmedi. GUI-183 özgün paket kapsamındadır, 118 satırlık tabloda ayrı bir satırı yoktur; yeni ID eklenmedi.
- Format cross-check: upstream iw scan.c, https://kernel.googlesource.com/pub/scm/linux/kernel/git/jberg/iw/+/c62dc398aa2406bd2dd0aa28ecb34cf4d3a2e1af/scan.c (HT operation/last seen/VHT output format). Kaynak kod kopyalanmadı; mevcut parser genişletildi.

## D04 / D05 — Testler ve derleme
- 15 yeni Türkçe katalog girdisi; Qt placeholder kontrolü PASS. Önceki 45 runner + yeni `wifichannel_native_test.py`: 46/46 PASS. Yeni test actual C++ interval/age functions: Debug, O2, ASan+UBSan, 100.000 rastgele interval fixture/her mod, exact/relative/future/overflow/unknown age, range boundary and overlap. Kaynak sözleşmesi testi genişletildi, final GUI/katalog değişikliklerinden sonra ilgili 3 test tekrar PASS; bunlar native Qt davranış testi değildir.
- Rust `advertised_channel_ranges_and_timestamp` fixture eklendi (HT, VHT, unsupported/duplicate, hidden SSID, malformed timestamp); Cargo bulunmadığından çalıştırılmadı. CI yeni native C++ runner'ı içerir; uzak CI çalıştırılmadı.
- Apt update DNS hatası, Qt6/cargo/rustc paketleri bulunamadı. Debug/Release CMake Qt6Config.cmake bulunamadığı için BLOCKED; Rust cargo test BLOCKED. Native Qt/CTest, Rust/Clippy, Gentoo PGO-LTO, gerçek GUI ve fiziksel Wi-Fi doğrulaması yapılmadı.
- CO Search crash guard, GPU Apply/Reset, SMU, firmware ve diğer Rust donanım helper'ları değiştirilmedi. Wi-Fi scan helper değiştirilmiştir; fiziksel donanım kabulü yok. GitHub push yok.

## Değişiklik envanteri
6 mevcut dosya değişti, 1 yeni test dosyası, 0 kaynak silindi.
- `.github/workflows/ci.yml`
- `crates/centurion-helpers/src/wifi.rs`
- `gui/i18n/centurion_tr.ts`
- `gui/src/netpage.cpp`
- `gui/src/wifiage.h`
- `gui/tests/scene_wifi_contract_static_test.py`
- `gui/tests/wifichannel_native_test.py`

## Kalan kapsam
`GUI-TUR19-48-PAKET.tsv` ve `GUI-TUR19-118-KABUL.tsv` tüm özgün kabul ve önceki turların açık işlerini korur. `GUI-TUR19-RUNNERS.tsv` test sonuçları. Tüm paketler önceki kabul statülerini koruyor. Bu teslimat kısmi kaynak paketidir, tamamlanmış release değildir.
