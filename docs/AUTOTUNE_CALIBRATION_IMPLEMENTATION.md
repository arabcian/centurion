# Autotune & calibration development-plan implementation

Source: `centurion_autotune_kalibrasyon_gelistirme_plani(1).md`, applied against `centurion-main(9).zip` (November 2026). The existing protective mechanisms (journal/restore, memory pressure brakes, temperature gates, IO write budget, session/reference metadata, ABBA validation) were preserved. **This is an experimental source patch, not a verified production release.**

## Implemented and staged

| Plan | What is in the code | Rollout |
|---|---|---|
| A01 | GUI **Fast** runs one main Autotune decision. **Explore…** reruns on demand with 3 trade-off variants; **Deep** additionally evaluates sensitivity variants. Existing mode-less IPC stays backward-compatible. | GUI defaults to Fast |
| A02 | Parallel explore creates isolated profile copies on demand, sharing the already fitted model data; one guarded template replaces per-job result mutexes. | On |
| A05 | Fast / Balanced / Deep selector, live Inputs / Model / Decision / Validation status (stderr stage notices), timing breakdown in Autotune report, pre-existing timeout retained. | On |
| K03 | Bounded quiet preflight, observes foreign CPU use and system IO/GPU/IRQ counters; at most three 300-ms probes with short waits; no knob is written by preflight. Strict rejection is separate. | `CENTURION_PREFLIGHT=1`; strict `CENTURION_PREFLIGHT_STRICT=1` |
| K04 | Allocate a 12-run-estimate minimum per phase; allocate remainder by factor count, cost and lack of same-context/same-power-source evidence. Original split remains available. | `CENTURION_ADAPTIVE_BUDGET=1` |
| K05 | Prior design evidence is filtered by phase/context/source; high historical dose coverage lowers the space-filling fraction, retaining a minimum. | On |
| K06 | Record measured run wall time and use bounded, 50%-blended historical run cost for acquisition estimates. | On |
| K07 | Alternating thermal run blocks (still using existing temperature gate) as a limited experiment. Does not yet include full time-block causal modeling. | `CENTURION_HEAT_BLOCK_AB=1` |
| K08 | Optional scoped cache handling for IO; avoids global `sync`/`drop_caches`; benchmark-local fadvise/direct reads already exist. Distinct IO benchmark version 8 prevents mixing with previous version 7. | `CENTURION_IO_SCOPED_CACHE=1` |
| M01 | Deterministic 384-row session/reference-stratified coarse hyperparameter fitting followed by full-data factorization; coverage gate falls back to original full hyperparameter search when failed. | `CENTURION_GP_COARSE=1` |
| M02 | Bound periodic retunes by new evidence count and maximum batch gap; skip alternate energy-source search if that source has no newest-session evidence and known hyperparameters. | On |
| M03 | Optional diversity-preserving fit-row selection representing sessions, references and individual knob doses; optional cap of 256, 512 or 768 instead of 1100. Baseline highest-weight truncation is kept by default. | `CENTURION_FIT_CORESET=1`, optional `CENTURION_FIT_ROW_CAP=512` |
| M08 | Precomputed session-id → index lookup in `PhaseSet::obs`. | On |
| S02 | Autotune stage timings; standard-library-only JSONL p50/p95 comparison script and deterministic script tests. Existing Rust `virtual_lab`/model simulator should still be run in CI/hardware. | On, no hardware results claimed |
| S03 | GP `MODEL_VERSION=4`, GP fit hash captures experimental parameters; scoped IO version 8 separated from old IO version 7. Existing data not deleted. | On |
| S05 | Opt-in experimental flags permit one change at a time and established fallback by unsetting variables. GUI Fast can switch to Deep. | Partial |

## Deferred from original plan

**Not implemented:** K01 two-tier measurements with missing-objective awareness; K02 confidence-driven early stopping without compromising tail sample minimums; K09 full metric-effect topology and periodic full control probes; K10 sequential-racing confirmation upgrades (the existing ABBA/verify remains); K11 battery sampling scheduler changes; M04 cross-objective immutable kernel sharing; M05 durable Cholesky factor cache; M06 incremental/periodic LOSO; M07 permanent thread pool; M09 rank-k Cholesky updates; M10 RAM-aware phase scheduling; A03 local sensitivity pruning; A04 prediction memoization; A06 high-risk GUI Confirm-before-Apply coupled to ABBA and rollback; A07 field-data GUI verification; A08 explicit action-level risk UI; S01 broad fault injection; S04 validated randomised blocking. Those need additional algorithmic work, careful hardware validation and, in some cases, user-consented workflows. No claimed speed-up or safety equivalence is inferred merely from this patch.

## Build and checks (run on Gentoo with the dependencies installed)

```sh
# Root of extracted repository
cargo test --workspace --locked
cargo test -p centurion-helpers --locked coarse_search_sampling_keeps_sessions_and_references_deterministically
cargo test -p centurion-helpers --locked repeated_calibration_reduces_initial_design_only_with_coverage
cargo test -p centurion-helpers --locked --release virtual_lab
python3 -m unittest discover -s tests -p 'test_compare_autotune_timings.py' -v
cmake -S gui -B /tmp/centurion-gui-build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/centurion-gui-build -j2
```

Keep the baseline build available. Start with the GUI's Fast mode and **all experimental environment flags unset**. Compare fast and deep outputs on an identical calibration snapshot and verify that the **main preset is unchanged**. Run the deterministic model tests, and then use controlled repetitions to check 90%-interval coverage, leave-one-session-out results and decision regret before enabling GP coreset/coarse search. For scoped IO, do not treat benchmark-7 and benchmark-8 values as interchangeable. Do not deploy a calibration experiment unattended until rollback, restore and IO-budget behavior have been verified on your hardware.

## Performance comparison

The `autotune_req` IPC response now carries `timings_ms` (`inputs`, `model`, `decision`, `total`) and `explore_mode`. Collect at least 10 full JSON responses per variant from the same calibration, goal, weights, power state and hardware condition (each response on one line). Then:

```sh
python3 scripts/compare_autotune_timings.py baseline.jsonl candidate.jsonl
```

The script reports stage p50/p95 and p50 speed-up. It **does not** prove quality preservation or statistical significance: use the Rust simulator, phase-wise LOSO and matched hardware comparisons for that.

## Known build verification boundary

The editing container did not contain `cargo`, `rustc`, or Qt6 Widgets development packages. The Rust and Qt code could not be compiled or run here. The supplied standalone Python comparison tests passed; this is **not** a substitute for `cargo test --workspace` and the Qt build. **Do not install these source changes as a privileged tuning helper before the build and safety tests pass.**
