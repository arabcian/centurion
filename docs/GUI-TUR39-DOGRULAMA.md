# Centurion Experimental — Tur 39 doğrulama

**KISMİ teslimat; 0/48 tam kabul, 47 kısmi, D05 engelli.** 118 GUI ID, ek GUI-234 ve diğer özgün ek ID'ler, FIX-01–21, WANTED 1–11 ve BUILD açık. Temel T38 SHA-256 `262432b3a20dc37c9cf7009a9e600d68a5f1961462ce6418f47ffde431d11fc0`; ZIP CRC PASS. 5 mevcut dosya değişti, 7 yeni kaynak/test/teknik rehber dosyası, 0 silinen (birikimli raporlar hariç). T38 kaynak değişiklikleri korunuyor; gerçek source patch temiz T38 üzerinde uygulandı, byte eşitliği PASS.

## Gerçek kaynak değişiklikleri

- A07: önceki `--simulate` sunum modu korunarak yeni developer-only `--simulate-shell` giriş yolu eklendi. İzolasyon kontrolü QApplication'dan önce; hatada normal GUI'ye düşmez. x86-64 seccomp gerçek execve/execveat/socket/connect ve seçili kernel mutation çağrılarını engeller; no-new-privileges, TSYNC; root/unsupported ABI ret. Bu bir genel amaçlı hostile-code sandbox değildir. `tools/real_shell_fixture.py`: altı sentetik fixture, read-only sys/proc, private settings, offscreen, capability drop, readonly runtime/font bind, private temporary candidate output. Normal host home/system device/sockets mount edilmez. Gerçek mount kombinasyonu çalıştırılmadı. Host runtime libs/font ve kernel metadata deterministik fixture değildir.
- A07: root/userhelper girişlerinde simülasyonda async failure + blocked request sayaçları, sahte başarılı readback yok. Direkt QProcess yolları OS filter ile hedeflenir; sayaçlar bütün syscall girişimlerini kapsamaz. Fixture dosya digest before/after. Başarılı fake helper / mutation replay ve bütün tüketiciler için typed backend injection henüz yok.
- A08: gerçek MainWindow constructor ve production page IDs; iki shell, TR/EN (TR resource zorunlu), 5 DPI, 3 pencere boyutu, 4 seçilebilir tema. Enabled ana sekmeler, görünür nested QTabWidget grupları, Health navigationEntries ve openNavigationTarget ile kayıtlı lazy araçlar; beklenen sayfa seti/geometry/DPR ve bounded PNG/manifest. Modal/hang için 180s launcher timeout. Normal kullanıcı etkileşim ve mutating controls test edilmedi. Strict baseline set+SHA eşitliği; otomatik baseline update yok. Pixel toleransı, intentional clipping/focus/contrast, sabit clock, tüm 57 tema ve onaylı PNG açık. Gerçek ekran görüntüsü üretilmedi.
- D05: CI bağımsız iki yeni runner adımı; native CI çalıştırılmadı. T38 CO/Scene/SMART/NVIDIA güvenlik yolları değiştirilmedi, donanım write/stress/reboot/restore denenmedi. Önceki FIX özgün bireysel kriterleri hâlâ yok; kabul uydurulmadı.

## Test / çıkış kodu

| Kontrol | Sonuç |
|---|---|
| Bağımsız runner seti (son geometry/UID/CI değişikliklerinden önce) | 69/69 exit0 |
| Final hedefli iki runner | 2/2 exit0 |
| Final guard GNU LTO `-Werror` | exit0 |
| Final guard ASan + UBSan | exit0 |
| T38 source patch uygulama ve byte eşitliği | PASS |
| apt-get update | exit0 ancak deb.debian.org DNS failed |
| apt-get install Qt6/Cargo/bubblewrap | exit100; paketler bulunamadı |
| Qt Debug/Release configure | exit1; Qt6Config.cmake yok |
| cargo build --locked | exit127; cargo yok |
| real shell launcher | exit2; unprivileged x86-64/bubblewrap önkoşulu sağlanmadı; live fallback yok |
| Qt build/CTest, Rust tests/Clippy/deny, native Bubblewrap smoke, GUI render, Gentoo PGO/LTO, CI/physical | ÇALIŞTIRILMADI |
| GitHub push | YAPILMADI |

Guard native testi gerçek filter'ı subprocess'te çalıştırır, main thread ve sonradan oluşturulan thread için socket/exec reddi ve internal socketpair korunumu. Launcher testinde altı fixture, komut mount sözleşmesi, sentinel/digest değişimi, path/duplicate/extra-artifact ve baseline mismatch reddi. Bu testler native MainWindow veya gerçek namespace yürütmesi değildir. Ayrıntılı komutlar runner kaynaklarında ve `docs/REAL-SHELL-FIXTURE.md` içinde, exit/çıktı logları kanıt paketinde.

## Sonraki öncelik

D05 native ortamı, A07 gerçek namespace smoke ve tam fake backend/transaction behavior, A08 gerçek reviewed baseline + pixel diff/layout; A04/A05 ve A03 kalan ortak lifecycle/telemetry; B03/B07/B09/B11 ve bütün açık paketler. T39 OS izolasyon yaklaşımı ayrı bir kısmi uygulamadır, A07 özgün typed injection kabul kriterinin yerine geçmez.
