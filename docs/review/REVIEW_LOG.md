# Centurion kod incelemesi — çalışma günlüğü

Her oturum önce bu dosyayı okur, "Durum" bölümünden devam eder.
Kurallar: davranış korunur; kırmızı çizgi alanları (helpers doğrulama/clamp/izin listeleri,
donanıma yazma sırası/trial-revert, polkit/udev/systemd/openrc, install/uninstall) yalnızca raporlanır.
Python static testleri kaynakta metin arıyor (≈441 assert); bunları kıran bir refactor durdurulup sorulur.

## Durum

- Aşama 0: TAMAMLANDI (onaylandı 2026-11-28).
- Aşama 1 (helpers ortak altyapı): TAMAMLANDI — 1 düzeltme uygulandı, 5 kırmızı çizgi bulgusu onay bekliyor.
- Sıradaki: Aşama 2 (tune.rs + tune-helper + tune-admin/profile-helper + power/pmtable). Önce F1.2'yi orada ele al.

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

## İnceleme planı (onay bekliyor)

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

Kırmızı çizgi alanında; uygulanmadı, onay bekliyor:

- **F1.3** `unlock.rs:309` `unhex`: `&s[i..i + 2]` bayt dilimi; pin.json'daki `salt`/`hash` çok baytlı bir karakter içerirse
  panic (release'de abort) — yetki yolunda. Dosya root/0600, yalnızca `set_pin` yazıyor → kullanıcı tetikleyemez.
  Öneri: `if !s.is_ascii() { return Vec::new(); }` (dönüş zaten "boş = geçersiz" anlamında). Risk: düşük.
- **F1.4** `unlock.rs:105` `Failures::load`: `as_u64() as u32` kesmesi; 2^32 ve üstü sayaç küçük bir sayıya döner ve
  kilit sıfırlanmış olur. Yalnızca root yazıyor. Öneri: `defaults.rs:87`'deki gibi `.min(u32::MAX as u64)`. Risk: düşük.
- **F1.5** `unlock.rs:73` `write_private`: rename'den sonra dizin fsync yok (`lib.rs::write_root_file` yapıyor).
  Yanlış PIN'den hemen sonra elektrik kesilirse sayaç güncellemesi kaybolabilir → bir deneme fazladan. Öneri: dizin fsync. Risk: düşük.
- **F1.6** `distro.rs:276` `command_lines`: rpm/pacman/xbps-query backup-helper içinde root olarak çalışıyor, yalnızca
  `is_file()` kontrolüyle; `lib.rs::modprobe` aynı iş için `trusted_path()` kullanıyor. Normal sistemde istismar edilemez
  (/usr/bin root'un). Ayrıca NixOS dalı ortamdaki `USER`'ı yola koyuyor (lib.rs başlığı "ortama güvenilmez" diyor);
  `USER=..` → `/etc/profiles/bin` listelenir, zararsız salt okuma. Öneri: `trusted_path` filtresi. Risk: düşük.
- **F1.7** `lib.rs:384` `FdLock`: ham fd tutuyor, dosyayı ödünç almıyor. Bir çağıran File'ı kilitten önce düşürürse Drop,
  yeniden kullanılmış bir fd üzerinde `LOCK_UN` yapar. Bugünkü 4 kullanım (intel_uv, memory_spd, wlog, legion_wmi) doğru
  sırada. Öneri: ömür parametresi (`FdLock<'a>` + PhantomData) ile derleyiciye zorlatmak. MSR/acpi_call serileştirmesi →
  kırmızı çizgi; davranış değişmez. Risk: düşük.

## Okunan dosyalar

Aşama 1 (tamamı satır satır): `crates/centurion-helpers/src/` lib.rs, field.rs, par.rs, wlog.rs, defaults.rs,
machine.rs, unlock.rs, distro.rs.

## Bulgular

### Aşama 1 — helpers ortak altyapı

| # | Yer | Sorun / senaryo | Durum | Risk | Kırmızı çizgi |
|---|---|---|---|---|---|
| F1.1 | `distro.rs:291-294` | Zaman aşımında `kill(); wait()` sınırsız bekliyor ve okuyucu thread join ediliyordu. D durumunda takılan sorgu (asılı diskte rpm) ya da stdout'u tutan alt süreç → watchdog'u olmayan backup-helper (gözetimsiz otomatik yapılandırma yedeği) sonsuza kadar asılı. | **Düzeltildi** `b358b67`: `kill_reap` + başarısızlıkta join yok. Önce kıran test eklendi (5 s → 0,2 s). | düşük | hayır |
| F1.2 | `defaults.rs:93` `record` (çağıranlar `bin/tune-helper.rs:1162`, `:1219`) | outcomes.json kilitsiz oku-değiştir-yaz; iki çağrı da tune kilidinin dışında. Apply ile guard rollback eşzamanlıysa bir rollback sayımı kaybolur → "iki rollback adayı emekliye ayırır" emniyeti gecikir. | **Aşama 2'ye ertelendi** (çağıranlar orada; düzeltme yeri oraya bağlı) | orta | hayır (emniyeti sıkılaştırır) |
| F1.3–F1.7 | unlock.rs, distro.rs, lib.rs | bkz. "Bekleyen onaylar" | onay bekliyor | düşük | evet |

Sorunsuz bulunanlar: field.rs, par.rs, wlog.rs, machine.rs (tüm dilimleme ASCII sınırlarında, panik yolu yok),
lib.rs'in geri kalanı (`write_root_file` atomik: O_EXCL tmp + fsync + rename + dizin fsync; `read_root_bytes` kontrolleri
açılmış fd üzerinde → TOCTOU yok).

Doğrulama (b358b67): cargo test 727 geçti / 0 kaldı / 9 ignored · clippy temiz · GUI build + ctest 77/77 · python 45/45.

## Ölü kod adayları

Aşama 1: yok (tüm `pub` öğelerin çağıranı var).

