# Centurion Experimental — Tur 37 doğrulama raporu

**KISMİ teslimat.** T36 kaynak SHA-256 `eb7cdd2805c88cd419a3cc91b59756586ffa2e4e170401e236c99407f6908b7a`. T37, T36 üzerine uygulanmıştır. Rapor/matris ekleri hariç 6 mevcut dosya değişti, 5 yeni dosya, 0 silinen dosya. Kaynak diff temiz T36 üzerinde `patch -p1` ile uygulanıp değişen dosyaların byte eşitliği kontrol edildi. Tüm 48/118/FIX/WANTED kapsamı aynen korunur: 0/48 tam kabul, 47 kısmi, D05 engelli; 118 GUI ID açık/kısmi; 21 FIX ve 12 WANTED/BUILD açık.

## Kullanıcının Gentoo build hatası

`gui/src/smartpage.cpp` içindeki `QJsonObject all` üzerinde `cbegin()/cend()` Qt6 API'sinde yoktur. `constBegin()/constEnd()` kullanıldı. Aynı yanlış kullanım T36 `gui/src/smartwatch.cpp` içindeki `disks` nesnesinde de vardı; geçmiş kodu bağımsız `smarthistory.cpp` dosyasına taşınırken düzeltildi. Diğer `cbegin()/cend()` kullanım noktaları tarandı; QMap/QHash/QList vb. geçerli iterator çağrıları topluca değiştirilmedi. Kalan noktalar logda. Qt6 bulunamadığı için bu kaynak düzeltmesi Gentoo'da native derlenmiş değildir.

## Gerçek kaynak ilerlemesi

- A03/A04: SMART Page ve SmartWatch ortak helper çağrısı (`smart::requestSample`). Uçuş, timeout dahil gerçek süreç çıkışına dek dolu; timeout sonrası sonuç Unknown, geçmişe yazılmaz. QPointer ile yok olmuş owner callback'i atlanır. Uçuş başına bir history record; ayrı GUI süreci/CLI/root uçuşları için kilit değildir. Root helper hiç çıkmazsa yeni SMART kontrolü başlatılmaz; UI event loop'u bloke etmemesi hedeflenir. Native Qt fixture engelli.
- C10: `smarthistory.cpp` bağımsız history katmanı. Boş/identity-only/error/duplicate/invalid identity batch tamamlandı sayılmaz; partial saklanır fakat son tam gün ilerlemez. Atomic QSaveFile commit sonucu denetlenir; corrupt dosya korunur. Bilinen sağlık alanları dışında yeni alanlar kaydedilmez. Eski geçerli geçmiş formatı korunur. Aynı gün tekrar ölçüm, son örnek üzerinden alert karşılaştırır. Bir gün için son örnek tutulur; gün içi event ledger değildir. Daha önce kaydedilen yanlış günlük marker'lar otomatik olarak yeniden yazılmaz.
- C10: Media-error/unsafe-shutdown/power-on-hour/critical-warning nonnegative signed 64-bit sayaç yolu; taşma/fraction unsupported → Unknown. Media delta için bağımsız önceki gün örneği gerekli, monoton olmayan interval Unknown. U64/128 ve helper string sayaçlar için tam uçtan uca destek henüz yok. Tek sıcaklık örneği sağlıklı disk kanıtı sayılmaz. SATA attribute inspector açık.
- C10: Açık kullanıcı tercihine bağlı arka plan SMART ölçümü, varsayılan kapalı. Kullanıcı işaretlerse yetkili yönetici için var olan saatlik due check sürer. Manual Check drives ve ilk sayfa açılışındaki sorgu korunur. Opt-in mevcut helper'ı kullanır; uyuyan disk uyanabilir. Otomatik self-test/stres/yazma eklenmedi. Tercihi kapatmak hâlen yürüyen sorguyu öldürmez, gelecekteki otomatik sorguları durdurur.
- D04: Yeni 7 SmartPage TR metni, düz metin hata/uyarı/cihaz etiketi. Özgün FIX metinleri yok; hiçbir FIX kabulü kapatılmadı.

## Doğrulama

| Test | Sonuç |
| --- | --- |
| T36 ZIP CRC | PASS |
| T36→T37 patch uygulaması ve değişen dosya byte eşitliği | PASS |
| 65 Python runner sweep | 65/65 PASS (son broker/GUI küçük düzenlemelerinden önce başladı) |
| Final hedefli rerun | 5/5 PASS (smarttrend, yeni tur37 static, gui_stage3, health_ui, translation_tr) |
| smarttrend exact INT64, counter reset, missing, no baseline | PASS; O2, GNU LTO -Werror, ASan+UBSan |
| smarthistory native CTest (QTemporaryDir, no helper/device) | YAZILDI, Qt yok; çalıştırılmadı |
| apt-get update | exit 0 ancak DNS: Temporary failure resolving deb.debian.org |
| apt-get install qt6-base-dev qt6-svg-dev qt6-tools-dev cargo rustc | exit 100; paketler bulunamadı |
| Final Qt Debug/Release configure | exit 1; Qt6Config.cmake bulunamadı |
| cargo build --locked | exit 127; cargo: command not found |
| Tam GUI build/CTest, gerçek Gentoo PGO/LTO, Rust test/clippy/deny, CI/visual/physical | ÇALIŞTIRILMADI |
| experimental push | YAPILMADI (native kabul kapısı açık) |

65+5 toplam benzersiz test sayısı değildir: 4 tekrar + 1 yeni static test; 66 benzersiz Python runner en az bir kez koşuldu, tamamı PASS, ancak bütün 66 final tree üzerinde yeniden baştan çalıştırılmadı. Native Qt compile/test kanıtı yok. CO, NVIDIA, firmware, fan, ABBA, boot, restore güvenlik kontrollerinde bu tur değişiklik yapılmadı. Gerçek donanım ölçüm/yazma/stres/reboot/restore testleri tetiklenmedi.

## Açık işler

A03/A04 ortak SMART ilerlemesi dışında önceki T36 açık alt işler aynen geçerlidir. A05/CO cross-process lease, A07/A08 fail-closed MainWindow + gerçek görsel kabul, B03/B07/B09/B11 P0 kapsamı, C10 ortak cooling/battery/sleep scheduler ve SATA attribute parity, diğer tüm paketler açık. 48/118/FIX/WANTED TSV tabloları önceki tüm sütunları ve T37 kayıtlarını içerir. T36 birleşik kalan tablo kaynak içinde korunur.
