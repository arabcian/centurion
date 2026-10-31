# Centurion: Yol haritası (özellik 1–9)

Kapsam: istenen 9 özelliğin durumu. Uygulananlar: 1, 2, 5, 6, 7, 8 ve 9'un AUR / COPR kısmı.
Ertelenen: Flatpak. 3 ve 4'ün donanım engeli kalktı (LampArray arayüzü, aşağıda); sıradaki adım akış yardımcısının donanım testi.

## Durum tablosu

| # | Özellik | Durum | Nerede / neden | Tahmini iş | Bağımlılık |
|---|---|---|---|---|---|
| 1 | Sahne başına global kısayol | ✅ Yapıldı | `gui/src/shortcuts.*` (GlobalShortcuts portalı), Scenes → *Shortcut* | — | QtDBus, xdg-desktop-portal ≥ 1.18 |
| 1b | Fn+Q'ya özel davranış | ❌ Kapsam dışı | Fn+Q'yu EC işliyor; uygulama yalnızca sonradan profil değişikliğini görüyor. Kullanıcı kararıyla düşürüldü | — | — |
| 2 | MangoHud katmanı | ✅ Yapıldı | `centurion-gamemode OVERLAY`, `gui/src/mangohud.*`, Scenes → MangoHud kutusu | — | MangoHud |
| 3 | Sese duyarlı aydınlatma | 🔓 Engel kalktı (LampArray, c193) | Geçici (RAM) yol: HID LampArray arayüzü; `lighting-helper stream` yazıldı, donanımda çalıştırılmadı | 2–3 gün | Akış yardımcısının donanım testi |
| 4 | Ekran rengi senkronu | 🔓 Engel kalktı (LampArray, c193) | 3 ile aynı yol; ayrıca ScreenCast/PipeWire akışı | 3–4 gün (3'ten sonra) | 3'ün akış yardımcısı |
| 5 | Uyku teşhisi önerileri | ✅ Yapıldı | `gui/src/sleepadvice.*`, Health → Sleep → *Suggestion* | — | centurion-sleep-log hook |
| 6 | D-Bus API | ✅ Yapıldı | `gui/src/dbusapi.*`, `com.centurion.Centurion` | — | QtDBus |
| 7 | Masaüstü bildirimleri (eylem düğmeli) | ✅ Yapıldı | `gui/src/notify.*`; *Undo* (eğri koruması), *Re-apply* (drift), *Details* (sahne hatası) | — | QtDBus |
| 8 | İlk kurulum sihirbazı | ✅ Yapıldı | `gui/src/firstrunwizard.*`; Home → *Setup wizard…* | — | — |
| 9 | AUR / COPR | ✅ Yapıldı | `packaging/arch/`, `packaging/fedora/`, ortak düzen `packaging/install-files.sh` | — | — |
| 9b | Flatpak | ⏸ Ertelendi | Sandbox'tan `pkexec` için `flatpak-spawn --host` gerekir, bu da sandbox'ı fiilen kaldırır | 3+ gün | Dar bir D-Bus system servisi (mimari değişiklik) |

Kalan sıra: **akış yardımcısının donanım testi → 3 → 4 → Flatpak (gerekirse)**.

---

## 3. Sese duyarlı aydınlatma

**Engel (kalktı).** Spectrum arayüzünden (`048d:c197`, rapor 7) geçici renk yolu yok: Gen10
firmware'i LLT'nin host-driven A1/Aurora karelerini yok sayıyor, CB (EFFECT_SET) ise kalıcı profil
belleğine yazıyor. Aynı denetleyici ayrı bir USB cihazında (`048d:c193`, usage page 0x59) standart
HID LampArray arayüzü de sunuyor: autonomous mode kapalıyken renkler RAM'de tutuluyor, Spectrum
profili değişmiyor (16AFR10H'de doğrulandı: `state` hash'i önce/sonra aynı, autonomous=1 ile kendi
efektine döndü). 319 lamba (0–101 tuşlar, 102–318 ön/arka şeritler), en kısa güncelleme 32,2 ms.
Sürücü: `crates/centurion-spectrum/src/lamparray.rs`. 4 bölgeli (33 bayt) klavyede sürekli yazma
hâlâ donanımda doğrulanmadı.

**Plan.**
1. ~~*Araştırma (Spectrum)*~~: gerek kalmadı (LampArray).
2. *Akış yardımcısı* (yazıldı, donanım testi bekliyor): `lighting-helper stream`; uaccess ile
   pkexec'siz, stdin'den satır başına bir kare, en fazla cihazın 32,2 ms aralığında gösterir
   (araya girenler atlanır), çıkışta / SIGTERM'de autonomous mode'u geri açar, profil belleğine
   yazmaz. SIGKILL sonrası için `{"op":"la_restore"}`.
3. *Ses:* PipeWire monitor kaynağından yakalama (`pw-record --target @DEFAULT_SINK@.monitor`
   ya da libpipewire), 1024 örneklik FFT, bant enerjisi → renk. 4 bölgede 4 bant,
   Spectrum'da sütun başına bant.
4. *Arayüz:* Lighting sekmesinde "Audio reactive" anahtarı. Sahnelere bağlanmaz (sürekli
   süreç), tepsiden aç/kapa.

**Kabul ölçütü:** 1 saatlik çalışmada CB yazma sayısı 0 (helper sayacı), CPU < %2,
gecikme < 50 ms.

## 4. Ekran rengi senkronu (ambient)

**Engel:** 3 ile aynı, kalktı (LampArray akış yardımcısı).

**Plan.**
1. *Wayland:* `org.freedesktop.portal.ScreenCast` (CreateSession → SelectSources monitor →
   Start). PipeWire düğümü `restore_token` ile saklanırsa izin bir kez sorulur. Kareler
   libpipewire ile ~10 Hz, küçük çözünürlükte (`SPA_PARAM_Format` 64×36) alınır.
2. *X11:* XShm `XShmGetImage`, 10 Hz, kök pencere, aynı küçültme.
3. Ekranı 4 dikey şeride (4 bölge) ya da klavye ızgarasına (Spectrum) böl, ortalama renk
   al, 3'teki akış yardımcısına gönder.
4. Ses ve ekran modları aynı anda açık olamaz; tek bir "host-driven lighting" yöneticisi
   olur.

**Kabul ölçütü:** 10 Hz'te CPU < %3. Portal izni oturum başına en fazla bir kez sorulur.

## 7. Masaüstü bildirimleri (eylem düğmeli) — yapıldı

- `Notifier` (`gui/src/notify.*`): `org.freedesktop.Notifications.Notify`, app_name "Centurion",
  app_icon "centurion", `desktop-entry` ipucu. Konu = başlık: aynı başlıklı yeni bildirim
  öncekini `replaces_id` ile günceller. `ActionInvoked` / `NotificationClosed` dinlenir; başka
  istemcilerin bildirimleri yok sayılır. Eylemli bildirimlerde zaman aşımı sunucuya bırakılır
  (-1), diğerleri 2,5 s. Servis yoksa ya da çağrı hata verirse `showMessage`'a düşer.
- `Tray::notify(title, msg, actions = {})`: mevcut çağrı yerleri değişmedi.
- Eylemler: eğri koruması stoğa döndürünce **Undo** (önceki kayıtlı profil tepsi menüsüyle aynı
  yoldan yeniden uygulanır; kayıtsız düzenlemeler için düğme yok), drift → **Re-apply**
  (`DriftWatch::reapply`), sahne hatası → **Details** (pencereyi Scenes sekmesinde açar). Diğer
  eylemler pencereyi açmaz.
- Ryzen: `ryzen-co-helper` artık isteğe bağlı `"profile"` adını eğri günlüğüne yazar (Intel ve
  NVIDIA helper'ları zaten yazıyordu); *Undo* Ryzen'de de bu sayede çalışır.
- Test: `gui/tests/notify_test.cpp`, `dbus-run-session` altında sahte bir Notifications
  servisine karşı (argümanlar, `replaces_id`, eylem eşleme, kapanma, servis yokken yedek yol).
- Açık kalan: KDE ve GNOME'da gerçek masaüstünde görsel kontrol.

## 8. İlk kurulum sihirbazı — yapıldı

`gui/src/firstrunwizard.*`. `gui.ini` → `first_run/done` yoksa pencerenin ilk açılışında
(tepside başlarken değil) bir kez açılır; *Skip setup* de "tamam" sayılır. Kayıtlı sahnesi olan
mevcut kurulumlara gösterilmez. Home → *Setup wizard…* yeniden açar (pencerede menü çubuğu
olmadığı için düğme *Device info…*'nun yanında).

| Sayfa | İçerik |
|---|---|
| 1. Donanım | Model, desteklenme durumu, bulunan arayüzler |
| 2. Boot servisleri | systemd / OpenRC algılanır, etkin olanlar işaretlenir; seçilenler için komut gösterilir ve kopyalanır (root işlemi yok, polkit değişmedi) |
| 3. İlk yedek | Health → Backup'ın *Back up now* yolu |
| 4. İlk sahne | "Default" = güç profili + firmware sınırları (*Capture current* ile aynı anlık görüntü; fan eğrisi helper ister, Scenes'te); isteğe bağlı AC / pil otomasyonu |
| 5. Ekstralar | "Default" için kısayol, MangoHud satırı, D-Bus bilgisi, Handbook bağlantısı |

## 9. Dağıtım paketleri

- **Ortak düzen:** `packaging/install-files.sh`, install.sh'nin dosya kurulum bölümü (install.sh
  onu `source` eder; `DESTDIR` çıktısı öncekiyle birebir aynı). Paketler `POLKIT_RULESDIR` ile
  kuralı `/usr/share/polkit-1/rules.d`'ye koyar; `/etc/polkit-1/rules.d`'deki aynı adlı kopya onu
  geçersiz kılar (yerel güvenlik seviyesi). Paket varsayılanı seviye 3.
- **AUR:** `packaging/arch/centurion` (sürüm etiketi `v$pkgver`) ve `centurion-git`. Servis
  etkinleştirilmez, `.install` yalnızca bilgi verir; kaldırırken `tune-helper restore` çalışır.
  Yayından önce: `updpkgsums` ve `.SRCINFO`.
- **COPR:** `packaging/fedora/centurion.spec`, `make-dist.sh` tarball'u (crate'ler vendored,
  çevrimdışı derleme), `%systemd_post` / `%udev_rules_update`. `-march=native` / PGO kapalı,
  LTO ve sertleştirme Fedora bayraklarından. SELinux: helper'lar varsayılan `bin_t`.
- Gentoo ebuild'i hâlâ kendi listesini kuruyor; `install-files.sh`'ye taşınabilir.
- **Flatpak:** ertelendi (yukarıdaki tablo).

---

## Uygulanan özelliklerden kalan küçük işler — yapıldı

- **D-Bus:** `SensorValues() → a{sd}` (`cpu_temp_c`, `fanN_rpm`, `nvmeN_temp_c`,
  `power_<hwmon>_w`, `igpu_temp_c`, `battery_percent`, `battery_power_w`,
  `battery_health_percent`). `Sensors()` aynen duruyor.
- **Kısayol:** portal `version` ≥ 2 ise Scenes → Shortcut'ta *Change on desktop…*
  (`ConfigureShortcuts`). KGlobalAccel yedeği eklenmedi (Plasma 5.27 öncesi için gerekirse).
- **MangoHud:** blok artık `exec=cat $XDG_RUNTIME_DIR/centurion-overlay`; dosyayı GUI sahne /
  profil değişince yazar, kapanışta siler. Çalışma dizini yoksa ya da yol tırnak gerektiriyorsa
  `centurion-gamemode OVERLAY` kalır. Eski bloklar dokunulmadan çalışmaya devam eder (yeni biçim
  için anahtarı kapatıp açmak yeterli). Kazanç ölçülmedi.
