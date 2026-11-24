# Centurion Experimental — Tur42 doğrulama raporu

## Kapsam ve sonuç

Tur41 ZIP temel SHA-256 `bef0bcf4d0f9e98ee4fcf208d40d3113ca6e99989f421d65cdb46cd33316afff`. 15 kaynak/test dosyası, 496 ekleme ve 145 silme (rapor ve birikimli TSV hariç). 20 planlanan çekirdek paketin tümü tamamlanmadı. Bu tur 12 pakete ilişkilendirilen kaynak delta güvenlik ve kalibrasyon ağırlıklıdır. Paket kabul sayısı 0/48; 47 kısmi, D05 engelli. Önceki kabul kriterleri silinmedi. Fiziksel donanım yazması, gerçek benchmark, GitHub push yapılmadı.

## Kaynak incelemesinde düzeltilen sorunlar

- Kalibrasyonun her somut yazmadan önce bütün orijinallerini journal'da saklaması, duplicate/bozuk kayıtları reddetmesi, storage identity ve fd üzerinden write, gerçek readback. Scheduler restore sonradan diğer değerleri değiştirebildiğinden kısmi restore durumunda bütün transaction kaydı saklanır. Legacy storage kayıtları kimliksiz otomatik yazılmaz.
- Aynı tuning flock kalibrasyon/manual restore/field import'ta tutulur, signature lock sonrasında yüklenir; normal tuning root helper yarım kalibrasyon recovery durumunda durur. Bağımsız harici araçları kapsamaz.
- ABBA aynı metrik vocabulary, bütün ölçüm blokları, her weighted objective coverage, OOM/restore/boot/power context ve exact baseline plan kontrolü. Şema 3 eski kayıtları yetki olarak kabul etmez; root Apply, exact preset değerleriyle son receipt'i ve gerçek context/baseline'ı doğrular. 24 saat, aynı boot, pozitif corrected CI. Scoped storage/device workload eşlemesi olmadığı için otomatik doğrulama reddedilir; vm.dirty/storage IO koşullarında explicit --io gerekir. Kullanıcının açık manual override yolu korunur. Harici sysfs yazmaları karşısında atomik garantisi yoktur.
- Kalibrasyon Scene/fan original capture/journal/exit/readback. Başarısızlıkta journal korunur ve yeni kalibrasyon engellenir. Orijinal profile geçişi için Custom/firmware restore şartı; dış değişiklik yüzünden manuel restore gerektiğinde journal korunur. Helper wait_with_output halen sınırsız; bütün SceneEngine owner/lifecycle tamamlanmadı.
- CO Search duplicate physical/logical, boş veya negatif topology reddi; verify-only mevcut limit, uniform allcore, crash guard ve excluded değerleri. Native bağımsız walk algoritması korunur; yeni Qt fixture derlenmedi.
- NVcurve mixed profile tek GPU kimliği güçlendirildi; read-only karışık sensörlerde belirsiz veya değişen kimlikte unavailable. Çoklu GPU combined sensor desteği geçici olarak reddedilir; tek GPU da doğrulama API'si yoksa unavailable. Standalone emergency reset değiştirilmedi; bütün per-device write/rollback ve reset karışık index riski açık. Root helper calibration hold'u lock beklemesinden sonra yeniden kontrol eder; tam cross-process atomic hold yok.

## Testler ve sınırlamalar

- Ön tarama: 71/71 bağımsız Python runner exit 0. Bu testler static source contract veya Qt'den bağımsız derlenen native C++ harness'tır; bütün GUI/Rust native testleri değildir.
- Son kaynak: 11 farklı hedefli runner; ilk geçiş 10/11. CO walk runner eski constructor satırına bağlı source assertion'da durdu; assertion yeni clamp satırına uyumlu güncellendi, aynı exhaustive native algoritma yeniden çalıştırıldı ve PASS: 454710 walk / 2353711 probe / max 50 probe. Son durumda 11/11. İlk hata logu ve retry ayrı saklandı.
- Yeni Rust unit testleri: Journal decode duplicate/identity, ABBA coverage, receipt authorization pure gate; yeni Qt costrategy fixture; yazıldı fakat derlenip çalıştırılmadı.
- apt update exit 0 olsa da DNS başarısız kaynaklar, apt install exit 100; Qt CMake configure exit 1: Qt6Config.cmake eksik. Cargo build/test/clippy/deny exit 127 (Cargo yok). Qt/Rust native, CTest, Gentoo PGO/LTO, fiziksel ve görsel matrix açık. Bağımsız runner exit0, uygulamanın derlendiği anlamına gelmez.
- git diff --check exit0; source ZIP CRC doğrulandı; SHA256 teslimatta ayrı verilir. Derin denetim yalnız değiştirilen ve bağlantılı yollar için yapıldı, bütün proje denetimi değildir.
