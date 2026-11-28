# Centurion kod incelemesi — çalışma günlüğü

Her oturum önce bu dosyayı okur, "Durum" bölümünden devam eder.
Kurallar: davranış korunur; kırmızı çizgi alanları (helpers doğrulama/clamp/izin listeleri,
donanıma yazma sırası/trial-revert, polkit/udev/systemd/openrc, install/uninstall) yalnızca raporlanır.
Python static testleri kaynakta metin arıyor (≈441 assert); bunları kıran bir refactor durdurulup sorulur.

## Durum

- Aşama 0: TAMAMLANDI (onaylandı 2026-11-28).
- Aşama 1 (helpers ortak altyapı): TAMAMLANDI — tüm bulgular uygulandı.
- Aşama 2 (tune.rs, tune-helper, power, pmtable): TAMAMLANDI — tüm bulgular uygulandı (F2.5 yalnızca bilgi).
- Onaylı düzeltmeler (2026-11-28, "onay bekleyenleri düzelt"): F1.2–F1.7, F2.2–F2.4 uygulandı.
- Kullanıcı talimatı: aşamalar arasında onay beklemeden devam et; kırmızı çizgi bulguları yine raporlanır, uygulanmaz.
- Aşama 3 (nvcurve): TAMAMLANDI — F3.1–F3.4 onayla uygulandı (2026-11-28), F3.5 yalnızca bilgi.
- Aşama 4 (Intel undervolt): TAMAMLANDI — F4.1–F4.3 onayla uygulandı (2026-11-28), F4.4 yalnızca bilgi.
- Aşama 5 bağlam için ikiye bölündü. 5a (Ryzen/SMU, Legion WMI/EC, fwattr): OKUNDU — kod değişikliği yok, F5.1 onay bekliyor.
- Aşama 5b (amdgpu, dgpu, nvreg, cooling, fan_table, lighting, centurion-spectrum): OKUNDU — kod değişikliği yok,
  F5.4 onay bekliyor, F5.5–F5.8 yalnızca bilgi.
- Aşama 6a (autotune): TAMAMLANDI — F6.1 uygulandı (yalnızca doc yorumu), F6.2–F6.4 yalnızca bilgi.
- Aşama 6b (calib, centurion-calibrate, calctx, calsim): OKUNDU — kod değişikliği yok, F6.5–F6.7 rapor.
- Aşama 6c (bench, stability, cooltest, gamemode, boot*): OKUNDU — kod değişikliği yok; F6.9 onay bekliyor, F6.8/F6.10/F6.11 bilgi.
- Aşama 7 (backup, tarscan, sysadmin, fstabcheck): OKUNDU — kod değişikliği yok; F7.1–F7.3 onay bekliyor (kırmızı çizgi), F7.4–F7.5 bilgi/soru.
- Onaylı düzeltmeler (2026-11-28, "sadece onay bekleyenleri fixle"): F5.1, F5.4, F6.9, F7.1, F7.2, F7.3 uygulandı.
- Devir: `docs/review/HANDOVER.md` (açık kalanlar + kalan aşamalar).
- Oturum 2 (2026-11-28): başlangıç yeniden ölçüldü (dd0c608: 737 / 0 / 9, clippy temiz, ctest 77/77, python 45/45).
  Sorulara cevaplar: F5.7 dokunma; F6.10 kapalı başarısız olsun; F7.5 MODE/OWNER/GROUP reddedilsin; F8.3 açılıştaki
  gibi tolere et. F6.10 (`dfb8f08`), F7.5 (`b9c7027`), F8.3 (`14b08ad`) uygulandı.
- Aşama 8 (netguard, firewall, traffic, wifi + bin'ler): TAMAMLANDI — F8.1 uygulandı (`63d5077`), F8.3 onayla uygulandı,
  F8.2/F8.4–F8.6 bilgi.
- Aşama 9 (teşhis / sağlık): KISMEN — kök yolları okundu, kod değişikliği yok; F6.6 kapandı (ulaşılamıyor), F9.1–F9.3 bilgi.
  Kalan: aşağıda "Okunan dosyalar / Aşama 9"da yalnızca taranan diye geçen modüller satır satır okunmadı.

## Başlangıç durumu (2026-11-28, commit 123c3c7, dal experimental)

Ortam: rustc/cargo 1.97.0, cmake 3.28.3, Qt 6.4.2 (CI'daki apt paket listesi kuruldu).
Not: ilk ölçüm container'ın eski klonunda (b745ba0) yapılmıştı; uzak dalda ~55 yeni commit vardı,
birleştirildi ve her şey yeniden çalıştırıldı. Aşağıdaki sayılar birleştirilmiş uçtan.

| Komut | Sonuç |
|---|---|
| `cargo test --workspace --locked` | GEÇTİ — 726 geçti, 0 kaldı, 9 ignored, 41 suite |
| `cargo clippy --workspace --all-targets --locked -- -D warnings` | GEÇTİ (eski uçtaki `sysadmin.rs:171` lint hatası upstream'de zaten düzeltilmiş) |
| `cmake … -DCENTURION_SANITIZE=ON -DCENTURION_LTO=OFF` + build + `ctest` | GEÇTİ — 77/77 (CI bayrakları: ASAN/UBSAN, `QT_QPA_PLATFORM=offscreen`) |
| CI'daki 45 `python3` test adımı (rust + gui job'ları) | GEÇTİ — 45/45 |
| `cargo-deny` (CI'da ayrı job) | ÇALIŞTIRILMADI — görev listesinde yok, araç kurulu değil |

Ignored testler: `cowork::chunk_times`, `autotune::perf_bench::bench_big_signature`, `autotune::live_autotune`,
`model::perf_realistic_size`, `calsim::{incumbent_head_start_steadies_decisions, loso_against_loo, seen_test_rates, sim_regression}`, `sim::sweep`.

## Envanter

Toplam (123c3c7): Rust ≈ 66.0k satır (testler dahil), GUI `gui/src` ≈ 73.1k satır, test dosyaları ≈ 9.6k satır.
Tablolardaki dosya bazlı sayılar b745ba0'dan; fark küçük (ör. optimizetab 4669, backup.rs 2898, netguard.rs 2551).

Risk seviyeleri: **R1** = root + donanıma (MSR/EC/WMI/SMU/sysfs/NVAPI) yazma; **R2** = root + sistem dosyası/ağ/
yapılandırma yazma; **R3** = root olmayan veya salt okuma/saf mantık; **G1** = ayrıcalıklı helper çağıran GUI;
**G2** = salt UI.

### Rust crate'leri

| Crate | Satır | Risk | Not |
|---|---:|---|---|
| centurion-helpers | 54 579 | R1/R2/R3 karışık | 26 binary, 19'u polkit üzerinden root |
| nvcurve | 5 631 | R1 | NVAPI V/F eğrisi, `nvcurve-root-helper` |
| centurion-cpustress | 1 614 | R3 | stres çekirdekleri (SIMD) |
| centurion-spectrum | 1 162 | R1 (hafif) | klavye/LampArray HID yazma |
| centurion-gpustress | 1 002 | R3 | Vulkan stres |

### centurion-helpers modül grupları

| Grup | Dosyalar (satır) | Risk |
|---|---|---|
| Ortak altyapı | lib.rs 558, field 244, par, wlog, defaults, machine 322, unlock 400, distro 484 | R1 (tüm helper'lar kullanıyor) |
| Tune / güç limitleri | tune.rs 4266, bin/tune-helper 2007, tune-admin-helper, tune-profile-helper, power, pmtable | R1 |
| Intel undervolt | intel_uv 2116, intel_uv_daemon 637, intel_uv_search 318, bin/intel-uv-helper, bin/centurion-intel-uv | R1 |
| Ryzen / SMU | smu 711, ryzen_mon, bin/ryzen-co-helper 344 | R1 |
| Legion firmware/EC | legion_wmi 1505, bin/legion-{profile,firmware,gpu}-helper, legion-ec-sensors, bin/fwattr-helper 340 | R1 |
| GPU / fan / ışık | amdgpu 672 + helper, dgpu 313, nvreg 355, cooling 633, fan_table 291, lighting 399 + helper | R1 |
| Autotune / kalibrasyon | autotune 4210, calib 2853, bin/centurion-calibrate 3972, calctx 374, calsim 484, bench 1689, stability 397, bin/centurion-autotune 318, bin/centurion-cooltest | R1 (yazmayı tune/uv yollarıyla tetikliyor) |
| Gamemode / boot | bin/centurion-gamemode 1673, bootguard, boothist, bootlogo 447, bin/centurion-boot-guard | R1/R2 |
| Yedek / sistem yönetimi | backup 2676, tarscan 509, bin/backup-helper, sysadmin 585, fstabcheck, bin/sysadmin-{,config-}helper | R2 |
| Ağ | netguard 2526, firewall 540, traffic 337, wifi 1073, bin/netguard-helper, bin/centurion-netguard | R2 |
| Teşhis / sağlık | model 2535, diagreport 1250, health 537, health_* , overview 749, memory_spd 698, pstore, ksnap, flight, sleepdiag, wakeup, hibernate, iorate, trends, tools, display 418, bin/health-reader | R2/R3 |

### GUI (gui/src)

| Grup | Dosyalar (satır) | Risk |
|---|---|---|
| Ayrıcalık/çekirdek | privileged.cpp 277, mainwindow 1090, main 423, dbusapi, telemetri broker, prewrite, unlock, sysadmin, userhelper | G1 |
| Donanım ayar sekmeleri | optimizetab 4612, cosearchdialog 2714, hometab 2130, nvidiatab 1766, scenes 1727 + scenestab 1192, inteltab 1625, ryzentab 1359, amdgputab 920, fwattrtab 865, curvepage, fancurvedialog, coolingpage, lightingtab 1195, lighting | G1 |
| Sistem / ağ / yedek | netpage 2817, netfirewall, backuppage 1632, bootpage, sleeppage, kernelparamspage, bootlogodialog, memorydialog, nvregdialog | G1 |
| Teşhis / sağlık | healthtab 1953, diagreportview 1413, systemdiagnostics 1409, advanceddiagnostics 1051, systools 1270, diagnostictools, klogpage, smartpage … | G1/G2 |
| Salt UI | theme 1026, charts 808, sysinfo 730, header-only yardımcılar (*.h), tray, wizard … | G2 |

İlk sinyaller (ham sayım, test kodu dahil — bulgu değil, sadece öncelik için):
`QProcess::waitFor*` UI dosyalarında: cosearchdialog (4), systemdiagnostics (3), coolingpage, firstrunwizard, netlookup (1'er).
Rust'ta `unwrap/expect` yoğun dosyalar: backup (112), tune (83), calib (70), autotune (65), diagreport (65) — çoğu test modülünde olabilir, okumada ayrılacak.

## İnceleme planı (onaylandı 2026-11-28)

Sıra: önce ortak altyapı (diğer her şey ona yaslanıyor), sonra R1, R2, R3, sonra GUI G1, G2.
Her aşama tek oturuma sığacak boyutta tutuldu (~4–8k satır).

| Aşama | Kapsam | ≈Satır |
|---|---|---:|
| 1 | helpers ortak altyapı: lib, field, par, wlog, defaults, machine, unlock, distro | 2.5k |
| 2 | tune.rs + tune-helper + tune-admin/profile-helper + power/pmtable | 7.5k |
| 3 | nvcurve crate'in tamamı | 5.6k |
| 4 | Intel UV (intel_uv*, iki binary) | 3.4k |
| 5 | Ryzen/SMU + Legion WMI/EC + fwattr + amdgpu + dgpu + nvreg + cooling/fan_table + lighting + spectrum | 7k |
| 6a | autotune.rs + centurion-autotune | 4.5k |
| 6b | calib.rs + centurion-calibrate + calctx + calsim | 7.7k |
| 6c | bench + stability + cooltest + gamemode + boot* | 4.5k |
| 7 | backup + tarscan + sysadmin + fstabcheck + bin'ler | 4.5k |
| 8 | netguard + firewall + traffic + wifi + bin'ler | 4.8k |
| 9 | model + diagreport + health* + kalan teşhis modülleri | 8k |
| 10 | cpustress + gpustress | 2.6k |
| 11 | GUI çekirdek: privileged, mainwindow, main, dbusapi, telemetri, paylaşılan başlıklar | ~5k |
| 12a | optimizetab | 4.6k |
| 12b | cosearchdialog + ryzentab + inteltab | 5.7k |
| 12c | hometab + nvidiatab + curvepage + amdgputab + fwattrtab | 6k |
| 12d | scenes + scenestab + lightingtab + coolingpage + fancurvedialog | 5k |
| 13 | netpage + netfirewall + backuppage + boot/sleep/kernelparams | 6.5k |
| 14 | healthtab + diagreportview + systemdiagnostics + advanceddiagnostics + systools … | 8k |
| 15 | kalan G2 UI dosyaları + son rapor | ~10k |

## Bekleyen onaylar

F5.1, F5.4, F6.9, F7.1, F7.2, F7.3 onaylandı ve uygulandı (2026-11-28). Oturum 2: F5.7 "dokunma" cevabıyla kapandı;
F6.10, F7.5, F8.3 onayla uygulandı. Test yazılamayan düşük bulgular (F6.3, F6.5, F8.4) `HANDOVER.md` bölüm 4'te. Bulguların özgün metni "Bulgular" altındaki aşama
tablolarında; onay bekleyenlerin uzun açıklamaları için bu dosyanın git geçmişine bakın (482aeb9…4e0a7bd).

## Okunan dosyalar

Aşama 1 (tamamı satır satır): `crates/centurion-helpers/src/` lib.rs, field.rs, par.rs, wlog.rs, defaults.rs,
machine.rs, unlock.rs, distro.rs.

Aşama 2: `tune.rs` (kod kısmı 1–260, 1133–4266 satır satır; TUNABLES tablosu 261–1133 285 satırlık veri: anahtar
tekrarı, Int aralığı min≤max ve her satırda Target alanı mekanik olarak denetlendi, yardım metinleri okunmadı),
`bin/tune-helper.rs` (tamamı), `bin/tune-admin-helper.rs`, `bin/tune-profile-helper.rs`, `power.rs`, `pmtable.rs`.

Aşama 3: `crates/nvcurve` tamamı (27 dosya, 5 863 satır) satır satır.

Aşama 4: `intel_uv.rs`, `intel_uv_daemon.rs`, `intel_uv_search.rs`, `bin/intel-uv-helper.rs`,
`bin/centurion-intel-uv.rs` (tamamı); karşılaştırma için `stream.rs` ve `bin/ryzen-co-helper.rs:198-245`.

Aşama 5a: `smu.rs`, `ryzen_mon.rs`, `bin/ryzen-co-helper.rs`, `legion_wmi.rs`, `bin/legion-profile-helper.rs`,
`bin/legion-firmware-helper.rs`, `bin/legion-gpu-helper.rs`, `bin/legion-ec-sensors.rs`, `bin/fwattr-helper.rs` (tamamı).

Aşama 5b: `amdgpu.rs`, `bin/amdgpu-helper.rs`, `dgpu.rs`, `nvreg.rs`, `cooling.rs`, `fan_table.rs`, `lighting.rs`,
`bin/lighting-helper.rs`, `crates/centurion-spectrum/src/lib.rs`, `lamparray.rs` (tamamı).

Aşama 6a: `bin/centurion-autotune.rs` (tamamı); `autotune.rs` kod kısmı 1–2957 satır satır (testler 2957–4210 yalnızca
fikstür ve doğrulamalar için tarandı). Kural gövdelerindeki sezgisel katsayılar (etki vektörleri) alan kararı olduğu için
doğruluk açısından değerlendirilmedi; panik, indeksleme, bölme ve dosya G/Ç kalıplarına bakıldı.

Aşama 6b: `calctx.rs` (tamamı satır satır). `calib.rs`: dosya G/Ç ve depolar (imza yükle/kaydet/yedek, fit deposu,
ölçek önbelleği, 1180–1290, 1425–1470, 1778–1850) satır satır; istatistik kodu panik/indeksleme kalıplarıyla tarandı.
`bin/centurion-calibrate.rs` (3229 satır kod, büyük kısmı kırmızı çizgi: ayar yazma + günlük + geri yükleme): başlık,
`--field` içe aktarma ve imza yazma yolları okundu, geri kalanı risk kalıplarıyla tarandı (ölçüm/geri alma mantığı
değiştirilmeyeceği için ayrıntılı değerlendirilmedi). `calsim.rs` yalnızca test (centurion-calibrate'in test modülü):
gözden geçirildi.

Aşama 6c: `bootguard.rs`, `bin/centurion-boot-guard.rs`, `boothist.rs`, `stability.rs` (kod kısmı), `bin/centurion-cooltest.rs`
(tamamı satır satır); `bootlogo.rs` (kırmızı çizgi: UEFI değişkeni + ESP yazımı) tamamı okundu. `bench.rs` (1485 satır kod,
ölçüm): dosya oluşturma (yalnızca O_TMPFILE), eşleme ve indeksleme yolları okundu, ölçüm yöntemleri değerlendirilmedi.
`bin/centurion-gamemode.rs` (1673): pkexec çağrısı, ön ayar yükleme, sahne durumu / kilit yolları okundu; sahne uygulama
akışı (`apply_scene`, 691–1020) helper çağrılarının sırası olduğu için yalnızca tarandı.

Aşama 9 (kısmi): satır satır — `ksnap.rs`, `health.rs` (kod), `bin/health-reader.rs`, `wakeup.rs`, `pstore.rs` (kod),
`diagreport.rs` 715–870 (arşiv / anlık görüntü yazma), `flight.rs` 180–257, `iorate.rs` `probe`, `model.rs` 505–580 ve
735–775 (`unsafe` paylaşımlı matris yazımı: Cholesky ve çekirdek doldurma), `overview::state_dir` ve tüm çağıranları.
Yalnızca risk kalıplarıyla taranan (Command / dosya yazma / unsafe / unwrap): `model.rs`'in geri kalanı, `diagreport.rs`'in
geri kalanı, `health_nvme.rs`, `health_diagnostics.rs`, `overview.rs`, `memory_spd.rs`, `sleepdiag.rs`, `hibernate.rs`,
`trends.rs`, `tools.rs`, `display.rs`.

Aşama 8: `netguard.rs` (kod kısmı 1–2222 satır satır), `firewall.rs`, `traffic.rs`, `wifi.rs` (kod kısımları satır satır),
`bin/netguard-helper.rs`, `bin/centurion-netguard.rs`; GUI tarafı yalnızca `netpage.cpp` `runUser` zaman aşımı için.

Aşama 7: `sysadmin.rs`, `fstabcheck.rs`, `tarscan.rs` (kod kısımları satır satır), `bin/backup-helper.rs`,
`bin/sysadmin-helper.rs`, `bin/sysadmin-config-helper.rs`. `backup.rs`: yapılandırma geri yükleme yolu (1740–1960) ve yol
yardımcıları (233–270) satır satır; sistem imajı yedekleme/geri yükleme ve zincir geri yükleme (578–1420) ile
config_diff risk kalıplarıyla tarandı (fd'ye göre `unlinkat` / `renameat` kullanıyor).

## Bulgular

### Aşama 1 — helpers ortak altyapı

| # | Yer | Sorun / senaryo | Durum | Risk | Kırmızı çizgi |
|---|---|---|---|---|---|
| F1.1 | `distro.rs:291-294` | Zaman aşımında `kill(); wait()` sınırsız bekliyor ve okuyucu thread join ediliyordu. D durumunda takılan sorgu (asılı diskte rpm) ya da stdout'u tutan alt süreç → watchdog'u olmayan backup-helper (gözetimsiz otomatik yapılandırma yedeği) sonsuza kadar asılı. | **Düzeltildi** `b358b67`: `kill_reap` + başarısızlıkta join yok. Önce kıran test eklendi (5 s → 0,2 s). | düşük | hayır |
| F1.2 | `defaults.rs:93` `record` (çağıranlar `bin/tune-helper.rs:1162`, `:1219`) | outcomes.json kilitsiz oku-değiştir-yaz; iki çağrı da tune kilidinin dışında. Apply ile guard rollback eşzamanlıysa bir rollback sayımı kaybolur → "iki rollback adayı emekliye ayırır" emniyeti gecikir. | **Aşama 2'ye ertelendi** (çağıranlar orada; düzeltme yeri oraya bağlı) | orta | hayır (emniyeti sıkılaştırır) |
| F1.3 | `unlock.rs:309` `unhex` | `&s[i..i + 2]` bayt dilimi; pin.json'da çok baytlı karakter → yetki yolunda panic (abort). Dosya root/0600, yalnızca `set_pin` yazıyor. Öneri: `is_ascii()` kontrolü. | **Düzeltildi** `aaa6f01` | düşük | evet |
| F1.4 | `unlock.rs:105` `Failures::load` | `as u32` kesmesi: ≥2^32 sayaç küçük sayıya döner → kilit sıfırlanır. Yalnızca root yazıyor. Öneri: `.min(u32::MAX as u64)`. | **Düzeltildi** `9f8cb18` | düşük | evet |
| F1.5 | `unlock.rs:73` `write_private` | rename sonrası dizin fsync yok; elektrik kesilirse sayaç güncellemesi kaybolabilir (bir fazla deneme). Öneri: dizin fsync. | **Düzeltildi** `d9f5ca5` | düşük | evet |
| F1.6 | `distro.rs:276` `command_lines` | rpm/pacman/xbps-query root olarak yalnızca `is_file()` ile çalışıyor (`modprobe` `trusted_path` kullanıyor); NixOS dalı ortamdaki `USER`'ı yola koyuyor (zararsız salt okuma). Öneri: `trusted_path` filtresi. | **Düzeltildi** `a590cdf` | düşük | evet |
| F1.7 | `lib.rs:384` `FdLock` | Ham fd tutuyor, File'ı ödünç almıyor; ters sırada bırakılırsa yeniden kullanılmış fd'ye `LOCK_UN`. Bugünkü 4 kullanım doğru. Öneri: `FdLock<'a>` + PhantomData. | **Düzeltildi** `538f2b9` | düşük | evet |

Sorunsuz bulunanlar: field.rs, par.rs, wlog.rs, machine.rs (tüm dilimleme ASCII sınırlarında, panik yolu yok),
lib.rs'in geri kalanı (`write_root_file` atomik: O_EXCL tmp + fsync + rename + dizin fsync; `read_root_bytes` kontrolleri
açılmış fd üzerinde → TOCTOU yok).

Doğrulama (b358b67): cargo test 727 geçti / 0 kaldı / 9 ignored · clippy temiz · GUI build + ctest 77/77 · python 45/45.

### Aşama 2 — tune.rs, tune-helper, power, pmtable

| # | Yer | Sorun / senaryo | Durum | Risk | Kırmızı çizgi |
|---|---|---|---|---|---|
| F2.1 | `tune.rs:3821` `describe_with` | `irqbalance_running()` her IRQ satırında (4 satır) tekrar → her light describe'da (GUI 4 sn'de bir) 4 tam /proc taraması (süreç başına bir `comm` okuması), artı `optimization_conflicts` içinde bir tane. | **Düzeltildi** `fc31e8b`: döngüden önce bir kez. Çıktı aynı. | düşük | hayır (salt okuma) |
| F1.2 | `defaults.rs:93` `record` | Kilitsiz oku-değiştir-yaz; iki çağıran da tune kilidi dışında → eşzamanlı apply + guard rollback'te bir sayım kaybolabilir. | **Düzeltildi** `7103c33`: `helper_lock("outcomes")` (alınamazsa eskisi gibi). Test yok: sabit /var/lib yolu + uid 0 şartı CI'da kurulamıyor. | orta | hayır |
| F2.2 | `bin/tune-helper.rs:607` | Her storage dosyası için tüm baseline `canonicalize()` → O(plan×baseline) syscall, kilit altında. | **Düzeltildi** `712d795`: satır başına bir kez takma ad kümesi; koşul eşdeğer. | düşük | evet (onaylı) |
| F2.3 | `bin/tune-helper.rs` `spawn_guard` | Çok thread'li süreçte `fork()` sonrası async-signal-safe olmayan guard gövdesi → kilitlenme, rollback olmaz. | **Düzeltildi** `553837d`: guard aynı binary'nin yeniden başlatılması (stdin'de iş, iç op `guard_run`; pkexec ile ve root olmayan çağrıda reddedilir). Gövde aynı. Uçtan uca deneme bu ortamda yapılamadı (`init()` Lenovo olmayan makineyi reddediyor). | orta | evet (onaylı) |
| F2.4 | `tune.rs:3580` `forked_firmware_write` | Çocuk yalnızca 0..4096 fd'lerini kapatıyordu. | **Düzeltildi** `bdde6b6`: `close_range`, eski döngü yedek. Kıran test (fd 5000) eklendi. | düşük | evet (onaylı) |
| F2.5 | `tune.rs:1597` `powerd_pids` | comm ile tanıma; `trusted_path` kötüye kullanımı engelliyor. | Bilgi — değişiklik yok | — | evet |

Sorunsuz bulunanlar / gözlemler: `power.rs`, `pmtable.rs` (dilimler `get` ile sınırlı). tune.rs'te dilimlemeler ASCII
önekten sonra (`ratio:`, `K`/`M`, `nvme`) → panik yolu yok. `describe` light modu yavaş satırları (iw/ethtool/hdparm/PCI
config) zaten atlıyor. `write_checked_inner`'ın /proc izin listesi ve sysfs kanonikleştirmesi sağlam. Yazma sonrası
okuma doğrulaması "dosyayı son yazan satır" kuralıyla doğru.

Doğrulama (fc31e8b): cargo test 727 / 0 / 9 ignored · clippy temiz · GUI build + ctest 77/77 · python 45/45.
Onaylı düzeltmelerden sonra (553837d): cargo test 733 / 0 / 9 ignored · clippy temiz · ctest 77/77 · python 45/45.

### Aşama 3 — nvcurve

| # | Yer | Sorun / senaryo | Durum | Risk | Kırmızı çizgi |
|---|---|---|---|---|---|
| F3.1 | `nvcurve/src/proc.rs` `run` | Araç çıktıktan sonra borusunu tutan alt süreci de bekliyordu (tek kullanıcı: nvidia-smi güç limiti yedeği); zaman aşımı dalı `kill_reap` kopyası. | **Düzeltildi** `c249cca`: paylaşımlı tampon + çıkıştan sonra en çok 500 ms EOF beklemesi, `kill_reap`. Kıran test eklendi (5 s → anında). | düşük | evet (onaylı) |
| F3.2 | `nvcurve-root-helper.rs` `calibration_hold` | Ortak fonksiyonun kopyası, gerekçe yorumu eskimiş; pid 1 için farklı sonuç. | **Düzeltildi** `e5d63d6`: ortak fonksiyon `pid > 1` filtresini aldı, kopya kaldırıldı; sahip istisnası eklenmedi (davranış aynı). | düşük | evet (onaylı) |
| F3.3 | `nvcurve/src/ops.rs` `persistent_load` | Sınırsız, symlink izleyen okuma. | **Düzeltildi** `81d7f55`: `read_regular` + `MAX_CONFIG_BYTES`; hata yine uyarı + boş. | düşük | evet (onaylı) |
| F3.4 | `nvcurve/src/ocjournal.rs` | Ortak journal'ın kopyası. | **Düzeltildi** `5255ff0`: ince sarmalayıcı (aynı satır, aynı dosya; daha sıkı dizin/dosya denetimi). | düşük | hayır |
| F3.5 | `hal/limits.rs:141` | Ortam değişkeniyle API sırası. | Bilgi — değişiklik yok | — | — |

Sorunsuz bulunanlar / gözlemler: dilimlemeler ve indekslemeler ya sabit boyutlu sürücü tamponlarında ya da önceden
uzunluğu denetlenmiş verilerde (`Buf::u32_at` çağıranlarının hepsi sınır içinde). Yazma yolları (vfcurve, apply, ops,
snapshot) yazmadan önce iki bağımsız okuma, kimlik denetimi, maske denetimi ve yazma sonrası birebir geri okuma yapıyor;
başarısızlıkta tek geri alma denemesi. Static testler (tests/nvcurve_safety_static_test.py) bu crate'i yoğun biçimde
metin kalıbıyla sabitliyor.

Doğrulama (5255ff0): cargo test 734 / 0 / 9 ignored · clippy temiz · ctest 77/77 · python 45/45.

### Aşama 4 — Intel undervolt

| # | Yer | Sorun / senaryo | Durum | Risk | Kırmızı çizgi |
|---|---|---|---|---|---|
| F4.1 | `bin/intel-uv-helper.rs` | `main`'in 120 sn watchdog'u 4 saatlik `monitor_stream`'i 2 dakikada öldürüyordu; GUI her seferinde yeni pkexec başlatıyordu. | **Düzeltildi** `34bbec0`: akış dalı `alarm(0)` ile watchdog'u kaldırıyor (ryzen-co-helper gibi); istek okuma ve diğer op'lar 120 sn sınırında. Otomatik test yok (helper Lenovo dışında başlamıyor). | düşük | evet (onaylı) |
| F4.2 | `intel_uv.rs` `stock_note` | intel-stock.json kilitsiz RMW; eşzamanlı ilk kayıtlarda bir alan kaybolabiliyordu. | **Düzeltildi** `9a9b843`: `helper_lock("intel-stock")`. | düşük | evet (onaylı) |
| F4.3 | `intel_uv_search.rs` `stress_bin` | PATH'teki cpustress root olarak çalışabiliyordu. | **Düzeltildi** `1e85319`: yanındaki / /usr/bin / /usr/local/bin, yalnızca `trusted_path`; yoksa "çalışamadı" (çıkış 3, son iyi değere dönüş). | düşük | evet (onaylı) |
| F4.4 | `intel_uv.rs` `monitor_sample`, `read_status` | Örnek başına CPU başına msr açma; POWER_CTL iki kez okunuyor. | Bilgi — değişiklik yok | — | — |

Doğrulama (1e85319): cargo test 734 / 0 / 9 ignored · clippy temiz · ctest 77/77 · python 45/45.

Sorunsuz bulunanlar / gözlemler: her yazma "oku → yaz → geri oku" ve atomik modda ters sırada geri alma yapıyor;
mailbox işlemleri msr fd'sinde flock ile serileştiriliyor; profil ayrıştırma tüm aralıkları yazmadan önce denetliyor
(cTDP 0..2 olduğu için `["nominal","down","up"][level]` indekslemesi güvenli). MMIO açılışı hizalama, sayfa sınırı ve
/proc/iomem "System RAM" denetimi yapıyor.

### Aşama 5a — Ryzen/SMU, Legion WMI/EC, fwattr

Uygulanan değişiklik yok (bulguların hepsi kırmızı çizgi alanında; F5.1–F5.3 "Bekleyen onaylar"da).

Sorunsuz bulunanlar: SMU mailbox'ı yalnızca sabit tablodan opcode gönderiyor, 1 sn sınırlı yoklama ve self-test var;
CO toplu yazma önce tümünü doğruluyor, hata olursa geri alıyor ve durumu ("original/stock/unknown") dürüstçe bildiriyor.
Legion WMI yazmaları "önce oku, yaz, geri oku" ve firmware-güvenli sırada (`fw_write_order`); fwattr-helper yolu biçim +
Lenovo + kanonik konum + canlı aralık ile denetliyor, toplu yazmada orijinalleri yakalayıp geri alıyor.

### Aşama 5b — amdgpu, dGPU, nvreg, cooling, fan tablosu, aydınlatma

Uygulanan değişiklik yok. F5.4 "Bekleyen onaylar"da; aşağıdakiler yalnızca bilgi (öneri yok ya da kırmızı çizgi):

| # | Yer | Gözlem | Risk | Kırmızı çizgi |
|---|-----|--------|------|---------------|
| F5.5 | `dgpu.rs:118-131` `in_child` | Süre dolunca çocuğu `SIGKILL`'liyor ama `waitpid` ile toplamıyor; helper çıkana dek zombi kalır. D durumundaki (modül boşaltma/PCI rescan) bir çocuk zaten toplanamaz; helper kısa ömürlü. Değişiklik önerilmez. | yok | evet (sürücü/PCI yolu) |
| F5.6 | `cooling.rs:382` `load_fanmax` | `load_runs` (`:363`) 4 MiB sınırı koyuyor, `load_fanmax` koymuyor. Kullanıcının kendi dosyası, kullanıcı süreci okuyor; yalnızca tutarsızlık. | çok düşük | hayır |
| F5.7 | `nvreg.rs:83` `Kind::Dwords` | İzin verilen karakterlerde `,` var ama yalnızca anahtar adında işe yarıyor (değer `num()`'dan geçmeli). NVIDIA'nın `RegistryDwords` söz diziminde `,` gerekip gerekmediğinden emin değilim — soru, değişiklik önerilmez. | yok | evet (doğrulama) |
| F5.8 | `centurion-spectrum/src/lib.rs:520` `Device::open` | `flock(LOCK_EX)` süresiz bekliyor; takılan bir eş (ör. gamemode) lighting-helper'ı bekletir, lighting-helper'da watchdog yok. Tüm kullanıcılar kısa ömürlü olduğu için pratikte sorun değil. | düşük | hayır |

Sorunsuz bulunanlar: amdgpu `apply` her değeri yazmadan önce doğruluyor (OD tablosu sürücünün aralığıyla + 4000 MHz /
1300 mV akıl sınırı, voltaj ofseti yalnızca aşağı, fan eğrisi düşmeyen + sıcak noktada asgari fan), hata olursa en
yeniden eskiye geri alıp neyin geri alınamadığını söylüyor. dGPU `release` önce sahipleri buluyor, durdurduğu
servisleri ret durumunda geri başlatıyor; sürücü/PCI adımları süreli çocukta. nvreg yalnızca `modinfo`'nun bildirdiği,
tek anlamlı seçenekleri yazıyor; tip aralığı dışı ya da metin/dizi tipleri reddediliyor. fan_table her yazmayı geri
okuyup uyuşmazlıkta eski tabloyu geri yüklüyor; son üç adım için termal taban var. Spectrum yalnızca 960 baytlık feature
raporu bildiren ve D1'e yanıt veren arayüzle konuşuyor; LampArray akışı sinyal/çıkışta otonom moda dönüyor.

### Aşama 6a — autotune

| # | Yer | Sorun | Senaryo | Öneri / yapılan | Risk | Kırmızı çizgi |
|---|-----|-------|---------|-----------------|------|---------------|
| F6.1 | `autotune.rs:234`, `:1400`, `:2535` | Üç doc yorumu yanlış öğeye bağlı: `game_sessions`'ınki `project_c_under`'da, `level_costs`'unki `poll_names`'te, `autotune_req`'inki `progress_event`'te. | rustdoc / IDE yanlış açıklamayı gösteriyor. | Yorumlar ait oldukları öğelere taşındı (271b502). Kod değişmedi. | yok | hayır |
| F6.2 | `autotune.rs:1455` `joint_pass` | `r.joint.take()` sonrası `Rules::seen` için model yok; önbellekte olmayan bir anahtar için `set` → `seen()` `true` döner. `joint_pass`'in kendi `set_live`'ları ve `repair_live` bu yoldan geçer. | Kâğıt üstünde: `signature_pass`'te `calibrated_choice` erken `false` dönmüş (ölçülmüş değer yok) bir eklem-model anahtarını eklem araması değiştirirse karar `seen: true` raporlanır. Mevcut eklem testinde (`joint_model_keeps_synergy_and_drops_redundancy`) geçici bir yoklamayla denedim: tüm anahtarlar önceden önbellekteydi, uyuşmazlık çıkmadı. Ulaşılabilir bir girdi bulamadım. | Kanıt yok, değişiklik yapılmadı. Ulaşılabilirse çözüm: `joint_pass` sonunda `r.joint`'i geri koymak. | düşük (yalnızca rapor alanı) | hayır |
| F6.3 | `bin/centurion-autotune.rs:161` `write_atomic` | Geçici dosya adı süreç başına değil (`.<ad>.tmp`). `history_push` aynı sorun için süreç kimlikli `write_replace`'e geçirilmiş; `--save` geçirilmemiş. | Aynı adla aynı anda iki `--save` (GUI + CLI) aynı geçici dosyaya yazar. | Yarış için güvenilir bir test yazılamıyor; yalnızca rapor. | çok düşük | hayır |
| F6.4 | `bin/centurion-autotune.rs:246` `report` | Watermark sayfa boyutu sabit 4 KiB. | Yalnızca 4 KiB dışı sayfa boyutlu çekirdeklerde tanı çıktısı yanlış olur (hedef x86). | Değişiklik önerilmez. | yok | hayır |

Sorunsuz bulunanlar: `centurion-autotune --weights` sıkı ayrıştırılıyor (sonlu, bilinen anahtar, tekrar yok). Kullanıcı
dosyası okuyan kök yolu (`read_regular`) O_NOFOLLOW + O_NONBLOCK + fstat ile korunuyor. Kanıt geçmişi `read_root_file` /
`write_root_file` kullanıyor. Çıkan her ön ayar kendi `audit`'inden geçiriliyor; 2^53 üstü tamsayılar atılıyor.
`guard_verdict` (geri alma kuralı) ve `audit` / `enforce` kırmızı çizgi alanı: yalnızca okundu.

### Aşama 6b — kalibrasyon

| # | Yer | Sorun | Senaryo | Öneri | Risk | Kırmızı çizgi |
|---|-----|-------|---------|-------|------|---------------|
| F6.5 | `bin/centurion-calibrate.rs:2476`, `:2484` | Eski `--oat` yolu imzayı `write_root_file` ile doğrudan yazıyor; `Calibration::save()`'i (ilk yazımda `.bak` yedeği, `MAX_FILE`'a yaklaşınca eski satırları kırpma) atlıyor. Ana tasarım yolu (`:3031`) `save()` kullanıyor. | `--oat` sonrası bozulan bir imza için yedek yok; dosya 8 MiB'ı aşarsa okuyucu "hasarlı" der. `--oat` yalnızca kayıt ekliyor (değer başına 10), bu yüzden pratikte küçük. | İki çağrıyı `cal.save()` ile değiştirmek. Sabit `/var/lib` yoluna kök olarak yazdığı için birim testi yazılamıyor; uygulanmadı. | düşük | hayır |
| F6.6 | `overview.rs:114` `state_dir` (ayrıca `cooling.rs:332`) | `XDG_STATE_HOME` ve `HOME` yoksa `/tmp/centurion`'a düşüyor. `autotune_req` tune-helper'dan kök olarak da çalışıyor (`autotune` op'u) ve oraya `write_replace` ile yazıyor. pkexec `HOME=/root` koyduğu için bugün güvenli. `HOME`'suz bir kök servis bu yolu çağırırsa dünya-yazılabilir `/tmp` altında sembolik bağlantıya açık olur. | Bugünkü çağıranlarda ulaşılamıyor. | Aşama 9'da (overview) `state_dir` çağıranları kök bağlamı açısından denetlenecek. | bilgi | hayır |
| F6.7 | `calctx.rs:124-157` `home_of` / `uid_of` / `ids_of` | Ad **ya da** uid ile eşleşiyor, ilk satır kazanıyor; adı başka bir hesabın uid'i olan bir kullanıcı ("1000") yanlış hesaba çözülebilir. | Çok nadir yapılandırma. | Değişiklik önerilmez; not. | çok düşük | hayır |

Sorunsuz bulunanlar: sahne dosyası kök tarafından kullanıcının fsuid'iyle çözülüyor (`FsAs`), O_NOFOLLOW + açık fd
üzerinde sahip/boyut denetimiyle okunuyor; açılış zamanlaması sahnenin güç kısmını onay anında köke kopyalıyor.
`apply_scene_power` önce tam orijinali günlüğe yazıyor, eksik bir orijinalde hiç yazmıyor; geri yükleme hata olursa
günlüğü tutuyor. `--field` kullanıcı günlüklerini O_NOFOLLOW + sahip denetimiyle okuyor. İmza yüklemesi bozuk dosyayı
"başka donanım"dan ayırıyor ve yedeğe düşüyor.

### Aşama 6c — bench, stability, cooltest, gamemode, boot*

| # | Yer | Gözlem | Risk | Kırmızı çizgi |
|---|-----|--------|------|---------------|
| F6.8 | `bin/centurion-cooltest.rs:177` `fanmax` | `--peaks nan` kabul ediliyor (`f64::parse`); JSON'a `null` yazılıyor ve o ayar için taban kalıcı olarak "fan sayısı farklı" oluyor (`forget` gerekir). GUI (`coolingpage.cpp:584`) yalnızca `qRound` tamsayıları gönderiyor; yalnızca elle CLI çağrısında olur. "İmkânsız durum" kuralı gereği değişiklik önerilmez. | çok düşük | hayır |
| F6.10 | `bootguard.rs:29` `read` | Okunamayan / bozuk durum dosyası `{}` döner → `should_skip` `None` → ön ayarlar uygulanır (açık kalarak başarısız). Bilinçli olabilir (bozuk bir dosya açılışları kalıcı olarak durdurmasın diye). Soru: niyet bu mu? | bilgi | evet (koruma mantığı) |
| F6.11 | `autotune.rs:376` `boot_id` | `bootguard::boot_id`'in birebir kopyası (`stability.rs` ikincisini kullanıyor). Davranış farkı yok. | yok | hayır |

Sorunsuz bulunanlar: boot guard sinyalleri `sigtimedwait` ile alıyor (kontrol ile bekleme arasında SIGTERM kaçmaz);
bootlogo yalnızca tam çözünürlükte temel JPEG kabul ediyor, her UEFI yazımını geri okuyup başarısızlıkta öncekileri geri
koyuyor. bench G/Ç testleri bağlantısız O_TMPFILE kullanıyor (dünya-yazılabilir `/var/tmp`'de bile sembolik bağlantıya
açık ad yok). gamemode kök gerektiren her şeyi pkexec ile yapıyor; ön ayar değerleri yalnızca köke ait depodan
(`/etc/centurion/presets`) alınıyor, kullanıcının dosyası değil; yetkilendirme takılırsa 60 sn, helper 180 sn sınırı var.

### Oturum 2'de onayla uygulanan eski bulgular

| # | Değişiklik | Test |
|---|---|---|
| F6.10 | `bootguard.rs`: dosya var ama okunamıyor/bozuksa `should_skip` atlıyor, `arm` korumayı "damaged" gerekçesiyle tetikliyor; dosya yoksa (ilk açılış) eskisi gibi boş durum. Uygulamada "Resume" (`reset`) yeni durum yazıyor. | `damaged_state_is_not_an_empty_one` (önce derlenmiyordu) |
| F7.5 | `sysadmin.rs` udev denetimi: eklenen satırlarda `MODE` / `OWNER` / `GROUP` reddediliyor. Not: `TAG+="uaccess"` (oturumdaki kullanıcıya erişim) hâlâ kabul ediliyor ve mevcut testte izinli örnek olarak duruyor. | mevcut red testine üç vaka (önce kırmızıydı) |

### Aşama 7 — yedekleme ve sistem araçları

| # | Yer | Gözlem | Risk | Kırmızı çizgi |
|---|-----|--------|------|---------------|
| F7.4 | `sysadmin.rs:417` `config_restore` | Geri yüklenen yedek `new_lines_allowed`'dan ve fstab'da `fstabcheck`'ten geçmiyor (`config_write` içindeki `validate` ve `findmnt --verify` çalışıyor). Yedekler kökün kendi kopyaları; yalnızca yasak listesinden önce yazılmış bir satır geri gelebilir. | çok düşük | evet |
| F7.5 | `sysadmin.rs:174` udev denetimi | Yalnızca RUN / PROGRAM / IMPORT{program} reddediliyor. `MODE` / `OWNER` / `GROUP` (ör. `KERNEL=="mem", MODE="0666"`) bir çekirdek korumasını aynı ölçüde düşürür. Her seferinde yönetici parolası. Soru: bilinçli olarak mı dışarıda bırakıldı? | düşük | evet |

Doğrulanan (değişiklik gerektirmeyen): `extract_limited` arşivi kök olarak açarken, arşivdeki bir dizin bağlantısının
(`system/etc -> /hedef`) ardından gelen bir üye o bağlantının üzerinden yazabilir mi? GNU tar 1.35 ile scratchpad'de
denedim: tar "Cannot open: Not a directory" diyerek reddediyor, hedef boş kalıyor. Ardından `plain_tree` her bağlantıyı
ve özel dosyayı reddediyor. Kullanıcı geri yüklemesi sahnelerin `command` alanını atıyor; kök geri yüklemesi modprobe.d
dosyasını kopyalamıyor, `nvreg::restore_from` ile yeniden doğruluyor. tarscan tanımadığı her başlıkta akışı "broken"
işaretleyip silme/yeniden adlandırmayı kapatıyor.

### Aşama 8 — ağ koruması, güvenlik duvarı, trafik, Wi-Fi

| # | Yer | Gözlem | Durum | Risk | Kırmızı çizgi |
|---|-----|--------|-------|------|---------------|
| F8.1 | `traffic.rs` `Ledger::from_json` | Bir günde 256 program + "(other)" varken (bunu `add` üretiyor) dosya geri okunduğunda alfabetik olarak sonuncu program düşüyordu (daemon yeniden başlatma, sayımın yeniden açılması). Ayrıca kısıt kesilmemiş adla denetleniyordu. | **Düzeltildi** `63d5077`: yükleme `add` ile aynı sınırı uyguluyor. `full_day_round_trips` (önce kırmızıydı). | düşük | hayır (sayım verisi) |
| F8.2 | `netguard.rs:1013`, `:1019-1020` `decide` | Ayrıştırılamayan paket ya da bulunamayan soket → kabul (paket başına açık kalarak başarısız). Kopya aralığı 256 bayt; IPv6 uzantı başlıklarını bu sınırın ötesine itmek CAP_NET_RAW istiyor (yetkisiz RTHDR tip 2 yalnızca 24 bayt), bu yüzden Wine programı için ulaşılamıyor. | Bilgi — değişiklik yok | düşük | evet (koruma kararı) |
| F8.3 | `netguard.rs:1447` yeniden yükleme | Trafik sayımı açık ve conntrack soketi açılamıyorsa her yeniden yükleme başarısız oluyordu: güvenlik duvarı / kara liste değişiklikleri sayım kapatılana dek uygulanmıyordu (açılışta aynı hata tolere ediliyordu). | **Düzeltildi** (onaylı) `14b08ad`: kurallar yükleniyor, hata durum dosyasına yazılıyor. Test yok (kök + netlink daemon döngüsü). Not: helper `state["error"]` doluyken yeni engelleme kurallarının açık bağlantılarını kesmiyor (`:1962`); bu durumda da öyle. | düşük | evet (onaylı) |
| F8.4 | `netguard.rs:1432` açılış | `sync_acct` başarısız olursa `write_state` önceki `start_error`'ı (ör. "firewall not applied") eziyor; GUI yalnızca sayım hatasını gösterir. | Bilgi — test yazılamıyor (daemon) | çok düşük | hayır |
| F8.5 | `bin/netguard-helper.rs` | Diğer kök helper'ların çoğunda olan `watchdog` yok. Bloklayabilecek yerler: `forget_blocked_flows` zaman aşımsız netlink dump okuması, nft alt süreci, whois DNS çözümü. Kullanıcı yolu GUI'de `timeoutMs` ile öldürülüyor; kök yolu `privileged::run`. Pratikte dump her zaman DONE ile bitiyor. | Bilgi | çok düşük | hayır |
| F8.6 | `firewall.rs:281` `move` | `i as i64 + by` taşması: release'de sarıyor → `to < 0` → işlem yok; debug'da panik. Yalnızca elle yazılmış istek. | Bilgi — "imkânsız durum" | yok | evet (doğrulama) |

Sorunsuz bulunanlar: güvenlik duvarı kuralının her alanı nft'ye gitmeden önce tipli değere ayrıştırılıyor (arayüz adı
izin listesi, port aralıkları birleştiriliyor, not nft'ye hiç gitmiyor); `firewall` isteği kaydetmeden önce `nft -c` ile
deneniyor ve `expected_firewall` kilit altında karşılaştırılıyor. Tablo tek nft işlemiyle değiştiriliyor; kuyruk kuralları
açılmadan önce bağlanıyor; Wine koruması `bypass`'sız kuyrukla kapalı başarısız. Netlink ayrıştırıcıları (`nl_each`,
öznitelik döngüleri) uzunluk denetimli; sock_diag yanıtları sıra numarasıyla eşleniyor. Günlükler 0640 root:wheel,
O_NOFOLLOW. whois yalnızca `whois.` ile başlayan geçerli adlara, genel adreslere, süre ve boyut sınırıyla bağlanıyor.
Wi-Fi: yetkisiz, salt okuma; araçlar `trusted_path` + temiz ortamla; arayüz adı yalnızca /sys/class/net listesinden.
Kanal hesaplarında işaretsiz çıkarmaların hepsi önceden korunmuş.

### Aşama 9 — teşhis / sağlık (kısmi)

| # | Yer | Gözlem | Risk | Kırmızı çizgi |
|---|-----|--------|------|---------------|
| F6.6 | `overview.rs:114` `state_dir` | Kapandı: kök olarak çağıranlar (`autotune_req`, `trends`, `diagreport` önizlemesi) yalnızca pkexec ile tune-helper'dan geliyor, pkexec `HOME=/root` koyuyor; servis yolları (`centurion-tune.service` → `boot`, paket kancası → `restore`) `state_dir`'a ulaşmıyor. `diagreport` yazımı kökte zaten reddediliyor. `cooling.rs`'in kopyası yalnızca kullanıcı süreci (cooltest). | yok | hayır |
| F9.1 | `ksnap.rs:40` `DENY_PREFIXES` | Eski `firmware/efi/vars` arayüzü (CONFIG_EFI_VARS, yeni çekirdeklerde kaldırıldı) listede yok; `efivars` var. İçerik çoğunlukla ikili (atlanıyor), metin olanlar GUI'ye dönebilir. | çok düşük | evet (izin listesi) |
| F9.2 | `bin/health-reader.rs` | `watchdog(30)` yalnızca `ksnap`'te; `nvme_deep`, `diagnostics`, `boots`, `sleep_diag`, `battery_info` (acpi_call) kökte süre sınırsız. | çok düşük | hayır |
| F9.3 | `bin/health-reader.rs:8` doc | "The scan never returns raw log lines" — olaylar `text` alanında sınıflanan satırın ilk 300 karakterini (adresler temizlenmiş) taşıyor. Sessiz polkit eyleminin gerekçesi bu cümleye dayandığı için yorum değiştirilmedi, raporlandı. | bilgi | evet (polkit gerekçesi) |

Sorunsuz bulunanlar: ksnap sembolik bağlantıları izlemiyor, kökleri `canonical_in_sysfs` ile /sys içine sabitliyor, debugfs'i
alt ağaç izin listesiyle açıyor, O_NONBLOCK okuyor, değerleri `scrub`'dan geçiriyor ve kilit açık pencere istiyor.
`scrub` ve kmsg ayrıştırıcıları dilimlemelerde karakter sınırında. pstore arşivi önce yazıp sonra siliyor, adları
temizliyor. diagreport arşivi ve anlık görüntüsü yalnızca kullanıcı olarak, O_EXCL geçici dosya + `RENAME_NOREPLACE`
ile yazılıyor; anlık görüntü okuması sahip / mod / nlink / sürüm denetimli. iorate yazma sınaması bağlantısız O_TMPFILE.
model.rs `unsafe` paralel yazımları satır sahipliğiyle ayrık; Cholesky'de hata bayrağı engelden önce yazılıp sonra
okunuyor, tüm işçiler aynı noktada dönüyor.

## Ölü kod adayları

Aşama 1: yok (tüm `pub` öğelerin çağıranı var).
Aşama 2: `tune.rs:2875` `sched_feature_state_for_test` — `#[cfg(test)] pub fn`, hiçbir yerde çağrılmıyor (testler doğrudan
`sched_feature_state`'i kullanabiliyor). Silinmedi.

Aşama 3: yok. (Kopyalar F3.2 ve F3.4'te; bunlar ölü değil, tekrar.) Not: `nvcurve/src/hal/sensors.rs:13-17`
modül yorumunda kopuk boş `//!` satırları var (yalnızca biçim, dokunulmadı).

Aşama 4: yok.

Aşama 5a: `bin/legion-gpu-helper.rs:116` kalibrasyon listesindeki `set_gpu_mode` / `set_fw_oc` girdileri (bu binary
o op'ları sunmuyor). Silinmedi.

Aşama 5b: `centurion-spectrum` kütüphanesinin çalışma alanında tüketicisi olmayan `pub` API'si (yalnızca kendi
testleri kullanıyor ya da hiç kullanılmıyor): `EffectType::{name, needs_host, has_speed, has_direction, has_clockwise,
has_colors}`, `Effect::kind`, `per_key_effects`, `KeyMap::{at, unique, zone_of, zone, spans}`, `Zone`, `is_perimeter`,
`PERIMETER_REAR/FRONT`, `Device::restore_effects_raw` (hiç çağrılmıyor). GUI C++ olduğu için bunları kullanamaz; eski bir
Rust GUI'nin ya da yedekleme özelliğinin kalıntısı olabilir. Silinmedi.

Aşama 6a: yok (`explore` yalnızca `#[cfg(test)]`, bilinçli).

Aşama 6b: yok (`calsim.rs` `#![allow(dead_code)]` ile test simülatörü, bilinçli).

Aşama 6c: yok (F6.11 bir kopya, ölü değil).

Aşama 7: yok.

Aşama 8: yok. (`netguard`/`wifi`/`traffic`'te dışarıdan kullanılmayan birkaç `pub` öğe var — `parse_*`, `find_socket`,
`KCONFIG`, `DAYS_KEPT` … — hepsinin modül içinde çağıranı var; yalnızca görünürlük gereğinden geniş.)

Aşama 9 (kısmi): yok.
