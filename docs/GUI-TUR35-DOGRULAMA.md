# Centurion Experimental — Tur 35 kısmi doğrulama

Temel: `Centurion-experimental-tur34-kismi.zip`, SHA-256 `d57f1404c39611c298e69589759ee8e5feab2e57d07540c9842b9742ad7d0d9a`. ZIP CRC PASS. Tam kabul: 0/48; 47 kısmi, D05 engelli.

## Kaynak değişiklikleri ve açık kapsam

### A09
- Uygulama: Global stable target ambiguity rejection; Health catalog capability/risk badges; explicit stable-result favorite action.
- Kalan: Full dynamic unopened firmware/CPU/GPU provider, unified metadata and Qt focus/constructor tests remain open.

### X01
- Uygulama: Explicit command palette favorites for static Optimization setting keys and stable control IDs, current page/Health favorites preserved; unavailable control favorites retained with manual removal.
- Kalan: Unregistered lazy subtools, full orphan cleanup/migration, DPI and native restart acceptance remain open.

### C11
- Uygulama: Connection Today is explicitly program-path totals, absent versus recorded zero distinguished, exact signed counter sorting and chart saturation label; program ranking uses full executable path.
- Kalan: Ledger remains program/day, not per PID lifetime; stable conntrack accounting and actual rate not implemented.

### C13
- Uygulama: Connection row opens saved firewall preview; unknown direction requires explicit selection; source/destination port follows selected direction.
- Kalan: Effective live nftables/saved/draft canonical diff, actual packet context and unsupported expression behavior remain open.

### X07
- Uygulama: Frozen bounded saved kernel comparison by full boot UUID, classified kind/severity/code and reported device label; duplicate seq dedup, conflict discard, absent counts unknown.
- Kalan: Physical device identity across boots, full guard/preset history, rotated/incomplete source coverage and native Qt acceptance open.

### X08
- Uygulama: List helper emits boot UUID, namespace, family, socket cookie, inode and process start ticks/path after owner recheck; GUI rejects legacy PID enrichment, rechecks live details, guards 15s snapshot and late generation; saved-rule preview.
- Kalan: No atomic socket/process ownership proof, PID grouping tree, exact log deep links, v6-only/effective firewall proof, backend cookie-bound mutations or lifetime traffic accounting.

### D04
- Uygulama: 31 additional Turkish strings; no other FIX acceptance claimed.
- Kalan: Full FIX-01–21 behavior and rendered TR/EN two-shell tests open.

### D05
- Uygulama: 62/62 independent runners before final navigation/translation edits; 9/9 affected reruns on final source; new algorithm normal/LTO/ASan+UBSan PASS.
- Kalan: Qt6 configure fails, Cargo missing; native CTest/Rust/Clippy/Gentoo PGO/LTO/CI/hardware/visual unrun; no push.

## Güvenlik ve tasarım sınırları

- Yeni helper kimlik alanları salt okunur list snapshot'ına aittir. Daemon verdict/conntrack, nftables yazma ve SOCK_DESTROY backend algoritması değiştirilmedi. Cookie kaydı tek başına socket'in hâlâ var olduğunu kanıtlamaz. FD sahipliği ve executable path okuma TOCTOU ve aynı path ile exec yarışları mümkündür; process stat, cookie ve namespace bütününü atomik kanıt olarak değerlendirmeyin. Legacy log/helper, canlı süreç kimliğini kanıtlamadığında live details gösterilmez. GUI root snapshot yaşı 15 saniye, CLOCK_BOOTTIME request başlangıcı esas alınır; whitelist, kill ve Open Ports değişikliklerinde ek stale check mevcut, fakat backend kill halen mevcut tuple semantiğini kullanır. Genel IP blacklist işlemleri değişmedi. 
- A/B veya trafik verileri uydurulmadı. Geçmiş ledger formatı değişmedi; socket bazında byte/rate yok. INT64_MAX değeri lower-bound olarak etiketlenir.
- Firewall preview kullanıcı seçimiyle varsayımsal saved-chain değerlendirmesi, effective nft/conntrack sonucu değildir. Yeni yazma yolu yok.
- Boot karşılaştırması iki dosyanın son 512 KiB'lik bölümleriyle sınırlı, kayıtlı ve sınıflandırılmış satırlar, tam UUID + seq. Aygıt etiketi fiziksel kimlik değil. Canlı yeni kernel sorgusu yok. Eski boot guard ve pstore karşılaştırması korunur.
- Favori seçimi elle yapılır; page/Health eski davranışı korunur, kayıtlı control/setting hedefleri erişilemezse kaldırma seçeneği. Global duplicate stable target sonuçları reddedilir; metadata indeksleme yeni helper/constructor çağırmaz. Yeni dinamik provider değil. Favoriden sayfa açmak mevcut normal sorguları tetikleyebilir.
- C12 HE/EHT, A07/A08 ve diğer açık paketler bu tur değiştirilmedi. Özgün 48 paket/118 GUI/ek ID/FIX/Wanted kriterleri daraltılmadı. GitHub push yok.

## Testler

- İlk tam bağımsız set: 62/62 PASS (Tur34'ten kalanlar dahil). Bu koşu son navigasyon ve katalog düzenlemelerinden önce tamamlandı. Son etkilenen 9 runner: 9/9 PASS; tüm set final kaynak üzerinde ikinci kez çalıştırılmadı.
- Yeni `tur35_network_boot_native_test.py`: 30.000 fixture her modda, normal, GNU LTO `-Werror`, ASan+UBSan PASS. Bu test C++ bağımsız algoritma; GUI veya Rust runtime doğrulaması değil.
- Qt Debug/Release configure FAIL (Qt6Config.cmake yok). Cargo bulunamadı. DNS getent başarısız. Bu tur paket kurulumu yapılmadı.
- Rust'a eklenen proc stat / cookie unit test çalıştırılmadı. Qt CTest, Rust workspace build/test/clippy/deny, Gentoo tam LTO/PGO, CI, görsel ve fiziksel donanım doğrulaması yapılmadı. Tur34 baseline ayrıca çalıştırılmadı.
