# Centurion: Optimizations / Autotune / Kalibrasyon geliştirme tablosu

Kapsam: Optimizations sekmesi (`gui/src/optimizetab.*`), autotune (`crates/centurion-helpers/src/autotune.rs`),
kalibrasyon (`src/bin/centurion-calibrate.rs`, `src/calib.rs`, `src/model.rs`) ve benchmark'lar (`src/bench.rs`).
Hedef: daha çok ayrıntı (ne ölçüldü, neden bu karar, ne kadar emin) ve daha güvenilir kararlar.

Bu dosya Sonnet / Opus'a madde madde verilmek için yazıldı: her madde tek PR, kendi kabul ölçütü ve testiyle.
Sondaki **"Modele verirken"** bölümünde hazır prompt şablonu var.

---

## İlerleme (2026-10-31)

**Tamamlanan:** D1, E8, B3, A11, D2, E2, E7, B5, C1, C7, A5, E9; kısmi: B4, A4 (ayrıntı ve sınırlar aşağıda). `cargo test -p centurion-helpers --release` yeşil.

| ID | Durum | Not |
|---|---|---|
| D1 | ✅ | `tests/doc_consistency.rs`: ağırlık tablosu, GUI `fallbackWeights`, `MARGIN`, `SEEN_Z`, `KEEP_Z`, `PHASE_ROWS` cap'leri. `MODESTY` dokümanda sayı olarak geçmediği için kontrol dışı. |
| E8 | ✅ | `ppt_cpu_cl` bağlama kaydedilir; `ctx.limits` kimliğine **girmez** (eski oturumların kimliği aynı kalır), ayrı `ctx.cl` boyutu olur: alanı olmayan eski oturum referans sayılır. |
| B3 | ✅ | Signature `predcheck` (son 200; confirm + verify), `--show` kapsama + 5 kutulu PIT, autotune'da < %70 (≥ 10 kontrol) uyarısı. |
| A11 | ✅ | Tüm deşarj olan bataryalar toplanır, > 300 W reddedilir; DEV koşusunda `energy_now` (yoksa `charge_now × voltage_now`) farkıyla ikinci tahmin, > %20 ayrışmada uyarı (yalnız uyarı; metrik değişmez). |
| D2 | ✅ | `tests/properties.rs`: rastgele JSON / eski sürüm / bozuk bayt / round-trip. 8 MiB sınırı testi yok (`save` kök + dosya ister; budama mantığı private). |
| E2 | ✅ (kısmi) | irqbalance tespiti, satırda uyarı (`conflict` → GUI ⚠ + tooltip), CCD uygulanınca servis durdurulur (işaret dosyası), restore / `all` ile yalnızca bizim durdurduğumuz servis geri başlatılır. **Yok:** 10 s sonra drift yeniden okuma, kullanıcı seçeneği (otomatik yapılıyor). |
| B5 | ✅ | `Decision`: `source` (structural/estimate/measured/joint/pinned/evidence/audit/retired), `seen`, `alone`/`in_context` ([mean, sd], joint pass), `n` (ölçüm kanıtı), `risk`. JSON: `"decisions": [...]`, `"notes"` ayrı, `"constraints"` yalnızca `enforce` kısıtları; `rationale` aynen. Testler: `every_decision_has_a_source`, `rollbacks_retire_a_key`. Eski GUI çalışır ama notları göstermez. |
| C1 | ✅ | `gui/src/autotunereport.*` (+ `tests/autotunereport_test.cpp`): Source + Confidence sütunları (yalnız `decisions` varsa; yoksa eski düzen), skor izi hücre tooltip'inde (satır genişletme yerine), "Only changes" filtresi, açılır "Notes / Evidence", B4'ün `diff_vs_last`'ı ile "↻ changed since the last autotune" işareti. |
| B4 | ✅ (kısmi) | `$XDG_STATE_HOME/centurion/autotune-history.jsonl` (autotune kullanıcı olarak çalışır; amaç başına son 20: ağırlık, bağlam, satır sayısı, değerler). Çıktıda `"diff_vs_last": [{key, from, value, why}]`, neden: new data / weights / context / rule. CLI ve GUI gösterir. Sahne preset'i (`profile`) geçmişe yazılmaz. **Yok:** joint pass incumbent bonusu (δ = MARGIN/2): kabul ölçütü B1 simülatörüne bağlı. |
| C7 | ✅ | `autotune::effective_weights` (autotune ve `autotune_defaults` aynı fonksiyonu kullanır); op `"adapted": {goal: {weights, why}}` döndürür; ağırlık penceresi farklı olan satırda "default 0.40 → in effect 0.55" + nedeni tooltip'te. |
| A4 | ✅ (kısmi) | Signature `baselines` (oturum × faz başına metrik medyan + robust sd, bağlam, kaynak; ~64 oturum). ≥ 2 referans koşudan sonra her commit'te geçmişle karşılaştırma (aynı faz/bağlam/kaynak, ≥ 3 eski oturum, > 4·MAD, MAD ≥ %1): stderr uyarısı (bir kez), quality `baseline_shift`, quality satırı, autotune notu (`evidence_lines`). **Yok:** `--strict` ile oturumu durdurma; kontrol koşu başına değil commit başına. |
| A5 | ✅ | Ortam: BIOS, mikrokod, kernel release, cmdline (`mitigations=`, `amd_pstate=`, `intel_pstate=`, `preempt=`), RAM hızı (kaynak yok → boş). Satır formatı değişmedi: `sess_env` (oturum → hash) + `envs` (hash → kayıt). Ağırlık: BIOS/mikrokod farkı ×0.7, mitigations/preempt farkı ×0.5, çekirdek kuralıyla çarpımsal; ortamı bilinmeyen eski oturumlar ×1. `--show` ve autotune notu dağılımı yazar. |
| E9 | ✅ | Tespit (`kernel.yield_type` varlığı), profil özeti "scheduler: Project C (BMQ/PDS)", scheduler kurallarının atlanması ve rapor notu zaten vardı; tespit `project_c_under(root)`'a ayrıldı, sahte /proc ağacı testi eklendi. |
| E7 | ✅ | `PLACE_LAG`, `PLACE_DEADLINE_INITIAL`, `PLACE_REL_DEADLINE` tam satır (Scheduler grubu → kalibrasyona otomatik girer). Tabloda olmayan `sched_burst_*` ve sched feature'lar `describe.discovered` ile GUI'de salt-okunur listelenir (elle yazma desteği yok). |

**Kalan:** B1, A3, A1, A2, C2, C3, B4 (bonus), A4 (--strict), B2, A6, A10, B6, C4, A7, C5, A9, A8, C6, E1, E3, E4, E5, E6, E10, E11, E12, E13.

## 0. Mevcut durum (yeniden yapılmasın)

Sistem zaten çok olgun; aşağıdakiler **var ve çalışıyor**, maddeler bunların üzerine kurulur:

- Sıralı deney tasarımı (space-filling → doubt-driven → dose refinement → confirmation), progressive lean oturumlar (base/pairs/crowd/refine/polish).
- Amaç başına GP modeli (tüm ikililer + üçlüler, heredity, oturum ofseti + drift, Student-t ağırlıklandırma, LOO, `var_scale` ile dürüst aralıklar).
- Ölçüm koşulları: güç kaynağı, bağlam pseudo-knob'ları (`ctx.profile/power/limits/heat`), die sıcaklığı kovaryatı, termal kapı, fan kilidi, pil güncelleme periyodu, ekran durumu, forced-idle, sürücü sıcaklığı, calibration hold.
- Güvenlik: journal + restore, inflight → unsafe set, OOM bisect, soft-unsafe, pressure guard, rollback öğrenimi, field stability (Gamma-Poisson).
- `--verify` (ABBA, paired t-aralığı), `--field` (FLM/MangoHud A/B), `--gpu-coupling`, boot'ta kalibrasyon, fit-store önbelleği.

**Asıl boşluklar:** (1) koşu başına *neden* bilgisi yok (saat, C-state, limit), (2) bazı istatistiksel anahtarlar çoklu karşılaştırmaya açık, (3) simülatör gerçek makineden çok daha temiz, (4) GUI, backend'in ürettiği bilginin küçük bir kısmını gösteriyor, kalibrasyonu GUI'den başlatmak / izlemek / doğrulamak mümkün değil.

---

## 1. Ana tablo

Öncelik: **P0** = güvenilirliği doğrudan etkiler / diğerlerinin temeli · **P1** = belirgin kazanç · **P2** = iyileştirme.
Efor: **S** ≤ 1 gün · **M** 2–4 gün · **L** 5+ gün. Model: önerilen yazıcı (istatistik/model işi → Opus, tesisat/GUI → Sonnet).

| ID | Başlık | Alan | Öncelik | Efor | Bağımlılık | Model |
|---|---|---|---|---|---|---|
| **B1** | Gerçekçi simülatör + karar-kalitesi regresyon testi | Model doğrulama | P0 | L | — | Opus |
| **D1** | AUTOTUNE.md ↔ kod sabitleri tutarlılık testi | Altyapı | P0 | S | — | Sonnet |
| **B5** | Kararlara yapısal kaynak/güven alanı (`decisions[]`) | Autotune çıktısı | P0 | M | — | Sonnet |
| **A3** | "Seen" testini çoklu karşılaştırmaya dayanıklı yap + histerezis | İstatistik | P0 | M | B1 | Opus |
| **A1** | Koşu başına telemetri: efektif saat, C-state, throttle/limit | Ölçüm | P0 | L | — | Opus |
| **A2** | Güç-bütçesi (PL1 tau / STAPM) durumu kovaryatı | Ölçüm + model | P1 | M | A1, B1 | Opus |
| **A4** | Oturum taban çizgisi geçmişi + "makine her zamanki halinde mi" kontrolü | Ölçüm | P1 | M | — | Sonnet |
| **A5** | Ortam dönemleri: BIOS / mikrokod / cmdline / RAM ayarı satıra işlensin | Signature | P1 | M | — | Sonnet |
| **C2** | `--show --json` / `--list --json` + GUI "Calibration" görünümü | GUI + CLI | P1 | L | B5 | Sonnet |
| **C1** | Autotune raporu v2 (kaynak rozeti, güven, skor izi, notlar ayrı) | GUI | P1 | M | B5 | Sonnet |
| **C3** | GUI'den "Calibrate now" + canlı ilerleme + durdur | GUI + root op | P1 | L | — | Sonnet (güvenlik incelemesi Opus) |
| **B4** | Karar kararlılığı: mevcut değere histerezis + çalıştırmalar arası diff | Autotune | P1 | M | B5 | Opus |
| **B2** | Leave-one-session-out doğrulama (LOO iyimserliğini düzelt) | Model | P1 | M | B1 | Opus |
| **A6** | Daha geniş parazit tespiti (IRQ, başkalarının disk I/O'su, GPU, ağ) | Ölçüm | P1 | M | — | Sonnet |
| **A10** | Depolama paketi: SSD yıpranma bütçesi + SLC önbellek tükenmesi | Ölçüm | P1 | M | — | Sonnet |
| **B6** | `--verify`: load fazı, tüm amaçlar, ardışık durdurma | Doğrulama | P2 | M | — | Opus |
| **C4** | GUI'den preset doğrulama (`--verify`) ve geçmişi | GUI | P2 | M | C3 | Sonnet |
| **B3** | Tahmin–gözlem kalibrasyon günlüğü (confirm + verify) | Model | P2 | S | — | Sonnet |
| **A7** | Referans koşu sıklığı: rastgele konum + drift'e göre uyarlama | Tasarım | P2 | M | B1 | Opus |
| **A11** | Pil ölçümü: çoklu batarya, `energy_now` farkı, birim kontrolü | Ölçüm | P2 | S | — | Sonnet |
| **C5** | Field A/B sayfası + MangoHud günlüğünün otomatik açılması | GUI + gamemode | P2 | L | — | Sonnet |
| **A9** | Ölü `MemBwGbs` metriği: bellek bant genişliği/gecikme probu ya da kaldır | Benchmark | P2 | M | — | Sonnet |
| **A8** | Koşu içi prob sırası etkisi (sabit sıra → dönen sıra + kovaryat) | Benchmark | P2 | M | A1, B1 | Opus |
| **C6** | Taban çizgisi ve kanıt (stall payları) grafikleri | GUI | P2 | S | A4, C2 | Sonnet |
| **C7** | Ağırlık penceresi: uyarlanmış ağırlıkları ve nedenini göster | GUI | P2 | S | — | Sonnet |
| **D2** | Signature dosyası için proptest (round-trip, bozuk girdi, eski sürüm) | Altyapı | P2 | S | — | Sonnet |

Önerilen sıra (fazlar): **Faz 0** B1 · D1 · B5 → **Faz 1** A3 · A1 · A4 · A5 → **Faz 2** C1 · C2 · C3 · A2 → **Faz 3** B4 · B2 · A6 · A10 · B6 · C4 → **Faz 4** geri kalanlar.

---

## 2. Madde kartları

Her kartta: **Sorun (kanıt)** → **Yapılacak** → **Dosyalar** → **Kabul ölçütü** → **Dikkat**.
Satır numaraları bu arşivdeki koda göre; kaymış olabilir, fonksiyon adlarıyla aranmalı.

### B1 — Gerçekçi simülatör + karar-kalitesi regresyon testi (P0, L, Opus)

**Sorun.** `centurion-calibrate.rs` içindeki `mod sim` (`truth`, `noisy`, `factors`; ~satır 2644–2780) 16 ikili knob, tek amaç,
iid gürültü, oturum ofseti / drift / aykırı değer / sıcaklık yok. AUTOTUNE.md tasarım kararlarını bu simülatörle gerekçelendiriyor
("0.6436 vs 0.6457"); ama modelin asıl zorlandığı durumlar simülatörde hiç yok. Sonraki her istatistik değişikliği (A2, A3, A7, B2)
bu simülatöre karşı doğrulanmalı.

**Yapılacak.**
- `sim` modülünü ayrı dosyaya taşı (`src/sim.rs`, `#[cfg(test)]` ya da `feature = "sim"`), senaryo yapısı:
  - 40–60 knob; ikili, sırasız seçim ve sayısal merdiven (doz-yanıt eğrisi tepe noktalı) karışık;
  - 4 amaç, knob başına etki vektörü (ödünleşimler: latency + / throughput −);
  - oturum ofseti, OU drift, ağır kuyruk (Student-t ν=3) + %2 aykırı koşu, metrik başına farklı gürültü;
  - nuisance kovaryatlar: başlangıç sıcaklığı (tasarım sırasıyla korele), güç bütçesi (A2 için);
  - "kör" knob'lar: gerçek etkisi var ama benchmark'ın göremediği amaçta (kural ≠ ölçüm durumu);
  - periyodik parazit (A7 için).
- Ölçütler: amaç başına yanlış karar oranı, utility regret (gerçek optimum − seçilen), %90 aralık kapsaması, sahte "seen" oranı.
- `cargo test --release -p centurion-helpers sim_regression -- --ignored` sabit tohumlarla çalışsın, sonuçları
  `crates/centurion-helpers/tests/sim-baseline.json` ile karşılaştırsın (tolerans: her ölçüt için ±%3 mutlak).

**Kabul ölçütü.** Mevcut kodla baseline dosyası üretilir ve commit'lenir; test baseline'a karşı geçer. Rapor çıktısı
(`--nocapture`) her senaryo için 4 ölçütü tablo halinde basar.

**Dikkat.** Mevcut `sequential_design_beats_one_at_a_time_at_equal_runs` ve `sweep` testleri bozulmasın; eski basit senaryo bir
senaryo olarak kalsın. Simülatör üretim koduna `model.rs`'deki gerçek `Problem/Joint` API'si üzerinden girmeli, kopya mantık yazılmamalı.

### D1 — AUTOTUNE.md ↔ kod tutarlılık testi (P0, S, Sonnet)

**Sorun.** AUTOTUNE.md ~1000 satır ve kesin sayılar içeriyor (SEEN_Z 2, KEEP_Z 1, MARGIN 0.03, ağırlık tablosu, eşikler).
Kod AI modellerle değiştirildikçe doküman sessizce kayar. Örnek: doküman "seen" için "bu amacın tarttığı objective'ler" diyor;
`seen_in` (`autotune.rs` ~1128) ise `0..4` tüm objective'leri döngüyle geziyor (A3'te netleştirilecek).

**Yapılacak.** `tests/doc_consistency.rs`: AUTOTUNE.md'deki ağırlık tablosunu (Gaming/Desktop/Throughput/Power save satırları)
parse edip `Weights::for_goal` ile; belirli sabitleri (`MARGIN`, `SEEN_Z`, `KEEP_Z`, `MODESTY`, cap'ler 1100/1100/600/300) regex
ile bulup koddaki değerlerle karşılaştır. GUI'deki `fallbackWeights` (`optimizetab.cpp` ~1750) kopyası da aynı test içinde
(dosyadan regex) kontrol edilsin.

**Kabul ölçütü.** Test şu an geçer (ya da gerçek bir uyumsuzluk bulur ve doküman/kod düzeltilir); bir sabiti değiştirip testin
kırıldığı gösterilir.

### B5 — Kararlara yapısal kaynak / güven alanı (P0, M, Sonnet)

**Sorun.** `Decision { key, value, why: String }` (`autotune.rs` ~933). Kararın kaynağı (kural / tahmin / ölçüm / joint pass /
pinned / evidence-driven), "seen" durumu, kazanç ve belirsizliği yalnızca İngilizce düzyazıda. `scores` haritası JSON'a giriyor
ama GUI göstermiyor. GUI'nin rozet, güven sütunu veya filtre göstermesi imkânsız.

**Yapılacak.** `Decision`'a opsiyonel alanlar: `source` (enum: `structural`, `estimate`, `measured`, `joint`, `pinned`,
`evidence`, `retired`, `audit`), `seen: Option<bool>`, `alone: Option<[mean, sd]>`, `in_context: Option<[mean, sd]>`,
`n: Option<f64>`, `risk: f64`. `assemble` çıktısına `"decisions": [...]` dizisi eklenir; `rationale` aynen kalır (geri uyum).
Notlar `constraints` içine karışmasın: `"notes"` ayrı dizi olsun, `constraints` sadece kısıtlar (GUI şu an her şeyi
`constraint:` önekiyle basıyor, `optimizetab.cpp` ~1951).

**Kabul ölçütü.** Her `Rules::set` çağrısı bir `source` üretir (test: bir signature fixture'ı ile 4 amacın her kararının `source`'u
boş değil). Eski GUI yeni JSON'la çalışmaya devam eder. `centurion-autotune <goal> --json` yeni alanları gösterir.

### A3 — "Seen" testi: çoklu karşılaştırma + histerezis (P0, M, Opus)

**Sorun.** `seen_in` (`autotune.rs` ~1128): bir knob'un *herhangi* bir değeri, *herhangi* bir objective'de |m| ≥ 2σ ise "görüldü".
4 değerli bir knob × 4 objective = 16 test; bağımsız varsayımla sahte "seen" olasılığı ≈ %50. "Seen" anahtarı kararın kaynağını
değiştiriyor (kural mı ölçüm mü), yani gürültü kuralın yerine geçebiliyor. Ayrıca durum oturumdan oturuma titreyebilir → preset
dalgalanması.

**Yapılacak.**
1. Önce B1 simülatöründe saf-gürültü senaryosunda mevcut sahte-seen oranını ölç, rapora yaz.
2. Testi değiştir: knob'un etki vektörü (değerler × ağırlıklı objective'ler) için posterior kovaryansla Mahalanobis χ² testi;
   kovaryans yoksa Šidák düzeltmeli z: `z = Φ⁻¹(1 − α/(2m))`, `m` = test sayısı, α = 0.05.
3. Sadece amacın ağırlığı > 0 olan objective'leri say (dokümanla hizala) veya dokümanı koda göre düzelt — hangisi doğruysa
   gerekçesiyle seç.
4. Histerezis: önceki durum fit-store'da tutulur; "görülmedi → görüldü" için z_enter, "görüldü → görülmedi" için z_exit < z_enter.
5. B5'in `seen` alanını doldur; rapor notu "N knob görüldü, M'si kıl payı".

**Kabul ölçütü.** Saf gürültüde sahte-seen oranı ≤ %5 (knob başına); 3σ gerçek etkili knob ≥ %90 görülür; B1 regresyonu
kötüleşmez. Histerezis için test: z 2.1 ↔ 1.9 arası gidip gelen knob'un durumu değişmez.

### A1 — Koşu başına telemetri (P0, L, Opus)

**Sorun.** Koşular yalnızca sonuç metriklerini kaydediyor. Bir knob throughput'u değiştirdiğinde bunun saatten mi, IPC'den mi,
yoksa koşu ortasında bir güç/ısı limitine takılmaktan mı geldiği bilinmiyor. `bench.rs` ve `Runner::run` (`centurion-calibrate.rs`
~816) içinde APERF/MPERF, `scaling_cur_freq`, C-state residency veya throttle sayacı okunmuyor. `pmtable.rs` (Ryzen limit
nedenleri) var ama yalnızca `ryzen-co-helper` kullanıyor.

**Yapılacak.** Yeni modül `src/runtel.rs`:
- Efektif saat: `perf_event_open` ile `cycles` / `ref-cycles` (CPU başına, kullanıcı+çekirdek) ya da `/dev/cpu/*/msr` APERF/MPERF
  (msr modülü yüklüyse). Hiçbiri yoksa `scaling_cur_freq` örneklemesi (düşük güven bayrağıyla).
- C-state residency: `/sys/devices/system/cpu/cpu*/cpuidle/state*/{usage,time}` farkları (durum başına pay).
- Throttle: Intel `/sys/devices/system/cpu/cpu*/thermal_throttle/{core,package}_throttle_count` farkı; Ryzen'de
  `pmtable::sample()` ile her limitin değer/limit oranı (STAPM, PPT fast/slow, TDC, EDC, THM) koşu başı ve sonu.
- Prob başına RAPL (şu an yalnızca `GameW` ve load'da `PkgW`).
- `Row`'a opsiyonel `tel` nesnesi (serde: yoksa boş). Bunlar **objective değil**, teşhis + nuisance.
- Kullanımlar: (a) `--show` ve autotune raporunda mekanizma ("throughput +%4, efektif saat +%4: kazanç saatten"),
  (b) koşu sırasında limit yüzde 98'i aştıysa ağırlık düşürme (busy/forced-idle ile aynı desen, quality kaydına sayaç),
  (c) A2'nin girdisi.

**Dosyalar.** `bench.rs` (prob başına sarmalayıcı), `centurion-calibrate.rs` (`Runner::run`, `Done`, `compute_rows`),
`calib.rs` (`Row`, okuma/yazma, `session_quality`), yeni `runtel.rs`.

**Kabul ölçütü.** `Rapl::from_dir` deseniyle sahte sysfs ağacına karşı birim testleri (cpuidle, thermal_throttle, cpufreq).
Eski signature dosyası okunur, yeni alanlarla yazılır, tekrar okunur (round-trip). Telemetri okuma koşu başına < 5 ms ek yük
(test: 1000 okuma süresi). Kök değilken / perf_event_paranoid yüksekken sessizce geri düşer.

**Dikkat.** dGPU'yu uyandıracak hiçbir şey okunmasın (D3cold; mevcut kod bu konuda dikkatli). perf fd'leri koşu başına
açılıp kapanmasın, oturum boyunca açık kalsın.

### A2 — Güç-bütçesi durumu kovaryatı (P1, M, Opus)

**Sorun.** Termal kapı sıcaklığı bekliyor ama zaman-ortalamalı güç limitlerini değil: Intel PL1/PL2 tau (28–56 s), AMD mobil
STAPM / PPT slow (dakikalar). 15 s'lik heat soak bütçeyi tüketiyor; sonraki "cool" koşu sıcaklık normale dönse bile PL1'de
başlayabilir. `ctx.heat=hot` şu an sıcaklık + bütçe etkisini karıştırıyor ve bütçe durumu tasarım sırasıyla korele (drift gibi
görünür ama knob'lara sızar).

**Yapılacak.** Koşu başında, powercap `constraint_*_time_window_us` (Intel) veya pmtable STAPM zaman sabitiyle (yoksa 60 s varsayım)
paket gücünün EWMA'sı → `budget = ewma / PL1` (0–1+). `Row.tc` gibi ikinci bir nuisance terim (`model.rs`'de sıcaklık teriminin
yanına; prior ölçeği küçük, oturumlar arası paylaşılan). Bütçe ≥ 0.9 iken başlayan cool koşu önce bekler (thermal_gate'e benzer,
üst sınırlı).

**Kabul ölçütü.** B1'e "bütçe nuisance'ı" senaryosu eklenir: terim yokken knob etkisinde bias görülür, terimle bias ≤ σ/2.
`MODEL_VERSION` artırılır (fit-store doğru şekilde geçersizleşir; test var).

### A4 — Oturum taban çizgisi geçmişi (P1, M, Sonnet)

**Sorun.** `ref_levels` (`centurion-calibrate.rs` ~719) yalnızca güç seviyelerini ve yalnızca **son** oturumu tutuyor
(`cal.ref_w.insert` her seferinde üzerine yazar, ~1218). Mutlak performansın zamanla değişmesi (termal macun eskimesi, BIOS
güncellemesi, arka planda yeni servis, yanlış güç modu) görülmüyor; oturumlar log-oran ile kendi referansına göre ölçüldüğü için
bu kayma sessizce geçiyor.

**Yapılacak.** Signature'a `baselines`: oturum başına her metriğin referans koşu medyanı + robust sd, ortam kimliği (A5), bağlam,
zaman (son 64 oturum). Oturum başında ilk 2 referans koşudan sonra geçmiş medyanla karşılaştır: |sapma| > 4·MAD ise uyarı +
quality kaydına `baseline_shift`; `--strict` ile oturumu temiz bitir. Autotune notu: "Son oturumda CPU tek iş parçacığı taban
çizgisi geçmişe göre −%7".

**Kabul ölçütü.** Birim testi: sentetik geçmiş + kaymış oturum → uyarı; normal oturum → uyarı yok. Eski dosyada `baselines` yoksa
sorun yok.

### A5 — Ortam dönemleri (P1, M, Sonnet)

**Sorun.** `Fingerprint.bios` kaydediliyor ama hiçbir yerde kullanılmıyor (`calib.rs` ~468–481; yorum: "BIOS updates are kept,
noted by records' dates"). Satırlar yalnızca çekirdek major.minor ile ağırlıklanıyor. BIOS, mikrokod, `mitigations=`,
`amd_pstate=`, `preempt=` gibi cmdline parametreleri ve RAM profili (EXPO/XMP açık/kapalı, hız) knob etkilerini değiştirir.

**Yapılacak.** Satıra `env` (kısa hash) + signature'da `envs: {hash: {bios, microcode, kernel_release, cmdline_relevant, ram_speed}}`.
RAM hızı `memory_spd.rs` veya dmidecode'suz yoldan (`/sys/devices/system/edac`, yoksa boş). Ağırlık: farklı BIOS veya mikrokod
×0.7, farklı `mitigations`/`preempt` ×0.5 (çekirdek kuralıyla çarpımsal). Rapor notu: "N koşu başka BIOS'ta (×0.7)".

**Kabul ölçütü.** Round-trip testi; ağırlık fonksiyonu için birim testi; `--show` env dağılımını yazar.

### C2 — `--show --json` / `--list --json` + GUI Calibration görünümü (P1, L, Sonnet)

**Sorun.** `--show` ve `--list` yalnızca metin üretiyor (`centurion-calibrate.rs` `show`, `show_model`); GUI signature'ı hiç
göstermiyor. Kullanıcı modelin neyi bildiğini, aşama ilerlemesini, kaliteyi göremiyor.

**Yapılacak.**
- CLI: `--show --json`, `--list --json` (kök gerekmez; mevcut metin aynı veriden üretilsin).
- GUI: Optimizations'ta yeni alt sekme "Calibration": faz başına aşama ilerlemesi (base→polish, %), satır/oturum sayısı, R²,
  %90 kapsama, varyans genişletme uyarısı, knob başına etki "forest plot"u (objective başına ortalama ± sd, seen işareti),
  güçlü etkileşimler (synergy/overlap/conflict), unsafe / soft-unsafe kümeler, oturum kalite geçmişi, verify geçmişi.
- Grafikler `gui/src/charts.*` ile.

**Kabul ölçütü.** JSON şeması `docs/AUTOTUNE.md`'ye eklenir; GUI boş signature, eski signature ve büyük (3000+ satır) signature
ile açılır (< 1 s, ağır iş QProcess'te). Metin `--show` çıktısı değişmez (snapshot testi).

### C1 — Autotune raporu v2 (P1, M, Sonnet)

**Sorun.** `showAutotuneReport` (`optimizetab.cpp` ~1922) kaynak, güven ve skor izini göstermiyor; notlar `constraint:` önekiyle
tek blokta; "değişenleri göster" filtresi yok.

**Yapılacak.** B5 alanlarıyla: Kaynak rozeti (Rule / Measured / Joint / Pinned / Evidence), Güven sütunu (kazanç ± sd, görülmedi
ikonu), "son çalıştırmadan beri değişti" işareti (B4 varsa), satır genişletince `scores` izi (değer → U), "Yalnızca değişenler"
onay kutusu, notlar ayrı açılır bölüm ("Notes / Evidence"). JSON'da yeni alan yoksa eski davranış.

**Kabul ölçütü.** Qt testi (mevcut `gui/tests` deseni): fixture JSON'larla tablo satır sayısı, rozet metinleri, filtre.

### C3 — GUI'den "Calibrate now" + canlı ilerleme (P1, L, Sonnet; güvenlik incelemesi Opus)

**Sorun.** GUI yalnızca "Calibrate at next boot" sunuyor (`scheduleBootCalibration`, ~1879). İlerleme sadece stderr'de `\r` satırı;
makine-okunur ilerleme yalnızca boot modu için (`calibrate-status.json`).

**Yapılacak.**
- `centurion-calibrate` her koşuda `/run/centurion/calibrate-progress.json` yazar: faz, aşama, koşu i/tahmini n, ETA, beklenen yanlış
  karar sayısı, son uyarılar (ekran değişti, meşgul, sürücü sıcak), quality özeti.
- Polkit'li yeni op (`tune-helper` `calibrate_now`): seçenekler beyaz listeli (budget 5–60, sessions 1–4, scene adı doğrulanır),
  süreç ayrık başlatılır; "Stop" → SIGINT (temiz restore zaten var).
- GUI: "Calibrate now…" diyaloğu (bütçe, oturum, bağlam), canlı ilerleme paneli, "makineyi kullanmayın" uyarısı, boot log görüntüleyici.

**Kabul ölçütü.** Op yalnızca beyaz listeli argümanlarla süreç başlatır (birim testi: kötü sahne adı, aşırı bütçe reddedilir).
İlerleme dosyası atomik yazılır. GUI kapanınca kalibrasyon devam eder; yeniden açılınca ilerlemeyi bulur.

**Dikkat.** Polkit seviyesi mevcut `calibrate_on_boot` ile aynı olmalı; komut satırı enjeksiyonu yok (argv dizisi).

### B4 — Karar kararlılığı (P1, M, Opus)

**Sorun.** Her yeni oturum veya küçük veri değişikliğinde marjinal kararlar çevrilebilir; kullanıcı neyin neden değiştiğini
göremez. Kodda incumbent / histerezis mantığı yok.

**Yapılacak.** `~/.local/state/centurion/autotune-history.jsonl` (amaç başına son 20 çıktı: kararlar, ağırlıklar, bağlam, signature
parmak izi). Yeni çalıştırmada, şu an uygulanmış / kaydedilmiş preset'teki değer için joint pass'te küçük bir incumbent bonusu
(δ = MARGIN/2): yeni öneri ancak alt sınırı `margin + δ`'yı geçerse değişir. Çıktıya `"diff_vs_last"`: değişen knob'lar ve neden
(yeni veri / ağırlık / bağlam / kural).

**Kabul ölçütü.** B1'de "aynı makine, 10 ardışık oturum" senaryosu: karar dalgalanma sayısı ölçülür, bonusla düşer, regret
kötüleşmez (≤ +%1).

### B2 — Leave-one-session-out doğrulama (P1, M, Opus)

**Sorun.** Model kalitesi (R², %90 kapsama, `var_scale` genişletmesi) satır bazlı LOO'dan geliyor. Aynı oturumun satırları ortak
ofset/drift paylaştığı için satır-LOO iyimser; gerçek kullanım ise "geçmiş oturumlarla öğren, yeni bağlamda tahmin et".

**Yapılacak.** LOSO: her oturum dışarıda bırakılarak (en çok son 8 oturum, maliyet sınırlı) tahmin; R²_loso, kapsama_loso.
`var_scale` hesabına LOSO z² medyanı da girsin (ikisinin büyüğü). Rapor ve `--show` iki değeri de göstersin.

**Kabul ölçütü.** B1'de oturum ofseti büyük senaryoda LOO kapsaması %90'ın üstünde, LOSO ise gerçek değere yakın çıkar;
autotune süresi fit-store isabetinde değişmez, ıska durumunda ≤ +%30.

### A6 — Daha geniş parazit tespiti (P1, M, Sonnet)

**Sorun.** `Foreign::busy_cpus` yalnızca başka kullanıcı-alanı süreçlerinin CPU zamanını sayıyor (> 0.6 CPU → ağırlık düşer).
IRQ fırtınası, başka süreçlerin disk I/O'su (idle/load fazında), GPU yükü, ağ trafiği görünmüyor.

**Yapılacak.** Koşu başı/sonu: `/proc/stat` `intr` ve `softirq` farkı (oturum referans koşularına göre oran), `/proc/diskstats`
farkı eksi kendi I/O'muz (IO fazı dışında), amdgpu `gpu_busy_percent` (dGPU yalnızca `runtime_status == active` ise; uyandırma yok),
ağ rx/tx bayt farkı. Eşik aşımı → ağırlık çarpanı + quality sayaçları, `--show`'da görünür.

**Kabul ölçütü.** Fixture dosyalarıyla birim testleri; eşikler sabit olarak tek yerde; quality JSON'u yeni sayaçları içerir.

### A10 — Depolama paketi: yıpranma ve SLC (P1, M, Sonnet)

**Sorun.** Bir IO koşusu ~1.5 × 256 MiB yazar; faz başına 600 satır sınırı, yüzlerce GB yazma demek. SSD'nin SLC önbelleği
oturum içinde tükenince yazma hızı düşer ve bu knob'a atfedilebilir. Yazılan toplam hiçbir yerde sayılmıyor.

**Yapılacak.** NVMe SMART `data_units_written` (sysfs/`nvme` ioctl; yoksa `/sys/block/*/stat` yazılan sektör) oturum başı/sonu
farkı → signature'da kümülatif `io_written`. Günlük varsayılan sınır (ör. 20 GiB, `--io-budget`). `bench::io` yazmayı 32 MiB
parçalara bölüp parça başına hız kaydetsin; ilk parçalara göre > %50 düşüş → `slc_drop` bayrağı, koşu ağırlığı düşer.

**Kabul ölçütü.** Sınır aşılınca IO fazı temiz atlanır ve nedeni yazılır; birim testi parça-hız serisinden düşüşü yakalar.

### B6 — `--verify` genişletme (P2, M, Opus)

**Sorun.** Verify yalnızca idle (+ `--io`) paketinde; load fazı yok; tek amaç; tur sayısı sabit (varsayılan 4).

**Yapılacak.** `--phase load` (ballast ile), `--goal all` (tüm amaçların ağırlıklı kazancı aynı veriden), ardışık durdurma:
her ABBA bloğundan sonra t-aralığı 0'ı dışlıyorsa ya da en çok tur sayısına ulaşıldıysa dur (min 3 blok).

**Kabul ölçütü.** `verify_pairs_runs_by_round_and_position` benzeri sentetik testler; durdurma kuralı için yanlış-pozitif oranı
simülasyonla ≤ %5.

### C4 — GUI'den preset doğrulama (P2, M, Sonnet)

Kaydedilmiş preset için "Verify…" düğmesi (C3'ün polkit/ilerleme altyapısıyla), sonuç kartı: objective başına paired kazanç ± CI,
"as predicted / SURPRISE", geçmiş (signature `verify` alanı). Kabul: fixture JSON'la Qt testi.

### B3 — Tahmin–gözlem kalibrasyon günlüğü (P2, S, Sonnet)

Confirmation ve verify her seferinde (tahmin ortalama, sd, gözlem) üçlüsünü signature'a eklesin (son 200). `--show`: PIT
histogramı benzeri özet ve son N'deki kapsama. Kapsama < %70 ise autotune raporunda uyarı. Kabul: birim testi.

### A7 — Referans koşu sıklığı (P2, M, Opus)

**Sorun.** Referans koşu sabit olarak her 6. koşu. 30–60 s periyotlu arka plan işleri (timer, cron) bu sıklıkla örtüşebilir;
drift hızlıysa 6 seyrek, yavaşsa gereksiz.

**Yapılacak.** Her 5–7'lik blokta rastgele konum; oturumun tahmini OU drift'i / referans sd'si yüksekse 4'e, düşükse 8'e kadar
uyarlama. Kabul: B1'e periyodik parazit senaryosu; regret ve kapsama iyileşir ya da aynı kalır.

### A11 — Pil ölçümü sağlamlığı (P2, S, Sonnet)

`battery_uw` (`bench.rs` ~440) ilk deşarj olan bataryada durur (BAT0+BAT1 olan makinelerde eksik), `power_now` birim/işaret
kontrolü sınırlı. Yapılacak: tüm bataryaları topla; DEV fazında uzun pencerede `energy_now` (yoksa `charge_now × voltage`) farkıyla
ikinci tahmin, ikisi %20'den fazla ayrışırsa uyarı; makul aralık dışı değerleri (ör. > 300 W) reddet. Kabul: sahte power_supply
ağacıyla birim testleri.

### C5 — Field A/B sayfası + otomatik MangoHud günlüğü (P2, L, Sonnet)

**Sorun.** A/B dönüşümü var (`centurion-gamemode` `field_ab`) ama kare-süresi günlükleri elle toplanıp `sudo centurion-calibrate
--field` ile içeri alınıyor; GUI'de sonuç yok.

**Yapılacak.** A/B açık oyunlarda gamemode `MANGOHUD_CONFIG`'e `autostart_log`, `output_folder=~/.local/state/centurion/frametimes`
eklesin (kullanıcının mevcut MangoHud ayarını ezmeden). Kullanıcı tetiklemeli içe aktarma tune-helper op'u üzerinden. GUI: oyun
başına A/B oturum sayısı, medyan / %1 low / pacing farkı ± CI, "yeterli veri yok" durumu. Kabul: `field.rs` testleri + Qt fixture testi.

### A9 — Ölü `MemBwGbs` metriği (P2, M, Sonnet)

`Metric::MemBwGbs` tanımlı (`calib.rs` ~79) ama hiçbir prob üretmiyor. Ya kaldır ya da çok iş parçacıklı triad bant genişliği +
4×L3 üzerinde boşta bellek gecikmesi probu ekle (APU'da iGPU paylaşımlı bellek, DF C-state, CCD yerleşimi, THP için anlamlı).
Ekleme yapılırsa `BENCH_VERSION` artışının etkisi (eski satırlar ×0.6) değerlendirilsin. Kabul: prob testi + koşu süresi bütçesi.

### A8 — Koşu içi prob sırası (P2, M, Opus)

`cpu_suite` ve `run_idle` probları hep aynı sırada koşar; önceki probun ısı/bütçe etkisi sonrakine geçer ve güç knob'larıyla
etkileşir. Yapılacak: koşular arası Latin-kare döndürme + sıra indeksini nuisance olarak kaydet. **Önce donanımda** referans
koşu sd'sinin sırayla değiştiğini gösteren bir ölçüm yapılmalı; etki yoksa madde kapatılır.

### C6 — Taban çizgisi ve kanıt grafikleri (P2, S, Sonnet)

A4'ün `baselines` geçmişi ve autotune `evidence_summary` (stall payları, saatler, boot sayısı) için Calibration sekmesinde zaman
grafikleri.

### C7 — Ağırlık penceresi (P2, S, Sonnet)

`editAutotuneWeights` varsayılanları gösteriyor ama makine geçmişine göre uyarlanmış etkin değerleri (footprint/storage/load/heat)
ve nedenini göstermiyor. Yapılacak: `autotune_defaults` op'u uyarlanmış değerleri ve not metnini de döndürsün; diyalog her satırda
"varsayılan 0.4 → etkin 0.55 (bellek stall payı %0.6)" göstersin.

### D2 — Signature proptest'leri (P2, S, Sonnet)

`tests/properties.rs` mevcut; signature.json için round-trip, rastgele bozuk girdi (panik yok, `damaged-TIME` yolu), eski sürüm
göçü ve 8 MiB sınırına yaklaşma özellik testleri eklensin.

---

## 2b. Yeni knob'lar (E serisi)

Şu an tabloda 169 knob var (`tune.rs` `TUNABLES`): EEVDF, BORE, Infinity, 7.2 LLC dengeleme, sched_ext başlat/durdur, THP/mTHP,
MGLRU, zswap, blok kuyruğu, cihaz güç yönetimi. Aşağıdakiler **eksik** ve hedef makinede (Legion Pro 7 16AFR10H: 9955HX3D,
RTX 5080, 2× Samsung NVMe, RTL8125, MT7927; çekirdek 7.3.0-cachyos) anlamlı olanlar.

Kapsam dışı: **MuQSS** 2021'de (5.12) bırakıldı, 7.x için yok. `wq.power_efficient` bilinçli olarak emekliye ayrılmış
(`RETIRED_KEYS`), geri getirilmez.

| ID | Başlık | Alan | Öncelik | Efor | Model |
|---|---|---|---|---|---|
| **E1** | sched_ext: modlar/argümanlar, scx_loader entegrasyonu, dinamik allowlist, sağlık izleme | Scheduler | P0 | L | Opus (güvenlik) + Sonnet |
| **E2** | irqbalance çakışma tespiti (`irq.affinity` sessizce geri alınıyor) | Scheduler/IRQ | P0 | S | Sonnet |
| **E3** | Oyun için görev başına EEVDF slice (`sched_attr.sched_runtime`) | Launch bloğu | P1 | M | Opus |
| **E4** | Elevator ayarları (`queue/iosched/*`: kyber, mq-deadline, bfq) | Storage | P1 | M | Sonnet |
| **E5** | zram: ikincil sıkıştırma (recompress) + idle işaretleme, boot zamanı yapılandırma | Memory | P1 | M | Sonnet |
| **E6** | Threaded NAPI + NAPI kthread'lerini CCD rolüne yönlendirme | Network | P1 | M | Sonnet |
| **E7** | BORE ve EEVDF özelliklerinin dinamik keşfi (eksik `sched_burst_*`, `PLACE_*`) | Scheduler | P1 | S | Sonnet |
| **E8** | `ppt_cpu_cl` kalibrasyon bağlamına girmiyor | Calibration | P1 | S | Sonnet |
| **E9** | Project C (BMQ/PDS) tespiti + `kernel.yield_type` | Scheduler | P2 | S | Sonnet |
| **E10** | Ağ: `tcp_notsent_lowat`, `tcp_fastopen`, `gro_flush_timeout`/`napi_defer_hard_irqs`, busy poll | Network | P2 | M | Sonnet |
| **E11** | Küçük bellek knob'ları: `max_ptes_shared`, `use_zero_page`, `compact_unevictable_allowed`, `dirtytime_expire_seconds`, KSM advisor | Memory | P2 | S | Sonnet |
| **E12** | btrfs async discard sınırları (`/sys/fs/btrfs/<uuid>/discard/*`) | Storage | P2 | S | Sonnet |
| **E13** | Arka plan cgroup'u: oyun sırasında diğerlerine `cpu.weight` / `cpu.idle` | Scheduler | P2 | M | Opus |

**Durum (2026-10-31).** E2 (irqbalance durdur/geri başlat), E7 (`PLACE_*` satırları + `sched_burst_*` / features keşfi) ve
E8 (`ppt_cpu_cl` `FW_ATTRS` içinde) zaten koddaydı. E3, E4, E5, E6, E9–E13 eklendi (AUTOTUNE.md "Settings added with the E
series"). E5 düzeltmesi: `recomp_algorithm` başlatılmış zram aygıtında `-EBUSY` döner, canlı knob değildir; yalnızca idle
işaretleme + `recompress` canlı (gamemode: oyun başı işaretle, oyun sonu sınırlı yeniden sıkıştır). Açık: E1.

**Her yeni knob için ortak kontrol listesi** (her E maddesinin kabul ölçütüne dahildir):
1. `tune.rs` satırı: yardım metni (diğerleriyle aynı üslup), `Kind`/`Options`, `debugfs`/`caution`/`vendor`; dosya yoksa satır gizlenir.
2. `defaults.rs` boot snapshot'ına girer mi (radyo anahtarları gibi istisna mı)?
3. `tune-helper` audit sınırları (reddet / uyar + fix).
4. Kalibrasyon: plana girer mi, yoksa `calib::EXCLUDED` + gerekçe mi? Merdiven değerleri autotune'un güven bölgesi içinde mi?
5. Autotune: yapısal kural mı, puanlanan knob mu (etki vektörü + risk), yoksa yalnızca elle mi?
6. Journal/restore yolu ve `RETIRED_KEYS` ile çakışmama; `tune.rs` testindeki anahtar listesine ekleme.
7. AUTOTUNE.md / HANDBOOK güncellemesi (D1 testi).

### E1 — sched_ext'i tamamla (P0, L)

**Sorun (kanıt).** `scx_set` (`tune.rs` ~1981) scheduler'ı **argümansız** başlatıyor: lavd'nin `--performance/--powersave`,
bpfland'in `-m performance -s/-S/-l` gibi modları kullanılamıyor. Allowlist sabit (`SCX_NAMES`: lavd, bpfland, rusty, flash,
cosmos, p2dq, tickless, layered); güncel scx deposunda beerland, cake, forge, pandemonium, rustland gibi yenileri var, `chaos`
ise test amaçlı gecikme enjekte eden bir scheduler (asla sunulmamalı). `scx_loader` (CachyOS'ta varsayılan yönetici, D-Bus
`org.scx.Loader`, sistem veriyolu) hiç hesaba katılmıyor: o çalışırken Centurion kendi kopyasını başlatırsa ikisi çakışır ve restore
yanlış durumu geri yükler. Scheduler'ın watchdog tarafından atılması (ejection) hiçbir yerde kayıt edilmiyor.

**Yapılacak.**
- Yeni knob `sched.ext_mode`: `auto | gaming | lowlatency | powersave | server`. Mod → argüman eşlemesi: önce
  `/etc/scx_loader/config.toml` (yoksa `/etc/scx_loader.toml`) içindeki `[scheds.<ad>]` dizileri, yoksa kodda küçük bir tablo
  (scx_loader'ın örnek yapılandırmasındaki değerler: bpfland gaming `-m performance`, lavd gaming `--performance`, powersave
  `--powersave` …). Argümanlar yalnızca bu tablodan gelir, kullanıcıdan serbest metin alınmaz.
- scx_loader varsa (D-Bus adı sahipli): `StartScheduler / SwitchScheduler(name, mode)` ve `StopScheduler` ile sür; baseline'a
  önceki `CurrentScheduler` + `SchedulerMode` yazılır, restore onları geri getirir. Yoksa mevcut doğrudan başlatma yolu.
- Allowlist: güvenilir dizindeki `scx_*` dosyaları keşfedilir, bilinen-güvenli listeyle kesişim (chaos ve demo/test
  scheduler'ları hariç); bilinmeyen yeni ad "experimental" olarak işaretli sunulur.
- Sağlık: `/sys/kernel/sched_ext/` altındaki `state`, `enable_seq` ve (varsa) olay sayaçları + çekirdek günlüğündeki sched_ext
  "disabled/error" satırları `stability.rs` olay sınıflandırmasına girer (ağırlık 1). Atılan scheduler'ın knob değeri drift olarak
  raporlanır.
- Kalibrasyon: sched_ext **ve** modu ölçülebilir (bağlam pseudo-knob'u değil, gerçek knob); ama EEVDF/BORE knob'larıyla
  karşılıklı dışlayıcı: scx etkinken `sched.*`, `kernel.sched_bore*` satırları anlamsız. Tasarımda bu çiftler "geçersiz kombinasyon"
  olarak işaretlenmeli (model tarafında `bad` kümeleri gibi).

**Kabul ölçütü.** Birim testleri: config.toml ayrıştırma, mod → argüman çözümü, allowlist (chaos reddedilir, `../` reddedilir),
scx_loader var/yok yolları için sahte D-Bus (veya soyutlanmış arayüz). Restore sonrası önceki scheduler + mod aynen döner.

### E2 — irqbalance çakışması (P0, S)

**Sorun.** Kod tabanında `irqbalance` hiç geçmiyor. `irq.affinity` (oyunda IRQ'ları diğer CCD'ye taşıma) uygulandıktan sonra
çalışan bir irqbalance daemon'u birkaç saniye içinde dağıtımı geri alır; kullanıcı ayarın uygulandığını sanır.

**Yapılacak.** `irq.affinity` / `wq.cpumask` / game isolate uygulanırken irqbalance süreci aranır; varsa: (a) GUI satırında uyarı,
(b) seçenek: oyun süresince `IRQBALANCE_BANNED_CPULIST` mantığıyla değil, servisi duraklatıp restore'da geri başlatmak (journal'a
yazılır; systemd ve OpenRC ikisi de). Drift izleyici bu knob'u 10 s sonra tekrar okuyup sapmayı "irqbalance" nedeniyle raporlar.

**Kabul ölçütü.** Süreç tespiti ve journal girişi için birim testi; servis zaten durmuşsa restore onu başlatmaz.

### E3 — Oyun için görev başına EEVDF slice (P1, M)

**Arka plan.** EEVDF 6.12'den beri `sched_setattr()`'ın `sched_runtime` alanını normal görevler için slice önerisi olarak
kabul ediyor (100 µs – 100 ms). Kısa slice daha erken sanal son tarih demek; uyanan görev çalışanı daha erken kesebilir: daha
tutarlı kare zamanlaması, karşılığında çok kısa seçilirse throughput kaybı.

**Yapılacak.** Launch bloğuna (`timerslack_ns` yanına) `slice_us` (kapalı / 1000 / 2000 / 3000). `centurion-gamemode` oyunun
süreç ağacına (yeni thread'ler dahil: periyodik tarama ya da clone takibi, mevcut timerslack yolu nasıl yapıyorsa) uygular.
BORE etkinken veya sched_ext varken etkisi farklıdır: bu durumda satır not düşer. Kalibrasyon: game-loop ve cache-loop probları
kendi thread'lerine bu slice'ı uygulayarak "launch.slice_us" knob'unu ölçebilir (yalnızca o probun thread'leri, sistem geneli değil).

**Kabul ölçütü.** `sched_getattr` ile geri okuma testi (çekirdek desteklemiyorsa satır gizli); oyun bitince değer kalmaz
(süreç ölür, sorun yok) ama gamemode'un kendi thread'ine sızmaz.

### E4 — Elevator ayarları (P1, M)

`blk.scheduler` var ama seçilen elevator'ün kendi ayarları yok: kyber `read_lat_nsec` / `write_lat_nsec`, mq-deadline
`read_expire` / `write_expire` / `fifo_batch` / `writes_starved`, bfq `low_latency` / `slice_idle`. Bunlar yalnızca o elevator
etkinken var (koşullu satır, `blk.scheduler` ile etkileşim). NVMe'de varsayılan `none` olduğu için çoğu zaman gizli kalır; ama
autotune mq-deadline/kyber seçerse ayarı da ölçülmeli. Kalibrasyonda `blk.scheduler` değeriyle iç içe tasarım: elevator ≠ o değer
iken bu knob'lar "uygulanamaz" (refused değil, koşullu). IO fazı ölçer.

**Kabul ölçütü.** Koşullu uygulama testi (elevator değişince dosyalar kaybolur/gelir; journal sırası: önce elevator — mevcut
`todo.sort_by_key` kuralıyla uyumlu).

### E5 — zram (P1, M)

Autotune zram'ı tanıyor (`SwapKind::Zram`, swappiness 150) ama zram'ın kendisi ayarlanmıyor. Canlı değiştirilebilenler:
`recomp_algorithm` (ikincil, daha güçlü sıkıştırıcı; ör. zstd) + `idle` işaretleme + `recompress type=idle` (soğuk sayfaları
yeniden sıkıştırır, RAM kazandırır). `comp_algorithm` ve `disksize` ise reset (swapoff) ister: bunlar knob değil, "boot zamanı
yapılandırma" olarak (zram-generator / OpenRC zram-init dosyasına öneri) raporlanır, canlı yazılmaz. Periyodik recompress bir
zamanlayıcı ister: centurion-tune boot op'u veya gamemode dışı boşta zamanlarında. Kabul: zram yoksa satırlar gizli; recompress
yalnızca idle sayfalarda ve sınırlı CPU ile çalışır (bir oturumda en fazla N MiB).

### E6 — Threaded NAPI (P1, M)

`/sys/class/net/<if>/threaded` = 1 iken ağ softirq işi `napi/<if>-<n>` kthread'lerinde çalışır ve bunlar diğer kthread'ler gibi
CCD rolüne (oyun dışı CCD) yönlendirilebilir. Şu an `irq.affinity` IRQ'yu taşıyor ama softirq işi hâlâ IRQ'nun düştüğü CPU'da.
RTL8125 ve MT7927 için: knob + game isolate yolunda kthread affinity. Kabul: sürücü desteklemiyorsa yazma reddi → satır gizli;
restore kthread'leri serbest bırakır.

### E7 — BORE / EEVDF özelliklerinin dinamik keşfi (P1, S)

BORE için sabit 6 sysctl var; çekirdek sürümüne göre başka `sched_burst_*` dosyaları olabilir. EEVDF'te `PLACE_LAG`,
`PLACE_DEADLINE_INITIAL`, `PLACE_REL_DEADLINE` gibi özellikler tabloda yok (mevcut: next_buddy, run_to_parity, preempt_short,
delay_dequeue, hrtick, sis_util, ttwu_queue, cache_hot_buddy, wakeup_preemption). Yapılacak: `/proc/sys/kernel/sched_burst_*` ve
debugfs `sched/features` içinde tabloda olmayan girdileri "Discovered" olarak (yalnız elle, kalibrasyon dışı) göster; bilinen ve
anlamlı olanları (yukarıdaki `PLACE_*`) tam satır olarak ekle ve kalibrasyona al. Kabul: keşif listesinde bilinen anahtar tekrar
görünmez.

### E8 — `ppt_cpu_cl` kalibrasyon bağlamında yok (P1, S)

Firmware'de `ppt_cpu_cl` var (bu makinede 70, aralık 30–100) ve `lib.rs` yazma sırası onu biliyor (`FW_PL_AFTER`), ama
`calctx::FW_ATTRS` listesinde yok: kalibrasyon bağlamı (`ctx.limits`) bu limiti kaydetmiyor; değiştiğinde iki farklı bağlam aynı
kimliği alır. Yapılacak: listeye ekle; `ctx_ref` / bağlam kimliği geri uyumu (eski oturumlarda alan yok = "bilinmiyor",
yeni bağlam açmaz). Kabul: `scene_power_and_description` testine alan eklenir.

### E9 — Project C tespiti (P2, S)

Project C (BMQ/PDS) 7.x'te kararsız: Liquorix Eylül 2026'da 7.2 için ekleyip ~20 saat sonra geri aldı. Yine de çekirdek
gelirse: `/proc/sys/kernel/yield_type` (0/1/2) tek canlı ayar; zaman dilimi `sched_timeslice=` boot parametresi (rapor notu,
knob değil). EEVDF/BORE/LLC/sched_ext satırları dosyaları olmadığından zaten gizlenir; profil özetinde "alt scheduler: PDS/BMQ"
yazsın ve autotune'un scheduler kuralları bu durumda devre dışı kalsın. Kabul: sahte /proc ağacıyla tespit testi.

### E10 — Ağ ince ayarları (P2, M)

`net.ipv4.tcp_notsent_lowat` (BBR + fq ile gönderme tarafı bufferbloat'u azaltır), `tcp_fastopen`, arayüz başına
`gro_flush_timeout` + `napi_defer_hard_irqs` (kesme sayısı ↔ gecikme, güç), `net.core.busy_poll/busy_read` (gecikme düşer ama
CPU uyanık kalır: power objective'de yüksek maliyet, yalnızca elle). Oyunda yerel µs düzeyi kazanç internet RTT'si yanında küçük;
öncelik bu yüzden düşük. Kalibrasyon bunları göremez (ağ benchmark'ı yok) → rule-only, `EXCLUDED` gerekçesiyle.

### E11 — Küçük bellek knob'ları (P2, S)

`khugepaged/max_ptes_shared`, `transparent_hugepage/use_zero_page`, `vm.compact_unevictable_allowed` (RT gecikmesi için 0),
`vm.dirtytime_expire_seconds`, KSM advisor (`/sys/kernel/mm/ksm/advisor_mode`, `ksm.run` açıksa). Hepsi heap probu / load fazıyla
ölçülebilir ya da rule-only; her biri ortak kontrol listesinden geçer.

### E12 — btrfs async discard (P2, S)

`/sys/fs/btrfs/<uuid>/discard/{iops_limit,kbps_limit,max_discard_size}`: arka plan TRIM'in oyun sırasında I/O gecikmesine
sızmasını sınırlar. Yalnızca btrfs + `discard=async` ile görünür. Dosya sistemi UUID'si değişebileceği için anahtar bağlama göre
(`/`, `/home`) çözülür.

### E13 — Arka plan cgroup'u (P2, M)

Oyun cgroup'u (`isolate.rs`, cpuset partition) var; geri kalan iş için ağırlık yok. Oyun süresince kullanıcı oturumunun diğer
cgroup'larına `cpu.weight` düşürme veya `cpu.idle=1` (SCHED_IDLE benzeri). systemd ve OpenRC (unified cgroup) farkı, derleme gibi
bilinçli arka plan işlerini aç bırakma seçeneği. Restore kesin olmalı (journal). Kalibrasyonun contended window'u bunu ölçebilir.

---

## 3. Modele verirken (prompt şablonu)

Her maddeyi ayrı bir oturumda verin. Şablon:

```
Centurion deposunda docs/ROADMAP-AUTOTUNE.md içindeki <ID> maddesini uygula.

Bağlam: docs/AUTOTUNE.md (davranışın tam tarifi) ve maddedeki dosyalar. Mevcut davranışı bozmadan,
yalnızca bu maddenin istediği değişikliği yap; komşu kodu yeniden düzenleme.

Kurallar:
- Önce ilgili kodu oku ve maddedeki "Sorun" iddiasını doğrula; iddia yanlışsa dur ve söyle.
- Kabul ölçütündeki testleri önce yaz (kırmızı), sonra uygula (yeşil).
- Signature / fit-store / JSON çıktısı geri uyumlu kalsın: eski dosya okunabilmeli, eski GUI yeni JSON'la çalışmalı.
- Model ya da benchmark davranışı değişiyorsa MODEL_VERSION / BENCH_VERSION etkisini açıkla.
- docs/AUTOTUNE.md'yi değişikliğe göre güncelle (D1 testi geçmeli).
- Bitince: `cargo test -p centurion-helpers --release` ve (GUI değiştiyse) gui testleri çalıştır, sonucu raporla.
```

Opus'a verilecek istatistik maddelerinde (A2, A3, B1, B2, B4, B6, A7, A8) ek olarak: "Değişikliği B1 simülatöründe önce/sonra
ölçütleriyle göster; regret veya kapsama kötüleşiyorsa birleştirme."
