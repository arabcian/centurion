# Centurion Experimental — Tur 34 kısmi doğrulama

Temel: `Centurion-experimental-tur33-kismi.zip` (SHA-256 `c3cd8514f58755fe98e1c665fd9c8b424d1f8fa8b0a740338f9417f6eb56bb18`). Tam kabul: **0/48; 47 kısmi, D05 engelli.**

## Uygulanan davranışlar
- B04: S08 firmware hedef kanıtı: strict signed integer, default/range/step, kaynak, UTC, platform profil değişim uyarısı, STAPM keşfi.
- B05: Ortak CPU topology: cpu_core/cpu_atom PMU membership, missing/conflict/offline/SMT contradiction, capture sonrası listelerin kontrolü.
- B06: AMD DPM explicit read worker, 5s UI deadline, late result rejection, per-tab single flight, transient sysfs inode identity.
- X04: S08 firmware metadata provenance iyileştirmesi; mevcut inspector read-only yolundan erişilir.
- D04: 24 yeni TR katalog girdisi.
- D05: Yeni bağımsız test 30.000 fixture normal/LTO/ASan+UBSan PASS; tam runner koşusu kesildi, 29 kayıtlı PASS; iki ayrı statik test PASS.

## Sınırlar ve tasarım kararları

- Firmware: yeni rapor hedef/default/range ayrımını yapar; native birim, gerçek SMU enforcement değil. Profil öncesi/sonrası kontrol A→B→A yarışını dışlamaz. WMI fallback ve doğrudan WMI komutları değişmedi. Eski firmware yazma yolları korunur.
- Intel: iki geçerli PMU listesi olmadan sınıflandırma yok; offline ve overlap ayrı, doğrulanmış SMT kardeşleri çelişirse conflict. PMU sınıfı fiziksel çekirdek türünün bağımsız CPUID kanıtı değildir. Raw core_type korunur. Referans: https://docs.kernel.org/admin-guide/cputopology.html .
- AMD: tek worker, 5s boottime GUI kabul süresi; sysfs okuması zorla iptal edilmez. Pencere kapanırsa callback context üzerinden iptal; worker gerçekten bitmeden yeni istek yok. Inode yeniden kullanımı ve runtime PM TOCTOU mümkündür. Yalnızca sunum tarafı; donanım yazma/restore algoritması değişmedi. Eski live polling hâlâ senkron.
- A09, C12 ve X08 dahil diğer açık paketlere bu tur ayrı değişiklik yapılmadı. A07/A08 açık. GitHub push yok.

## Testler

- Tam runner koşusu araç timeout nedeniyle kesildi: 29 kayıtlı PASS, 0 kayıtlı FAIL; 61 beklenen runner. Sonucu olmayanlar PASS sayılmadı. Ayrı yeni powerprovenance testi normal, GNU LTO -Werror, ASan/UBSan: PASS (30.000 senaryo her modda). Ayrı translation_tr ve gui_completion_static PASS. Tur33 baseline ayrıca çalıştırılmadı.
- Debug/Release Qt configure FAIL: Qt6Config.cmake yok. Cargo ve qmake6 bulunamadı; DNS probe sonuç döndürmedi. Paket kurulumu yapılmadı.
- Yeni Qt diagnostictools fixture, CTest, Rust, Gentoo LTO/PGO, fiziksel donanım ve CI çalıştırılmadı.

## Kalan kapsam
- B04: Gerçek PL/PPT/STAPM enforcement, draft bağlantısı, WMI hata/units, native ve fiziksel doğrulama açık.
- B05: Doğrudan Intel CPUID/core_type ABI sınıflandırması, voltage-domain coupling, deep links ve native fixtures açık.
- B06: Ortak telemetry scheduler, gerçek hotplug ve PM TOCTOU, asenkron live polling ve native Qt fixture açık.
- X04: Tam disabled reason enum, WMI partial receipt ve factory fan overlay açık.
- D04: Tüm FIX ve görsel/dil kabulü açık.
- D05: Qt6/Cargo eksik; native CTest, Rust, Gentoo PGO/LTO, fiziksel doğrulama ve kalan runner sonuçları açık.
- Özgün 48 paket / 118 GUI / ek ID / FIX / Wanted kriterleri korunur.
