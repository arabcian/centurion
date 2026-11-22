# Knob envanteri (kalibrasyon ve autotune)

Kaynak koddan üretildi (2026-11-01; 2026-11-23: 13 EEVDF feature biti, /sys dökümünden 37 knob ve MC/PKG sched domain satırları eklendi, elle işlendi): `tune::TUNABLES`, `calib::EXCLUDED`, `calib::phases_for`, `calib::dev_key`; autotune sütunu `autotune.rs`'in test dışı kodunda knobun adının geçip geçmediğine göre.

- **Kalibrasyon**: evet = tasarım oturumunda ölçülür (makineye göre atlanma koşulu parantez içinde).
- Her knob, dosyası bu makinede/çekirdekte yoksa atlanır (ör. `cpu.epp_boost` yalnız yamalı çekirdekte).
- **Faz[test]**: knobun kendi test listesi. Idle/load tasarım koşuları her koşuda CPU paketini (oyun döngüsü, kare süreleri, single/multi, işler) ve bellek probunu (fault, TLB, RSS) birlikte çalıştırır; köşeli parantez tek-knob (`--oat`) modunu ve süre tahminini belirler.
- **Autotune**: *ölçüm (joint)* = ortak modelden seçilir; *kural + ölçüm* = kural ön bilgidir, ölçüm güvenle aksini gösterirse ezer; *yalnız kural* = ölçülmeden kuralla; — = hiç ayarlanmaz (yalnız elle).
- Birleşik knoblar (`thp`, `vm.dirty`, `launch.slice_us`) TUNABLES dışında tanımlıdır; `thp` 5 mod, idle+load; `vm.dirty` IO fazı.

Özet: 254 knob · kalibre + autotune 123 · yalnız autotune kuralı 52 · hiçbiri 79

## CPU (38)

| Knob | Kalibrasyon | Faz[test] | Autotune | Ölçülmeme nedeni |
|---|---|---|---|---|
| `cpu.pstate_status` | hayır | — | yalnız kural | driver switch |
| `cpu.intel_pstate_status` | hayır | — | yalnız kural | driver switch |
| `cpu.dynamic_epp` | hayır | — | yalnız kural | hands EPP to the kernel: rewrites every EPP row (structural) |
| `cpu.governor` | evet | idle[cpu+power] load[cpu] | kural + ölçüm |  |
| `cpu.epp` | evet | idle[cpu+power] load[cpu] | kural + ölçüm |  |
| `cpu.epp_boost` | evet [Amd] | idle[cpu+power] load[cpu] | kural + ölçüm |  |
| `cpu.boost` | evet | idle[cpu+power] load[cpu] | kural + ölçüm |  |
| `cpu.min_freq` | hayır | — | yalnız kural | floor, power only |
| `cpu.floor_freq` | hayır | — | yalnız kural | floor, power only |
| `cpu.governor_ccd0` | hayır | — | yalnız kural | CCD role (structural) |
| `cpu.governor_ccd1` | hayır | — | yalnız kural | CCD role (structural) |
| `cpu.epp_ccd0` | hayır | — | yalnız kural | CCD role (structural) |
| `cpu.epp_ccd1` | hayır | — | yalnız kural | CCD role (structural) |
| `cpu.boost_ccd0` | hayır | — | yalnız kural | CCD role (structural) |
| `cpu.boost_ccd1` | hayır | — | yalnız kural | CCD role (structural) |
| `cpu.max_freq_ccd0` | hayır | — | yalnız kural | frequency cap: a role set by the goal, not a dose |
| `cpu.max_freq_ccd1` | hayır | — | yalnız kural | frequency cap: a role set by the goal, not a dose |
| `cpu.hwp_dynamic_boost` | evet [Intel] | idle[cpu+power] load[cpu] | kural + ölçüm |  |
| `cpu.max_perf_pct` | hayır | — | — | frequency cap: a role set by the goal, not a dose |
| `cpu.min_perf_pct` | hayır | — | — | frequency floor, power only |
| `cpu.epb` | evet [Intel] | idle[cpu+power] load[cpu] | kural + ölçüm |  |
| `cpu.epp_pcore` | hayır | — | yalnız kural | core-type role (structural) |
| `cpu.epp_ecore` | hayır | — | yalnız kural | core-type role (structural) |
| `cpu.max_freq_pcore` | hayır | — | — | core-type role (structural) |
| `cpu.max_freq_ecore` | hayır | — | — | core-type role (structural) |
| `cpu.uncore_max_khz` | hayır | — | yalnız kural | Intel uncore limit |
| `cpu.uncore_min_khz` | hayır | — | yalnız kural | Intel uncore limit |
| `cpu.rapl_pl1` | hayır | — | — | firmware power limit |
| `cpu.rapl_pl2` | hayır | — | — | firmware power limit |
| `cpu.tcc_offset` | hayır | — | — | thermal limit |
| `cpu.x3d_mode` | hayır | — | yalnız kural | V-Cache role (structural) |
| `cpu.idle_governor` | evet | idle[cpu+power] load[cpu] | kural + ölçüm |  |
| `cpu.cstate_max` | evet | idle[cpu+power] load[cpu] | kural + ölçüm |  |
| `cpu.wake_latency_us` | evet | idle[cpu+power] load[cpu] | kural + ölçüm |  |
| `cpu.schedutil_rate_limit_us` | evet | idle[cpu+power] load[cpu] | kural + ölçüm |  |
| `clock.source` | hayır | — | yalnız kural | repair only (structural) |
| `cpu.smt` | hayır | — | yalnız kural | takes CPUs offline |
| `cpu.ccd_park` | hayır | — | yalnız kural | takes CPUs offline |

## Memory (58)

| Knob | Kalibrasyon | Faz[test] | Autotune | Ölçülmeme nedeni |
|---|---|---|---|---|
| `thp.enabled` | hayır | — | yalnız kural (birleşik knob üzerinden) | tested as the thp group |
| `thp.shmem_enabled` | evet | idle[mem] load[mem] | ölçüm (joint) |  |
| `thp.defrag` | evet | load[mem] | kural + ölçüm |  |
| `thp.khugepaged_defrag` | hayır | — | — | khugepaged: minutes |
| `thp.mthp_16k` | hayır | — | yalnız kural (birleşik knob üzerinden) | thp group |
| `thp.mthp_32k` | hayır | — | yalnız kural (birleşik knob üzerinden) | thp group |
| `thp.mthp_64k` | hayır | — | yalnız kural (birleşik knob üzerinden) | thp group |
| `thp.mthp_128k` | evet | idle[mem] load[mem] | kural + ölçüm |  |
| `thp.mthp_256k` | evet | idle[mem] load[mem] | kural + ölçüm |  |
| `thp.mthp_512k` | evet | idle[mem] load[mem] | kural + ölçüm |  |
| `thp.mthp_1m` | evet | idle[mem] load[mem] | kural + ölçüm |  |
| `thp.khp_max_ptes_none` | hayır | — | yalnız kural | khugepaged: minutes |
| `thp.khp_pages_to_scan` | hayır | — | yalnız kural | khugepaged: minutes |
| `thp.khp_scan_sleep_ms` | hayır | — | yalnız kural | khugepaged: minutes |
| `thp.khp_max_ptes_swap` | hayır | — | yalnız kural | khugepaged: minutes |
| `thp.khp_alloc_sleep_ms` | hayır | — | — | khugepaged: minutes |
| `mm.lru_gen` | evet | idle[mem+cpu] load[mem+cpu] | kural + ölçüm |  |
| `mm.lru_gen_min_ttl` | evet | load[mem] | kural + ölçüm |  |
| `mm.ksm_run` | hayır | — | yalnız kural | no effect without mergeable memory |
| `mm.ksm_advisor` | hayır | — | — | no effect without mergeable memory |
| `thp.khp_max_ptes_shared` | hayır | — | — | khugepaged: minutes |
| `thp.use_zero_page` | evet | load[mem] | ölçüm (joint) |  |
| `vm.compact_unevictable_allowed` | evet | load[mem] | ölçüm (joint) |  |
| `vm.dirtytime_expire_seconds` | hayır | — | — | 12 h timestamp age: longer than a run, unmeasurable |
| `vm.max_map_count` | hayır | — | yalnız kural | a limit, not a cost |
| `vm.swappiness` | evet (swap yoksa atlanır) | load[mem] | kural + ölçüm |  |
| `vm.compaction_proactiveness` | evet | load[mem] | kural + ölçüm |  |
| `vm.watermark_boost_factor` | evet | load[mem] | kural + ölçüm |  |
| `vm.watermark_scale_factor` | evet | load[mem] | kural + ölçüm |  |
| `vm.min_free_kbytes` | evet | load[mem] | kural + ölçüm |  |
| `vm.zone_reclaim_mode` | evet (tek NUMA düğümünde atlanır) | load[mem] | kural + ölçüm |  |
| `vm.page_lock_unfairness` | evet | load[mem] | ölçüm (joint) |  |
| `vm.stat_interval` | evet | idle[cpu+power] | ölçüm (joint) |  |
| `vm.page_cluster` | evet (swap yoksa atlanır) | load[mem] | kural + ölçüm |  |
| `vm.dirty_background_ratio` | hayır | — | — (birleşik knob üzerinden) | vm.dirty |
| `vm.dirty_ratio` | hayır | — | — (birleşik knob üzerinden) | tested as vm.dirty |
| `vm.dirty_background_bytes` | hayır | — | yalnız kural (birleşik knob üzerinden) | vm.dirty |
| `vm.dirty_bytes` | hayır | — | yalnız kural (birleşik knob üzerinden) | vm.dirty |
| `vm.dirty_writeback_centisecs` | hayır | — | — | periodic flush every 5-15 s: longer than a run, unmeasurable |
| `vm.dirty_expire_centisecs` | hayır | — | — | 30-60 s data age: longer than a run, unmeasurable |
| `vm.vfs_cache_pressure` | evet | load[mem] | kural + ölçüm |  |
| `zswap.enabled` | evet (zram varsa / swap yoksa atlanır) | load[mem] | kural + ölçüm |  |
| `zswap.compressor` | evet (zram varsa / swap yoksa atlanır) | load[mem] | kural + ölçüm |  |
| `zswap.max_pool_percent` | evet (zram varsa / swap yoksa atlanır) | load[mem] | ölçüm (joint) |  |
| `zswap.shrinker_enabled` | evet (zram varsa / swap yoksa atlanır) | load[mem] | kural + ölçüm |  |
| `vm.defrag_mode` | evet | load[mem] | ölçüm (joint) |  |
| `thp.shrink_underused` | evet | load[mem] | ölçüm (joint) |  |
| `zswap.accept_threshold` | evet (zram varsa / swap yoksa atlanır) | load[mem] | ölçüm (joint) |  |
| `vm.percpu_pagelist_high_fraction` | evet | idle[mem] load[mem] | kural + ölçüm |  |
| `vm.extfrag_threshold` | evet | load[mem] | ölçüm (joint) |  |
| `mm.fault_around_bytes` | evet | load[mem] | ölçüm (joint) |  |
| `mm.tlb_single_page_flush_ceiling` | evet | load[mem] | ölçüm (joint) |  |
| `vm.laptop_mode` | evet | load[mem] | ölçüm (joint) |  |
| `vm.oom_kill_allocating_task` | hayır | — | — | behaviour, not performance |
| `mm.ksm_use_zero_pages` | hayır | — | — | no effect without mergeable memory |
| `mm.ksm_smart_scan` | hayır | — | — | no effect without mergeable memory |
| `kernel.numa_promote_rate_limit` | hayır | — | — | memory tiering only |
| `mm.swap_vma_readahead` | evet | load[mem] | ölçüm (joint) |  |

## Scheduler (76)

| Knob | Kalibrasyon | Faz[test] | Autotune | Ölçülmeme nedeni |
|---|---|---|---|---|
| `sched.ext` | hayır | — | yalnız kural | needs a userspace scheduler |
| `kernel.split_lock_mitigate` | evet | idle[cpu] load[mem+cpu] | kural + ölçüm |  |
| `kernel.watchdog` | hayır | — | yalnız kural | never disabled |
| `kernel.nmi_watchdog` | evet | idle[cpu+power] | kural + ölçüm |  |
| `kernel.numa_balancing` | evet (tek NUMA düğümünde atlanır) | idle[cpu] load[mem+cpu] | kural + ölçüm |  |
| `kernel.timer_migration` | evet | idle[cpu] load[mem+cpu] | kural + ölçüm |  |
| `kernel.sched_autogroup` | evet | idle[cpu] load[mem+cpu] | kural + ölçüm |  |
| `kernel.cfs_bandwidth_slice_us` | hayır | — | — | only with CPU quotas |
| `kernel.sched_util_clamp_min_rt_default` | hayır | — | yalnız kural | RT tasks only |
| `kernel.sched_energy_aware` | evet | idle[cpu] load[mem+cpu] | kural + ölçüm |  |
| `kernel.sched_schedstats` | hayır | — | yalnız kural | debug counters |
| `sched.itmt` | hayır | — | yalnız kural | preferred cores go to the load phase's ballast, not to the probes (structural) |
| `sched.preempt` | evet | idle[cpu] load[mem+cpu] | kural + ölçüm |  |
| `sched.base_slice_ns` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.min_base_slice_ns` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.migration_cost_ns` | evet | idle[cpu] load[mem+cpu] | kural + ölçüm |  |
| `sched.nr_migrate` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.cgroup_mode` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.llc_balance` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.llc_aggr_tolerance` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.llc_imb_pct` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.llc_overload_pct` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.feat_next_buddy` | evet | idle[cpu] load[mem+cpu] | kural + ölçüm |  |
| `sched.feat_run_to_parity` | evet | idle[cpu] load[mem+cpu] | kural + ölçüm |  |
| `kernel.sched_bore` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `kernel.sched_burst_inherit_type` | evet (BORE kapalıysa atlanır) | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `kernel.sched_burst_smoothness` | evet (BORE kapalıysa atlanır) | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `kernel.sched_burst_penalty_offset` | evet (BORE kapalıysa atlanır) | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `kernel.sched_burst_penalty_scale` | evet (BORE kapalıysa atlanır) | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `kernel.sched_burst_cache_lifetime` | evet (BORE kapalıysa atlanır) | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `kernel.yield_type` | hayır | — | — | Project C kernels only: manual |
| `kernel.infinity_smt_divisor` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.feat_preempt_short` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.feat_delay_dequeue` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.feat_hrtick` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.feat_sis_util` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.feat_ttwu_queue` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.feat_cache_hot_buddy` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.feat_place_lag` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.feat_place_deadline_initial` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.feat_place_rel_deadline` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.feat_wakeup_preemption` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.feat_pick_buddy` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.feat_delay_zero` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.feat_wa_idle` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.feat_wa_weight` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.feat_wa_bias` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.feat_util_est` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.feat_attach_age_load` | hayır | — | — | load-tracking accuracy, not a lever |
| `sched.feat_nontask_capacity` | hayır | — | — | capacity accounting, not a lever |
| `sched.feat_lb_min` | hayır | — | — | filter for nearly idle tasks: below what a run resolves |
| `sched.feat_ni_random` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.feat_ni_rate` | hayır | — | — | undocumented companion of NI_RANDOM |
| `sched.feat_hrtick_dl` | hayır | — | — | SCHED_DEADLINE entities only |
| `sched.feat_paranoid_avg` | hayır | — | — | bookkeeping check, not a policy |
| `wq.affinity_scope` | evet | idle[cpu] load[mem+cpu] | kural + ölçüm |  |
| `wq.cpumask` | hayır | — | yalnız kural | topology (structural) |
| `irq.affinity` | hayır | — | yalnız kural | topology (structural) |
| `sched.llc_epoch_period` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.llc_epoch_affinity_timeout` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.numa_scan_size_mb` | hayır | — | — | single NUMA node: no effect |
| `sched.numa_scan_period_min_ms` | hayır | — | — | single NUMA node: no effect |
| `sched.numa_scan_period_max_ms` | hayır | — | — | single NUMA node: no effect |
| `sched.numa_scan_delay_ms` | hayır | — | — | single NUMA node: no effect |
| `sched.numa_hot_threshold_ms` | hayır | — | — | memory tiering only |
| `sched.rseq_slice_ext_nsec` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `kernel.sched_rt_runtime_us` | hayır | — | — | halving it throttles real-time audio and input threads |
| `kernel.sched_rr_timeslice_ms` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.mc_imbalance_pct` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.mc_min_interval` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.mc_max_interval` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.mc_busy_factor` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.pkg_imbalance_pct` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.pkg_min_interval` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.pkg_max_interval` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |
| `sched.pkg_busy_factor` | evet | idle[cpu] load[mem+cpu] | ölçüm (joint) |  |

## Storage (20)

| Knob | Kalibrasyon | Faz[test] | Autotune | Ölçülmeme nedeni |
|---|---|---|---|---|
| `blk.scheduler` | evet | io[io] | kural + ölçüm |  |
| `blk.wbt_lat_usec` | evet | io[io] | kural + ölçüm |  |
| `blk.read_ahead_kb` | evet | io[io] | kural + ölçüm |  |
| `blk.rq_affinity` | evet | io[io] | kural + ölçüm |  |
| `blk.nomerges` | evet | io[io] | kural + ölçüm |  |
| `blk.iostats` | evet | io[io] | kural + ölçüm |  |
| `blk.add_random` | evet | io[io] | kural + ölçüm |  |
| `blk.nr_requests` | evet | io[io] | kural + ölçüm |  |
| `blk.max_sectors_kb` | evet | io[io] | ölçüm (joint) |  |
| `blk.kyber_read_lat_ns` | hayır | — | — | elevator tunable: manual only |
| `blk.kyber_write_lat_ns` | hayır | — | — | elevator tunable: manual only |
| `blk.dl_read_expire_ms` | hayır | — | — | elevator tunable: manual only |
| `blk.dl_write_expire_ms` | hayır | — | — | elevator tunable: manual only |
| `blk.dl_fifo_batch` | hayır | — | — | elevator tunable: manual only |
| `blk.dl_writes_starved` | hayır | — | — | elevator tunable: manual only |
| `blk.bfq_low_latency` | hayır | — | — | elevator tunable: manual only |
| `blk.bfq_slice_idle_ms` | hayır | — | — | elevator tunable: manual only |
| `btrfs.discard_iops_limit` | hayır | — | — | background TRIM pacing: the storage suite never waits for it |
| `btrfs.discard_kbps_limit` | hayır | — | — | background TRIM pacing: the storage suite never waits for it |
| `btrfs.discard_max_size` | hayır | — | — | background TRIM pacing: the storage suite never waits for it |

## Network (32)

| Knob | Kalibrasyon | Faz[test] | Autotune | Ölçülmeme nedeni |
|---|---|---|---|---|
| `net.tcp_congestion` | hayır | — | yalnız kural | grup kalibrasyon kapsamı dışında |
| `net.default_qdisc` | hayır | — | yalnız kural | grup kalibrasyon kapsamı dışında |
| `net.wifi_power_save` | hayır | — | yalnız kural | grup kalibrasyon kapsamı dışında |
| `net.tcp_slow_start_after_idle` | hayır | — | — | grup kalibrasyon kapsamı dışında |
| `net.tcp_mtu_probing` | hayır | — | — | grup kalibrasyon kapsamı dışında |
| `net.netdev_max_backlog` | hayır | — | — | grup kalibrasyon kapsamı dışında |
| `net.tcp_notsent_lowat` | hayır | — | — | grup kalibrasyon kapsamı dışında |
| `net.tcp_fastopen` | hayır | — | — | grup kalibrasyon kapsamı dışında |
| `net.gro_flush_timeout` | hayır | — | — | grup kalibrasyon kapsamı dışında |
| `net.napi_defer_hard_irqs` | hayır | — | — | grup kalibrasyon kapsamı dışında |
| `net.busy_poll` | hayır | — | — | grup kalibrasyon kapsamı dışında |
| `net.busy_read` | hayır | — | — | grup kalibrasyon kapsamı dışında |
| `net.napi_threaded` | hayır | — | — | grup kalibrasyon kapsamı dışında |
| `net.wifi_aql_enable` | hayır | — | — | no calibration phase for this group |
| `net.wifi_aql_threshold` | hayır | — | — | no calibration phase for this group |
| `net.wifi_napi_threaded` | hayır | — | — | no calibration phase for this group |
| `net.mt76_runtime_pm` | hayır | — | — | no calibration phase for this group |
| `net.mt76_idle_timeout` | hayır | — | — | no calibration phase for this group |
| `net.mt76_deep_sleep` | hayır | — | — | no calibration phase for this group |
| `bt.conn_min_interval` | hayır | — | — | no calibration phase for this group |
| `bt.conn_max_interval` | hayır | — | — | no calibration phase for this group |
| `net.netdev_budget` | hayır | — | — | no calibration phase for this group |
| `net.netdev_budget_usecs` | hayır | — | — | no calibration phase for this group |
| `net.dev_weight` | hayır | — | — | no calibration phase for this group |
| `net.gro_normal_batch` | hayır | — | — | no calibration phase for this group |
| `net.tcp_limit_output_bytes` | hayır | — | — | no calibration phase for this group |
| `net.tcp_autocorking` | hayır | — | — | no calibration phase for this group |
| `net.tcp_pacing_ss_ratio` | hayır | — | — | no calibration phase for this group |
| `net.tcp_pacing_ca_ratio` | hayır | — | — | no calibration phase for this group |
| `net.tcp_ecn` | hayır | — | — | no calibration phase for this group |
| `net.rmem_max` | hayır | — | — | no calibration phase for this group |
| `net.wmem_max` | hayır | — | — | no calibration phase for this group |

## Devices (25)

| Knob | Kalibrasyon | Faz[test] | Autotune | Ölçülmeme nedeni |
|---|---|---|---|---|
| `pci.aspm` | hayır | — | yalnız kural | grup kalibrasyon kapsamı dışında |
| `pci.aspm_links` | hayır | — | yalnız kural | grup kalibrasyon kapsamı dışında |
| `pm.mem_sleep` | hayır | — | yalnız kural | grup kalibrasyon kapsamı dışında |
| `pci.latency_timer` | hayır | — | — | grup kalibrasyon kapsamı dışında |
| `snd.hda_power_save` | evet (yalnız pilde) | dev[power] | kural + ölçüm |  |
| `snd.hda_power_save_controller` | evet (yalnız pilde) | dev[power] | kural + ölçüm |  |
| `usb.autosuspend` | hayır | — | yalnız kural | grup kalibrasyon kapsamı dışında |
| `usb.autosuspend_ms` | evet (yalnız pilde) | dev[power] | ölçüm (joint) |  |
| `net.wol` | hayır | — | yalnız kural | grup kalibrasyon kapsamı dışında |
| `net.eee` | evet (yalnız pilde) | dev[power] | ölçüm (joint) |  |
| `rf.bluetooth` | hayır | — | — | grup kalibrasyon kapsamı dışında |
| `rf.wlan` | hayır | — | — | grup kalibrasyon kapsamı dışında |
| `rf.wwan` | hayır | — | — | grup kalibrasyon kapsamı dışında |
| `gpu.amdgpu_abm` | evet (yalnız pilde) | dev[power] | kural + ölçüm |  |
| `disk.apm_0` | evet (yalnız pilde) | dev[power] | kural + ölçüm |  |
| `disk.apm_1` | evet (yalnız pilde) | dev[power] | kural + ölçüm |  |
| `disk.apm_2` | evet (yalnız pilde) | dev[power] | kural + ölçüm |  |
| `disk.apm_3` | evet (yalnız pilde) | dev[power] | kural + ölçüm |  |
| `pm.ahci_runtime_timeout` | evet (yalnız pilde) | dev[power] | kural + ölçüm |  |
| `pm.ahci_disk_runtime` | evet (yalnız pilde) | dev[power] | kural + ölçüm |  |
| `pm.ahci_port_runtime` | evet (yalnız pilde) | dev[power] | ölçüm (joint) |  |
| `gpu.amdgpu_dpm` | hayır | — | yalnız kural | grup kalibrasyon kapsamı dışında |
| `gpu.intel_max_mhz` | hayır | — | — | grup kalibrasyon kapsamı dışında |
| `gpu.intel_min_mhz` | hayır | — | — | grup kalibrasyon kapsamı dışında |
| `gpu.intel_slpc_profile` | hayır | — | yalnız kural | grup kalibrasyon kapsamı dışında |

## Power (4)

| Knob | Kalibrasyon | Faz[test] | Autotune | Ölçülmeme nedeni |
|---|---|---|---|---|
| `pm.pci_runtime` | evet (yalnız pilde) | dev[power] | kural + ölçüm |  |
| `pm.usb_runtime` | evet (yalnız pilde) | dev[power] | kural + ölçüm |  |
| `pm.sata_alpm` | evet (yalnız pilde) | dev[power] | kural + ölçüm |  |
| `pm.nvme_latency_us` | evet (yalnız pilde) | dev[power] | kural + ölçüm |  |

## Stability (1)

| Knob | Kalibrasyon | Faz[test] | Autotune | Ölçülmeme nedeni |
|---|---|---|---|---|
| `mce.check_interval` | hayır | — | — | grup kalibrasyon kapsamı dışında |

