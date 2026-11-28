# Kod incelemesi — devir tablosu (2026-11-28)

Yeni oturum buradan devam eder. Ayrıntılı bulgular, okunan dosyalar ve ölü kod listesi `docs/review/REVIEW_LOG.md`'de.
Dal: `experimental`. Bu belgenin yazıldığı andaki son kod commit'i: `tune: keep clean_values' type-complexity allowance`.

## 1. Kurallar (kullanıcının talimatı, özet)

| Kural | Uygulama |
|---|---|
| Önce düşün | Varsayımları yaz; kodun nedeni belirsizse (donanım/firmware geçici çözümü) değiştirme, sor. |
| Sadelik | En az kod; tek kullanımlık soyutlama, spekülatif esneklik, imkânsız durum için hata yönetimi yok. |
| Cerrahi | Her satır bir bulguya bağlı; yetim bıraktığını temizle, önceden var olan ölü kodu silme, raporla. |
| Hedefe dayalı | Önce ve sonra tüm doğrulamalar yeşil; hata düzeltmede önce onu yakalayan test. |
| Kırmızı çizgi 1 | `crates/centurion-helpers` girdi doğrulama / clamp / izin listesi / yetki: yalnızca raporla, onay bekle. |
| Kırmızı çizgi 2 | Firmware/EC/MSR/sysfs/WMI yazan yollarda yazma sırası, zamanlama, trial/revert: değiştirme, raporla. |
| Kırmızı çizgi 3 | polkit, udev, systemd/openrc dosyaları, install.sh/uninstall.sh: dokunma, raporla. |
| Testler | Testleri geçsin diye değiştirme/atlama/silme. `tests/` ve `gui/tests/` Python statik testleri kaynakta kalıp arıyor; kırılırsa dur, sor. |
| Akış | Aşama başına: oku → bulgu tablosu → düşük/orta kırmızı-çizgi-dışı düzeltmeler ayrı commit → doğrula → kırmızı çizgi bulgularını onaya sun → REVIEW_LOG güncelle → push. Aşamalar arasında onay beklemeden devam. |
| Commit | Mesaj sonunda `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` ve oturum satırı; model kimliği yazma. |

## 2. Doğrulama komutları ve ortam

| Adım | Komut | Son sonuç |
|---|---|---|
| Rust test | `cargo test --workspace --locked` | 737 geçti / 0 kaldı / 9 atlandı |
| Clippy | `cargo clippy --workspace --all-targets --locked -- -D warnings` | temiz |
| GUI | `cmake -S gui -B build -DCMAKE_BUILD_TYPE=Debug -DCENTURION_BUILD_TESTS=ON -DCENTURION_SANITIZE=ON -DCENTURION_LTO=OFF && cmake --build build --parallel && QT_QPA_PLATFORM=offscreen ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build --output-on-failure` | 77/77 |
| Python | `grep -oE "run: python3 .*" .github/workflows/ci.yml \| sed 's/^run: //'` ile çıkan 45 komutun hepsi | 45/45 |
| cargo-deny | kurulu değil, çalıştırılmadı | — |

Ortam notları: Qt6 paketleri CI'daki apt listesiyle kurulur (yeni konteynerde yeniden gerekir). Helper binary'leri
`init()` içinde Lenovo dışı makineyi reddettiği için uçtan uca çalıştırılamaz; testler birim düzeyinde. ctest ~220 sn.

## 3. Tamamlanan aşamalar

| Aşama | Durum | Uygulanan commit'ler (bulgu) |
|---|---|---|
| 0 | plan onaylandı | — |
| 1 helpers ortak | tamam | F1.1–F1.7 |
| 2 tune | tamam | F2.1–F2.4 |
| 3 nvcurve | tamam | F3.1–F3.4 |
| 4 Intel UV | tamam | F4.1–F4.3 |
| 5a Ryzen/WMI/fwattr | tamam | F5.1 (onaylı, bu oturum) |
| 5b amdgpu/dgpu/nvreg/cooling/fan/lighting/spectrum | tamam | F5.4 (onaylı, bu oturum) |
| 6a autotune | tamam | F6.1 (doc yorumları) |
| 6b calib | tamam | — |
| 6c bench/stability/cooltest/gamemode/boot | tamam | F6.9 (onaylı, bu oturum) |
| 7 backup/sysadmin/fstab/tarscan | tamam | F7.1, F7.2, F7.3 (onaylı, bu oturum; F7.3 için önce davranışı değiştirmeyen taşıma commit'i) |

Bu oturumda onayla uygulananlar:

| # | Değişiklik | Test |
|---|---|---|
| F5.1 | legion-firmware-helper: `set_gpu_mode`, `set_igpu_mode`, `set_fw_oc`, `set_fw_oc_many` için `refuse_during_calibration` | yok (helper `init()` Lenovo dışında reddediyor); derleme + tüm doğrulamalar |
| F5.4 | `Rgb::parse_hex`: tek isteğe bağlı `#` + tam 6 hex hane | `hex_colour_is_six_hex_digits` (önce kırmızıydı) |
| F6.9 | `stability.rs`: `record` / `on_shutdown` / `on_boot` `helper_lock("exposure")` altında | yarış testi yok; mevcut stability testleri yeşil |
| F7.1 | `SYSCTL_DENY` += `kernel.poweroff_cmd`, `kernel.hotplug` | mevcut red testine iki vaka eklendi (önce kırmızıydı) |
| F7.2 | `fstabcheck::udev_unescape` bayt düzeyinde çözüp UTF-8'e çeviriyor | `udev_names_keep_their_utf8` (önce kırmızıydı) |
| F7.3 | `config_restore_root`: `tune-boot.json` → `tune::clean_values`, `intel-uv-boot.json` → `boot_within_gui_floor`; geçemeyen atlanır, `refused` alanında listelenir, Backup sayfası loglar | `restored_boot_files_pass_the_save_checks` (önce derlenmiyordu) |

## 4. Açık kalanlar (karar / test gerektiren)

| # | Yer | Konu | Ne gerekiyor |
|---|---|---|---|
| F5.7 | `nvreg.rs:83` | `RegistryDwords` anahtarında `,` izni gerekli mi? | Kullanıcıdan cevap (kırmızı çizgi) |
| F6.10 | `bootguard.rs:29` | Bozuk durum dosyasında ön ayarlar uygulanıyor (açık kalarak başarısız). Bilinçli mi? | Kullanıcıdan cevap |
| F7.5 | `sysadmin.rs:174` | udev'de `MODE` / `OWNER` / `GROUP` engellenmiyor. Bilinçli mi? | Kullanıcıdan cevap (kırmızı çizgi) |
| F6.2 | `autotune.rs:1455` | `joint_pass` sonrası `seen` için model yok; ulaşılabilir girdi bulunamadı | Kanıt yoksa dokunma |
| F6.3 | `bin/centurion-autotune.rs:161` | `--save` geçici dosyası süreç başına değil | Test yazılamadı; istenirse `write_replace` gibi pid ekle |
| F6.5 | `bin/centurion-calibrate.rs:2476,2484` | `--oat` imzayı `Calibration::save()` atlayarak yazıyor | Test yazılamadı; istenirse `cal.save()` |
| F6.6 | `overview.rs:114`, `cooling.rs:332` | `HOME` yoksa `/tmp/centurion` düşüşü; kök bağlamda tehlikeli olabilir | **Aşama 9'da** `state_dir` çağıranlarını kök açısından denetle |
| F7.4 | `sysadmin.rs:417` | `config_restore` yasak listesini yeniden çalıştırmıyor | bilgi |
| diğer bilgi notları | REVIEW_LOG "Bulgular" | F2.5, F3.5, F4.4, F5.2, F5.3, F5.5, F5.6, F5.8, F6.4, F6.7, F6.8, F6.11 | değişiklik önerilmedi |
| ölü kod | REVIEW_LOG "Ölü kod adayları" | `tune.rs` `sched_feature_state_for_test`, legion-gpu-helper kalibrasyon listesindeki iki girdi, spectrum'un tüketicisiz `pub` API'si | Silinmedi; son raporda listelenecek |

## 5. Kalan aşamalar

| Aşama | Kapsam | ≈Satır | Not |
|---|---|---:|---|
| 8 | netguard, firewall, traffic, wifi + bin'leri | 4.8k | Kök ağ kuralları: büyük kısmı kırmızı çizgi (doğrulama) |
| 9 | model, diagreport, health*, overview ve kalan teşhis modülleri | 8k | F6.6'yı burada kapat |
| 10 | cpustress, gpustress | 2.6k | |
| 11 | GUI çekirdek: privileged, mainwindow, main, dbusapi, telemetri, paylaşılan başlıklar | ~5k | |
| 12a | optimizetab | 4.6k | |
| 12b | cosearchdialog, ryzentab, inteltab | 5.7k | |
| 12c | hometab, nvidiatab, curvepage, amdgputab, fwattrtab | 6k | |
| 12d | scenes, scenestab, lightingtab, coolingpage, fancurvedialog | 5k | |
| 13 | netpage, netfirewall, backuppage, boot/sleep/kernelparams | 6.5k | backuppage F7.3 için 2 satır değişti |
| 14 | healthtab, diagreportview, systemdiagnostics, advanceddiagnostics, systools … | 8k | |
| 15 | kalan G2 UI dosyaları + **son rapor** | ~10k | Son rapor: değişiklik özeti, bekleyen onaylar, ölü kod listesi, mimari gözlemler, test durumu |

## 6. Yeni oturum için başlangıç metni

> Centurion kod incelemesine `experimental` dalında devam et. Önce `docs/review/HANDOVER.md` ve
> `docs/review/REVIEW_LOG.md`'yi oku; kurallar ve kırmızı çizgiler orada (bölüm 1). Doğrulama ortamını kur
> (bölüm 2) ve başlangıç durumunu yeniden ölç. Aşama 8'den başla, aşamalar arasında onay beklemeden ilerle;
> kırmızı çizgi bulgularını ve bölüm 4'teki soruları bana sor.
