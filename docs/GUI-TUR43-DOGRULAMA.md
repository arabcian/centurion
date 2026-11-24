# Centurion Experimental — Tur 43 doğrulama raporu

## Kapsam

Tur42 SHA-256 `360efc1449ddf8f52874fb921f177b4ab4a08f8e28712552fdc23bf82bbc9b13`. Kaynak/test delta: 25 files changed, 498 insertions(+), 121 deletions(-) (rapor ve TSV hariç). T43 9 iş paketine ilişkin değişiklik; planlanan 27 paket ve T42 kalanlarının tamamı bitmedi. 0/48 tam kabul, 47 kısmi, D05 engelli. Fiziksel donanım yazması/testi ve GitHub push yok.

## Gerçek kaynak değişiklikleri ve sınırları

- Health Overview: rapor preview_schema 2, her dosya için SHA-256, checkable seçim, 32 KiB UTF-8 önizleme, daha uzun dosyalar varsayılan seçili değil. Save öncesi yeniden toplanan **seçili maskeli içerik** hash ile birebir eşleşmelidir; değişmiş veri reddedilir, yeni önizleme gerektirir. Frozen capture değildir; sürekli değişen log/dGPU içeriği seçilmişse kayıt engellenebilir. Yalnız seçili dosyalar arşivlenir. Eski doğrudan out request artık kabul edilmez. JSON private alanları recursive redaction, string leaf sanitize, malformed veya fazla büyük JSON dışarıda; ham log redaction best effort, tam anonimlik garantisi yok. Dosya içi alan seçimi ve tüm diğer rapor araçları tamamlanmadı. Binary export değişmedi. Archive temp staging exclusive private, partial dosya unique create_new 0600, sync ve rename. Rust native test yazıldı ama çalıştırılmadı.
- C01/X07: kaydedilmiş loglar normal descriptor üzerinden açılır, FIFO/symlink reddi, iki dosya için ayrı ayrı 4 MiB bounded tail ve max 10000 satır; yarım/8192 byte üzeri satır, seq overflow reddedilir. JSON record descriptor ve bounded size; eş boyutlu concurrent değişiklikleri tümüyle yakalama garantisi yok. Diagnostic archive bu ortak reader'ı kullanır, legacy ekstra hücre davranışı korunur. Native Qt test ekli fakat çalışmadı.
- EDID: ortak progressive timing decoder kullanımı, CTA 0 offset malformed payload ve incomplete data block kontrolü; bozuk yapıdan explicit VRR/HDR/audio/colorimetry yayınlanmaz. Capture sonu connected status recheck, tam hotplug incarnation ispatı değil. Interlaced/DisplayID timing yeni destek değildir. Saf parser fixture genişletildi, Qt fixture güncellendi. Fiziksel EDID write yok.
- Wi-Fi source timestamp invalid olduğunda age fallback geçerli sayılmaz; CLOCK_BOOTTIME nanos range. Firewall invalid IP family unknown. Bunlar saf C++ fixture ile sınanmıştır; effective ruleset/HE/EHT tamamlanmadı.
- BarChart görünür 128 satıra clamp, global scale, Ctrl+wheel zoom, PageUp/Down/Home ve context pagination, visible table/CSV ve all CSV. 100k Qt fixture yazıldı, çalıştırılmadı; tüm chart türleri veya gerçek performans ölçümü değil.
- Workspace header cached snapshot: debounce/close/aboutToQuit; immediate close fixture, Qt native yok. Diğer workspace state ve write-failure receipt açık.
- Türkçe katalog yeni kaynak metinleri; placeholder kontrolü PASS. Tüm GUI/TR-EN/DPI audit yapılmadı.

## Test kanıtları

- Ön tarama 72/72 bağımsız Python runner exit0; son kaynakta hedefli 11/11 exit0. Bazıları static contract, bazıları Qt bağımsız derlenen C++ harness; native GUI/Rust kabul değildir. `runners.tsv`, `final-runners.tsv` ve her runner logu ekte.
- apt update exit0 olsa da DNS hataları; apt install exit100; Qt configure exit1 (Qt6Config.cmake yok), Cargo test exit127. Native Qt/Rust, CTest, Clippy, Gentoo PGO/LTO, GUI/visual/physical kabul yok. Native fixtures derlenmedi. Qt/Cargo toolchain yokluğu D05'i engelliyor.
- git diff --check exit0 ve source ZIP CRC kontrolü. Bu tur değiştirilmiş modüller için kaynak incelemesi, bütün proje denetimi değildir. T44 hedefi önceki tüm açık kriterler + derin audit; eksikler kapanmadan tam kabul ilan edilmemeli.
