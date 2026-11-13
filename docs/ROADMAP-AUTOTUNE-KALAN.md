# Centurion autotune: kalan maddeler (devir notu, 2026-10-31)

Yeni bir oturumda bu dosyayı ve `docs/ROADMAP-AUTOTUNE.md`'yi verin. Madde kartlarının tam metni (sorun, yapılacak,
kabul ölçütü) ROADMAP dosyasının "2. Madde kartları" ve "2b" bölümlerinde; burada her maddenin **bugünkü kod durumuna göre**
nereden başlanacağı, nelere dikkat edileceği ve hangi kararın beklendiği var.

## Durum özeti

- **Biten (24):** D1, D2, B1, B2, B3, B4, B5, A5, A6, A11, C1, C7, E2–E13 (E1 hariç; E3–E6, E9–E13 zaten koddaydı, eski
  "Kalan" listesi bunları yanlışlıkla sayıyordu).
- **Kısmi:** A3 (3σ tespit ölçütü karşılanmadı, aşağıda), A4 (`--strict` yok), A10 (SLC yok).
- **Kalan (14):** A1, A2, A4-strict, A7, A8, A9, A10-SLC, B6, C2, C3, C4, C5, C6, E1.

## 2026-11-01 oturumu

- **A4-strict ✅** `early_shift` (saf): ikinci referans koşudan hemen sonra `baseline_shift`; kayma varsa hemen commit + uyarı;
  `--strict` ile oturum temiz biter (satırlar kalır, restore normal yoldan). Test: `early_shift_runs_at_the_second_reference_only`.
- **A9 kapandı:** kart yanlıştı; `MemBwGbs` `__probe`'un memcpy ölçümünden geliyor (`bench.rs` "membw_gbs", `probe_child`).
- **A10-SLC ✅** `bench::io` yazmayı 32 MiB parçalarda `fdatasync` ile ölçer; `slc_drop` (son 3 parça medyanı < ilk 3'ün yarısı)
  → koşu ×0.3. Sürüm yalnız IO'ya: `calib::IO_BENCH_VERSION` = 6, `bench_version(phase)`; idle/load satırları tam ağırlıkta kalır.
  Test: `slc_drop_from_part_speeds`. Donanımda doğrulanmadı.
- **A7 ✅ (ölçüldü, kazanç yok)** ortak `ref_due` (üretim + calsim). Rastgele konum ve 5–7 blok 12 tohumla ölçüldü: realistic
  ≈ aynı, classic kapsama 0.75 → 0.42 / 0.67; sabit 6 tutuldu. Uyarlamalı 4–8 denenmedi.
- **B6 ✅** `--phase load`, `--goal all` (signature `gains`), `--sequential` (Bonferroni, tam t tersi `t_crit`; FP %3.2 simüle).
  Testler: `sequential_verify_keeps_false_positives_under_5_percent`. Donanımda koşulmadı.
- **A8** ertelendi (donanım ölçümü).
- **cpu.epp_boost:** Gaming/Throughput sabitlemesi (`pinned`) kalktı; kural ön bilgi, ölçüm güvenle zarar gösterirse ezer.
- **mm.lru_gen:** kalibrasyona girdi (0 vs 7, idle + load, CPU paketi + bellek probu); autotune'un 7 kuralı ön bilgi olarak kalır.
  Load fazında balast altında MGLRU kapatılıyor: donanımda takılma izlenmeli.
- **docs/KNOBS.md:** 196 knobun kalibrasyon / autotune envanteri (koddan üretildi).

## 2026-11-13 oturumu: kod okuması bulguları

- **Kesilen faz ölçtüğünü tutuyor ✅** `Design::exec` içindeki `rn.run(..)?` hata verince (STOP, şarj gitti, balast, IO bütçesi)
  son commit'ten sonraki koşular kayboluyordu; space-filling aşaması yalnız sonunda commit ettiği için önyükleme kalibrasyonunda
  bir tuş, o fazın tamamını siliyordu ("what it measured is kept" yazmasına rağmen). `Design::kept_on_error` önce commit eder
  (≥ 4 koşu). Test: `a_phase_that_ends_early_keeps_its_runs`. Testlerde `save` signature yazmaz (`cfg!(test)`).
- **Sayısal seçenekler ✅** `num_opt`: `--budget/--sessions/--rounds/--seed/--secs/--io-budget` bozuk / negatif / NaN değerde hata
  (eskiden sessizce varsayılan; `--budget nan` süresiz max oturumdu). Test: `numeric_options_are_parsed_or_refused`.
- **diff_vs_last ✅** geçmiş kaydına `newest` (en yeni satırın zamanı): satır tavanındaki signature'da satır sayısı değişmediği için
  yeni veri "rule" diye etiketleniyordu.
- **İyimser tahmin teşhisi ✅** `Calibration::predcheck_optimism` ("kazananın laneti" açık bulgusu için): oturum × faz başına
  ortalama z; ≥ 6 grup, ortalama < −0.5 ve 0'ın 2 se altında ise `--show` ve autotune notu. Yalnız teşhis: tahminler düzeltilmiyor
  (büzme, simülatörde `sim_regression` ile ölçülmeden eklenmemeli).
- **Yapılmadı, bilinmeli:** (1) `verify_main`'de `rn.run(..)?` hata verince tamamlanmış ABBA blokları raporlanmadan çıkılıyor
  (donanımsız test edilemediği için dokunulmadı; döngüden `break` yeterli). (2) `docs/AUTOTUNE.md` "forced idle koşusu %10 sayılır"
  ve "uyarı sıcaklığındaki IO koşusu %30 sayılır" diyor, kod ikisini de atıyor (`Runner::run`); açılıştaki stderr notu da %10 diyor.
  (3) `autotune-seen.json` atomik yazılmıyor ve oku-değiştir-yaz: paralel iki amaç birbirinin kaydını ezebilir.

## 2026-11-01 (2) oturumu: hedef ölçeği

- **Metrik aileleri ✅** `compute_rows`/`fold` hedefi metrik başına değil senaryo (aile, `Metric::family`) başına toplar
  (ortalama × aile/2). Eski hali yoğun knoblarda (EPP, boost, C-state) idle'da lat/thr ×5.5, pwr ×1.5 (load ×6 / ×1) veriyordu:
  güç, hedef ağırlığının ~¼'ü kadar tartılıyordu. Şimdi idle 2/2/1/1, load 2/2/1/1, IO 1/1/1/1.
  `BENCH_VERSION` 5 → 6, `IO_BENCH_VERSION` 6 → 7, signature format 10: eski satırlar `rescale_families` ile ölçeklenir
  (baseline kaydındaki metrik listesi, yoksa `SET5_METRICS`), ×0.6; DEV satırları değişmez ve güncel sayılır; `hyp` ve
  `fit-scales.json` (yeni `v` alanı) yeniden kestirilir. **Bedel:** ailesinde yalnız olmayan tek bir metriği oynatan knob
  (ör. yalnız `GameJitterUs`) eskisinden küçük görünür. Testler: `objectives_count_scenarios_not_probes`,
  `format_9_rows_are_rescaled_and_their_scales_dropped`, `a_change_of_every_metric_weighs_power_against_latency_by_scenarios`.
- **Gürültü ağırlıkları havuzlu ✅** `metric_weights` oturumun referans saçılımını makinenin önceki oturumlarıyla
  (`Calibration::ref_log_sd`, aynı faz + güç kaynağı, 6 koşu değerinde) birleştirir; < 4 referanslı oturum artık eşit
  ağırlığa düşmez. Testler: `a_short_session_weighs_its_metrics_by_the_machines_earlier_scatter`, `reference_scatter_pools_*`.
- `sim_regression` değişmedi (simülatör hedef seviyesinde çalışır; metrik katmanı birim testlerle sınanıyor).
  Donanımda doğrulanmadı: MARGIN 0.03 yoğun CPU knobları için artık fiilen daha sıkı.

## Kalan maddeler

| ID | Öncelik / efor | Bağımlılık | Durum | Ne yapılacak (bugünkü koda göre) | Kabul / doğrulama | Dikkat, açık karar |
|---|---|---|---|---|---|---|
| **A1** Koşu başına telemetri | P0 / L | — | Yok | Yeni `src/runtel.rs`: efektif saat (perf `cycles`/`ref-cycles`, yoksa `/dev/cpu/*/msr` APERF/MPERF, yoksa `scaling_cur_freq` + düşük güven), cpuidle `state*/{usage,time}` farkı, Intel `thermal_throttle/*_count`, Ryzen `pmtable::sample()` limit oranları (STAPM, PPT fast/slow, TDC, EDC, THM), prob başına RAPL. Okuma `Runner::run` içinde (`centurion-calibrate.rs`, A6'nın `SysCounters` okuduğu yerin yanı). Kayıt: `calib::Row`'a alan eklemek 26 struct literal'i değiştirir; A5'teki gibi yan tablo (`sess`/`t` ile eşleşen) daha cerrahi. Limit ≥ %98 → ağırlık düşür (A6 bloğundaki desen), quality'ye sayaç. `--show` ve rapor: "throughput +%4, saat +%4: kazanç saatten". | `Rapl::from_dir` deseniyle sahte sysfs ağaçlarına birim testi; signature round-trip; 1000 okuma < 5 ms/koşu; kök değilken / `perf_event_paranoid` yüksekken sessiz geri düşüş. | dGPU'yu uyandıran hiçbir şey okunmasın (A6'daki `runtime_status == active` kontrolü örnek). perf fd'leri oturum boyu açık kalsın. Telemetri objective değil, teşhis + nuisance. Donanımda doğrulanmalı (bu ortam Legion değil, autotune "unsupported machine" der). |
| **A2** Güç-bütçesi kovaryatı | P1 / M | A1 | Yok | Koşu başında paket gücü EWMA'sı / PL1 (Intel powercap `constraint_*_time_window_us`, AMD pmtable STAPM sabiti, yoksa 60 s) → `budget` 0–1+. `model.rs`'de sıcaklık teriminin (`TEMP_SD`, `Obs.tc`, `pt` çekirdek parçası) yanına ikinci nuisance terimi. Bütçe ≥ 0.9 iken cool koşu önce bekler (`conditions()` / thermal gate deseni, üst sınırlı). | B1: `calsim.rs`'de `Noise.budget` zaten var ama gözlenmiyor; satıra bütçe değeri yazılıp terim yokken/varken knob bias'ı ölçülür (hedef ≤ σ/2). `MODEL_VERSION` 3 → 4 (fit-store testleri bunu yakalar). | Bütçe değeri satıra girerse `model::Row`'a alan gerekir (sim + calib + compute_rows). |
| **A4-strict** | P1 / S | — | Kısmi | Bugün: `baselines` kaydı + kayma tespiti `Design::commit`'te (her commit, ≥ 2 referans koşu, ≥ 3 eski oturum, > 4·MAD). Eksik: (1) kontrolü ilk 2 referans koşudan hemen sonra koşu döngüsünde yapmak, (2) `--strict` bayrağı ile oturumu temiz bitirmek (`STOP` / `power_lost` gibi temiz çıkış yolu). | Birim testi: kaymış oturumda `--strict` → oturum biter, satırlar ve restore korunur. | `calib::baseline_shift` saf fonksiyon, yeniden kullan. |
| **A7** Referans koşu sıklığı | P2 / M | B1 | Yok | Referanslar sabit kodlu: `since >= 6`, `i % 6 == 5`, `i % 3 == 2` (`centurion-calibrate.rs` tasarım döngüleri) ve `calsim.rs` içinde kopyası. Önce `fn ref_due(...)` gibi tek bir zamanlama fonksiyonuna çıkar, hem üretim hem `calsim::calibrate` kullansın; sonra 5–7 blokta rastgele konum + drift/referans sd'sine göre 4–8 uyarlama. | B1: `realistic` senaryoda periyodik parazit (17 koşuda bir) var; regret ve kapsama iyileşmeli ya da aynı kalmalı (`sim_regression` ±0.03). | Zamanlama fonksiyonu ortak olmazsa simülatör üretimi ölçmez. |
| **A8** Koşu içi prob sırası | P2 / M | Donanım | Yok | Önce donanımda referans koşu sd'sinin prob sırasıyla değiştiğini ölç; etki yoksa madde kapanır. Varsa Latin-kare döndürme + sıra indeksi nuisance. | Donanım ölçümü. | **Karar/ölçüm sizde.** |
| **A9** Ölü `MemBwGbs` | P2 / M | — | Yok | `Metric::MemBwGbs` (`calib.rs`) hiçbir prob tarafından üretilmiyor; yalnızca testlerde geçiyor (`rows_from_runs_are_signed_clipped_and_robust`). Ya kaldır ya çok iş parçacıklı triad + boşta bellek gecikmesi probu ekle. | Prob testi + koşu süresi bütçesi. | **Karar bekliyor:** prob eklemek `BENCH_VERSION` artışı = eski satırlar ×0.6. Kaldırmak `Metric::ALL` dizisi ve isim serileştirmesini etkiler. |
| **A10-SLC** | P1 / M | — | Kısmi | Bütçe kısmı bitti (`io_days`, `--io-budget`). SLC: `bench::io` tamponlu yazıyor; ilk parçalar sayfa önbelleğine gider, dirty sınırında çekirdek disk hızına kısar → parça hızı her koşuda "düşüş" gösterir (yanlış alarm). Doğrusu parça başına `fdatasync` (32 MiB) ile cihaz hızı ölçmek. | Birim testi: parça-hız serisinden düşüş yakalama (saf fonksiyon). | **Karar bekliyor:** benchmark yazma deseni değişir → `BENCH_VERSION` artışı (eski IO satırları ×0.6), `WriteMbs`/`FsyncP99Ms` anlamı değişir. NVMe SMART `data_units_written` (ioctl) yerine blok sayacı kullanılıyor; istenirse eklenebilir. |
| **B6** `--verify` genişletme | P2 / M | — | Yok | `--phase load` (ballast ile), `--goal all` (aynı veriden tüm amaçların ağırlıklı kazancı), ardışık durdurma (her ABBA bloğundan sonra t-aralığı 0'ı dışlıyorsa veya en çok tura ulaşıldıysa dur, min 3 blok). | `verify_pairs_runs_by_round_and_position` benzeri sentetik testler; durdurma kuralının yanlış-pozitif oranı simülasyonla ≤ %5 (calsim'deki `Bench` gözlem süreci kullanılabilir). | Ardışık testte α harcaması (tekrarlı bakış) hesaba katılmazsa FP %5'i aşar. |
| **C2** Calibration görünümü | P1 / L | B5 ✅ | Yok | CLI: `--show --json`, `--list --json` (kök gerekmez; metin aynı veriden). GUI'de Optimizations altına "Calibration" alt sekmesi: faz başına aşama ilerlemesi, satır/oturum, R² + %90 kapsama **ve** B2'nin LOSO değerleri (`Model::loso_r2/loso_cover`), varyans genişletme, knob başına etki forest plot'u (seen işareti; A3'ün `seen_z`), etkileşimler, unsafe / soft-unsafe, quality geçmişi (A6 `parasites`, A4 `baseline_shift` dahil), verify geçmişi, ortam dağılımı (A5 `env_spread`). Grafikler `gui/src/charts.*`. | JSON şeması `docs/AUTOTUNE.md`'ye; GUI boş / eski / 3000+ satırlık signature ile < 1 s (ağır iş QProcess'te); metin `--show` çıktısı değişmez (snapshot testi). | GUI testi deseni: saf biçimlendirme kodunu ayrı birime al (C1'deki `autotunereport.*` gibi), Qt6 + `-DCENTURION_BUILD_TESTS=ON`. |
| **C3** "Calibrate now" + canlı ilerleme | P1 / L | — | Yok | Kalibrasyon her koşuda `/run/centurion/calibrate-progress.json` yazar (atomik). `tune-helper` op'u `calibrate_now`: beyaz liste (budget 5–60, sessions 1–4, scene adı doğrulanır), argv dizisi, ayrık süreç; Stop → SIGINT (temiz restore mevcut). GUI: diyalog, canlı panel, "makineyi kullanmayın" uyarısı. | Birim testi: kötü sahne adı / aşırı bütçe reddedilir; GUI kapanınca kalibrasyon sürer, açılınca ilerlemeyi bulur. | Root op: güvenlik incelemesi (Opus). Polkit seviyesi `calibrate_on_boot` ile aynı. Mevcut boot yolu `calibrate-status.json` yazıyor, aynı desen. |
| **C4** GUI'den preset doğrulama | P2 / M | C3 | Yok | Kayıtlı preset için "Verify…" (C3'ün polkit/ilerleme altyapısı), sonuç kartı: objective başına paired kazanç ± CI, "as predicted / SURPRISE", geçmiş (signature `verify`). | Fixture JSON'la Qt testi. | — |
| **C5** Field A/B + MangoHud | P2 / L | — | Yok | A/B açık oyunlarda gamemode `MANGOHUD_CONFIG`'e `autostart_log`, `output_folder=~/.local/state/centurion/frametimes` (kullanıcının ayarını ezmeden). İçe aktarma tune-helper op'u ile (bugün `sudo centurion-calibrate --field`). GUI: oyun başına A/B oturum sayısı, medyan / %1 low / pacing farkı ± CI. | `field.rs` testleri + Qt fixture testi. | — |
| **C6** Taban çizgisi ve kanıt grafikleri | P2 / S | C2, A4 ✅ | Yok | A4'ün `cal.baselines` geçmişi (oturum × faz, metrik medyan + robust sd) ve autotune `evidence_summary` için Calibration sekmesinde zaman grafikleri. | Qt fixture testi. | C2 olmadan yeri yok. |
| **E1** sched_ext'i tamamla | P0 / L | — | Yok | `scx_set` (`tune.rs`) argümansız başlatıyor; `SCX_NAMES` sabit. Yeni knob `sched.ext_mode` (auto/gaming/lowlatency/powersave/server), argümanlar yalnız `/etc/scx_loader/config.toml` (yoksa `/etc/scx_loader.toml`) veya koddaki küçük tablodan. scx_loader D-Bus (`org.scx.Loader`) varsa `StartScheduler/SwitchScheduler/StopScheduler`, baseline'a önceki scheduler + mod; yoksa doğrudan yol. Allowlist: güvenilir dizindeki `scx_*` ∩ bilinen-güvenli (chaos ve demo/test hariç), bilinmeyen "experimental". Sağlık: `/sys/kernel/sched_ext/{state,enable_seq}` + çekirdek günlüğü → `stability.rs`. Kalibrasyonda scx ile EEVDF/BORE knob'ları karşılıklı dışlayıcı (`bad` kümeleri). | config.toml ayrıştırma, mod → argüman, allowlist (chaos ve `../` reddi), sahte D-Bus ile var/yok yolları; restore sonrası önceki scheduler + mod aynen döner. | Güvenlik (Opus): kullanıcıdan serbest argüman alınmaz. Şu an `scx_loader`/`ext_mode` kodda hiç geçmiyor. |

## Açık bulgular (madde dışı, bilinmesi gereken)

- **A3 kalanı:** 3σ gerçek etkilerin yalnızca %79'u "seen" (hedef ≥ %90). Hiçbir eşikle düzelmiyor; kaçanlar modelin büzdüğü
  etkiler (ör. gerçek −0.086, posterior −0.035 ± 0.021). Model tarafı (heredity / screening / prior ölçeği) incelemesi gerekir.
  Ölçüm: `cargo test --release -p centurion-helpers --bin centurion-calibrate seen_test_rates -- --ignored --nocapture`.
- **Kazananın laneti:** simülatörde gaming optimumunun tahmini kazancı bazen %90 aralığının dışında, iyimser.
- **Kör maliyet:** desktop/gaming regret'inin büyük kısmı benchmark'ın göremediği güç maliyetinden (simülatörde `blind`).
- **Eski GUI** yeni JSON'la çalışır ama `notes` dizisini göstermez (yeni GUI aynı yamada).
- **Doğrulanmayanlar:** GUI'nin tamamı derlenmedi (yalnızca değişen dosyalar + testler); hiçbir şey gerçek Legion'da çalışmadı;
  A6 eşikleri (`bench::IRQ_RATIO` 2×, `DISK_MB_S` 20, `NET_MB_S` 5, `GPU_BUSY_PCT` 20) ayarlanmadı.

## Çalışma düzeni (yeni oturum için)

- Testler: `cargo test --release -p centurion-helpers` (tümü ~3 dk; lib 317 test). GUI: Qt 6.4+ (Widgets, Network, Svg, DBus),
  `cmake -S gui -B build -DCENTURION_BUILD_TESTS=ON`, test hedefi ör. `centurion-autotunereport-test`.
- Simülatör (B1): `crates/centurion-helpers/src/calsim.rs` (kalibrasyon binary'sinin test modülü). Regresyon:
  `cargo test --release -p centurion-helpers --bin centurion-calibrate sim_regression -- --ignored --nocapture` (~40 s),
  baseline `crates/centurion-helpers/tests/sim-baseline.json` (±0.03); istatistik bilerek değişince `CENTURION_SIM_BASELINE=write`.
  Diğer ölçümler: `seen_test_rates` (A3), `loso_against_loo` (B2), `incumbent_head_start_steadies_decisions` (B4).
- Sürüm sabitleri: `model::MODEL_VERSION` = 3 (B2 ile arttı), `calib::BENCH_VERSION` = 6, `IO_BENCH_VERSION` = 7 (metrik aileleri).
- Bu oturumda eklenen kalıcı dosyalar: kullanıcı state dizininde `autotune-history.jsonl` (B4), `autotune-seen.json` (A3);
  signature'da `baselines` (A4), `envs` + `sess_env` (A5), `io_days` (A10).
- İlkeler (CLAUDE.md): cerrahi değişiklik, önce test, kartın iddiası koda uymuyorsa dur ve söyle. Bu oturumda üç kez kart
  verisiyle çelişti (A3 ≈%50 iddiası, B4 δ önerisi, A10 SLC yöntemi); karar her seferinde simülatör ölçümüyle verildi.
