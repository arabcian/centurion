# Centurion Experimental — Tur 21 doğrulama ve kalanlar

Temel: `Centurion-experimental-tur20-kismi.zip` (SHA-256 `6c3707fa4466af554c714770bb59b99d19607495afba1530f1e4a6efa59144ba`). Tur 20 matrisleri ve özgün Tur 14 raporu korunmuştur.

## Gerçek uygulama
- X08 / GUI-186: Open Ports artık bind kapsamını gerçek erişilebilirlikten ayırır. IPv4/IPv6/mapped IPv6, loopback, wildcard, private/ULA, shared, multicast, link-local, specific ve unknown. IPv6 dual-stack bilinmiyor. PID, UID, family sütunları; exe tooltip korunur. Aynı isimli farklı soketler gizlenmez; aynı snapshot tuple birden çok kez gelirse seçim otomatik geri yüklenmez.
- Saved firewall sütunu helper snapshot'ının kayıtlı politika özetidir, fiili nftables veya bağlantı testi değildir. Geçici UDP portu yalnızca olası istemci; wildcard turuncu ama erişim garantisi değil. Firewall uyarılarında erişim iddiaları kaldırıldı. Port eylemi tüm ilgili protokol/port için uygulanır.
- Sağ tık menüsü hedef tuple'ı kopyalar; işlem sırasında tekil snapshot çözümlemesi, geçersiz port/protokol ve başarısız snapshot kontrolü; confirmation sonrası tekrar çözümleme. Eski expected_firewall revision/lock yolu korunur. Kernel cookie/PID start identity değildir; root snapshot stale ve dış süreç değişimi mümkün. Gerçek ağ taraması veya yeni otomatik sorgu eklenmedi.
- D04: Türkçe katalog güncellendi. D05: CI native classifier runner eklendi.

## Dosya farkı
5 mevcut dosya değişti; 2 yeni kaynak/test dosyası; silinen yok. Belge matrisleri bu sayılara dahil değil.
- `.github/workflows/ci.yml`
- `gui/i18n/centurion_tr.ts`
- `gui/src/netfirewall.cpp`
- `gui/src/netpage.cpp`
- `gui/src/netpage.h`
- `gui/src/socketbind.h`
- `gui/tests/socketbind_native_test.py`

## Doğrulama
- 48/48 Python runner PASS (önceki 47 + socketbind_native_test.py), son sağ tık guard değişikliği öncesi. Son değişiklik ardından translation_tr_test.py ve test_firewall_preview_contract_static.py yeniden PASS. Bunlar Qt davranış testi değildir.
- Native C++ Debug, O2, ASan/UBSan: 100000 rastgele IPv4/mapped roundtrip her modda; adres sınırları, malformed ve NUL fixtures PASS. `g++ -Wall -Wextra -Werror`.
- CMake Debug/Release: Qt6Config.cmake eksik. Cargo test: cargo yok. apt update: DNS hatası; apt install simulation: paket bulunamıyor. Qt6 gerçek GUI, Rust, Gentoo LTO/PGO, fiziksel ağ ve GitHub CI çalıştırılmadı. Push yapılmadı.
- CO, GPU, firmware ve Rust helper dosyaları değişmedi. Firewall helper, yetki mekanizması ve expected revision guard korunur; gerçek ağ yazma testi yapılmadı.

## Kalan kapsam
48 iş paketi, 118 GUI ID, özgün kabul koşulları korunur. 0 tam kabul, 47 kısmi, D05 engelli. X08/GUI-186: stable process PID+start/exe, socket cookie, process grouping/children, deep links, namespace/v6-only/effective policy, native Qt/fiziksel test açık. Diğer paketler önceki durumlarında; yeni tamamlanma iddiası yok. Tam liste `GUI-TUR21-48-PAKET.tsv` ve `GUI-TUR21-118-KABUL.tsv`. Bu tur kapsamın tamamını kaynak düzeyinde denetlemedi.
