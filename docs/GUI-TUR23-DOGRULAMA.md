# Centurion Tur 23 — kısmi kaynak teslimatı / doğrulama

**Baseline:** Opus tarafından düzeltilmiş Centurion-experimental-tur22-duzeltilmis.zip, SHA-256 `0a349aa461a9bcb45bf33fa0ebfcf85e99faf3fe852da0ad92fe3c95a693ae25`. Eski devir hash'i farklı; yanlış arşiv kullanılmadı. ZIP CRC PASS. Kaynak üzerinde yerel Git baseline ve diff oluşturuldu; remote/push yapılmadı.

## Gerçek kapsam

0/48 iş paketi tam kabul. Devralınan 47 kısmi ve D05 engelli durumu korunuyor; 3 iş paketinde davranış ilerlemesi (A03/A04/B03), D04 çeviri ve D05 test kaydı. 118/118 GUI satırının özgün kabul kriteri ve 48/48 paket satırının özgün kapsamı taşındı. Ek GUI ID'leri ve FIX kapsamı orijinal paket başlıklarında aynen; FIX izleme dosyası ayrı. Bütün kaynak dosyalarının satır satır denetimi tamamlanmadı; öncelikli çağrı zincirleri incelendi. Yeni native GUI/Rust testleri çalıştırılamadı. Bu kaynak üretim güvenliği onayı değildir.

## Kaynakta yapılan değişiklikler

| Paket | Gerçek diff | Kalan |
|---|---|---|
| A03 | sysinfo typed text/numeric cache; Home chart numeric, teslim anında expiry; D-Bus iki API bounded worker/queue (32 istek, 5 s timeout; I/O bitmeden yeni worker yok) | Health/stress/SMART, stable device identity, suspend/AC scheduler, subscription, all-reader timeout, first-load GUI sync, device hotplug; GPU runtime gate best-effort, fiziksel no-wake garantisi yok; mevcut AMD iGPU sınıflandırması stable identity değil |
| A04 | userhelper process bounded stdout/stderr, timeout, owner destruction, invalid JSON/exit status guard | CO start/resume/discard/reset/stress teardown/destructor untouched; bütün GUI sync çağrıları ve A-B-A doğrulaması açık |
| B03 | read-only CLI validate-draft + GUI button; same draft payload and extracted Apply preflight, V/F shape issue indices, projected read-only snapshot; no privileged call | UUID/VBIOS/driver stable identity, overlays, full validator parity/point diagnostics, persistent journal, real apply/reset/trial verification, physical testing open. Explicit validation may wake GPU. Root-owned configuration/actual Apply re-check remains authoritative. |
| D04 | 15 yeni TR mesajı | FIX-01–21, tüm layout/DPI/native görsel testleri açık |
| D05 | baseline/final runner and independent cache tests | Qt6/Rust native, CI, Gentoo LTO/PGO, all hardware fixtures open |

A02 canlı tema, A05 merkezi registry, A07 gerçek MainWindow simulation, B07 optimization journal, B11 Autotune A/B ve diğer paketlerde yeni hedef davranış eklenmedi. Mevcut CO crash guard, firmware/SMU/fan, ABBA, boot/restore yollarına diff uygulanmadı. NVIDIA mevcut Apply preflight ortak fonksiyona taşındı; değişiklik fiziksel/GPU native doğrulanmadığından güvenlik garantisi verilmez. A03 fan duplicate seçiminde ilk pozitif değer, numeric ve text aynı kaynaktan gelir; önceki numeric son sürücü davranışıyla farklılık olabilir. D-Bus API anahtarları korunur, expired/unknown omitted. D-Bus eski doğrudan C++ çağrıları sync kalır. Yeni V/F preflight read-only, başarılı sonucu stabilite garantisi sayılmaz.

## Testler

| Test | Sonuç |
|---|---|
| Tur22 Opus baseline 49 runner | 49 PASS / 0 FAIL (statik ve bağımsız native karışık) |
| İlk değişiklik sonrası 49 runner | 49 PASS / 0 FAIL (son schema/çeviri/test eklemelerinden önce) |
| Bağımsız telemetrycache genişletilmiş native ASan+UBSan, -Werror | PASS, EXIT=0 |
| Bağımsız telemetrycache GNU LTO, -Werror | PASS, EXIT=0; Gentoo GUI LTO değildir |
| Qt6 Debug / Release configure | FAIL: Qt6Config.cmake yok |
| Qt6 CTest, yeni userhelper fixture, real GUI visual | ÇALIŞTIRILMADI |
| Rust locked test, clippy, cargo-deny | ENGELLİ: cargo bulunamadı |
| Gentoo PGO/LTO, gerçek cihaz, CI | ÇALIŞTIRILMADI |
| git diff --check | PASS |

Paket kurulumu Debian mirror DNS hatasıyla başarısız. Donanıma yazma veya otomatik stres yapılmadı. Son recheck sonuçları kanıt arşivinde varsa ayrıca yer alır; ilk 49 PASS son kaynak için tam yeniden doğrulama yerine geçmez.

## Devam

Özgün devir ve kabul matrisleri tam korunmuş; yeni sütunlar bu teslimatın fiili diff ve açık kriterlerini ayırır. Önce native Qt/Rust baseline ve bu tur diff derleme/test, ardından A04/A05, A07/A08, A02, B07/B11 ve bütün özgün kalan işlere devam. Tam kabul gerçekleşmeden experimental push yok. 

Son tam recheck: 49/49 PASS; diğer sonuçlar results.tsv içindedir.
