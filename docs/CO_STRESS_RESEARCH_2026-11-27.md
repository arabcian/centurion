# CO stress research and the 30-minute verification (2026-11-27)

Why the per-core CO Search verification grew from 20 to 30 minutes, why it gained `--load hop`, and why
Centurion keeps its own kernels instead of porting Prime95 / y-cruncher code.

## What exposes an unstable per-core Curve Optimizer offset

From CoreCycler's README and default config (the de-facto per-core CO tool) and community reports:

1. **One core at a time, lightest load.** "the cores cannot boost as high if all of them are stress tested, and
   therefore you won't be able to detect instabilities that only occur at a higher clock speed." CoreCycler's
   example: all-core Prime95 passed 24 h at CO -30, one core needed -9 when tested alone. Its default is Prime95
   **SSE** ("lightest load ... highest boost clock"), 6 min per core, Huge FFTs.
2. **Heavy loads too.** The README: "test both scenarios, light load / SSE and heavy load with AVX/AVX2". Its
   y-cruncher test list mixes scalar integer (BKT), AVX2 float FFTs (SFTv4, FFTv4, VT3) and integer NTTs (N63, SNT).
3. **Load changes and idle.** `suspendPeriodically = 1` by default: "This can simulate load changes / switches to
   idle and back". Many systems that pass load tests still crash at idle or light desktop load (WHEA 18 "cache
   hierarchy" reports at idle / video playback, fixed by a less negative offset).
4. **Switching from one core to another.** CoreCycler's `CorePairs` order: "testing if there is any problem when
   switching from a specific core to another". In CoreCycler a switch happens once per core turn (minutes).
5. **Wrong results and machine checks both count.** CoreCycler treats a WHEA warning on the tested core as an error.
6. **Time.** "Basically as long as you can"; 6 min per core is a first pass.

Points 1-3 and 5 were already covered (`--load co` probes: FFT-SSE / FFT-AVX / NTT, ten boost/idle patterns,
wake bursts; `power`; MCA / kernel events). Point 4 was not: in the 20-minute verification a lone thread moved
between cores 2·n times in total, always in the same order, so only n of the n·(n−1) ordered core pairs ever
occurred.

## New: `percore --load hop` (crates/centurion-cpustress/src/hop.rs)

One verified thread jumps between the listed cores. Each visit pins to the next core, optionally waits (the
previous core idles first), then runs 2K SSE FFT squares (`wake_chunk`, every square checked) for the dwell.
Dwell 0.3-1 / 2-8 / 20-60 ms × wait 0 / 1-5 / 30-120 ms, all nine combinations in turn. The order covers every
ordered pair (i → i+g for every i and distance g) once per sweep, same-CCD and cross-CCD. Rationale: on a CCD the
cores share one rail, set for the core that asks for the most; with per-core offsets every core asks for a
different voltage, so a jump wakes a core straight into its single-core boost while the rail moves from the
previous core's request to its own. Errors name the CPU and the wait before the burst (`idle_ms`), as the CO
wake phase does. These are scheduling targets; real idle states and clocks depend on CPU, firmware and OS.

## The 30-minute layout

`percore co` pass → `power` 180 s → `allcore co` 2 cycles → **`hop` 300 s** (two or more undervolted cores) →
more single-core `co` cycles → classic SSE with transitions → `power` for the rest (**at least 130 s**, was 60 s,
so the time at the power limit does not shrink). Over 44 undervolted cores the fixed parts exceed 30 minutes.

## Calculation: old 20 min vs new 30 min

Rates measured with the release build on the 4-core test VM (KVM Xeon, not a Ryzen):
a 24 s `co` probe = 455 wakes of the worker (voluntary context switches minus the 247 of a steady 24 s run);
classic SSE with transitions = 5.4 wakes/s; `hop` = 24 core switches/s, 37 % busy; one wake square ≈ 45 µs.

| Undervolted cores | Layout | Single-core busy / core | Wakes / core | Lone-thread core switches | Ordered pairs covered | At power limit |
|---|---|---|---|---|---|---|
| 8 | old 20 min | 105 s | 1,869 | 16 | 8 / 56 | 240 s |
| 8 | new 30 min | 147 s (+40 %) | 3,245 (+74 %) | 7,216 | 56 / 56, 64 sweeps | 316 s |
| 16 | old 20 min | 48 s | 910 | 32 | 16 / 240 | 312 s |
| 16 | new 30 min | 73 s (+52 %) | 1,458 (+60 %) | 7,232 | 240 / 240, 15 sweeps | 324 s |

Every dimension grows by at least the 1.5× of the extra time except core switching, which goes from tens to
thousands, the one scenario the research named that the old layout did not cover.

## Port Prime95 / y-cruncher code, or design our own?

| | Port their kernels | Run mprime / y-cruncher binaries | Own kernels + schedule (this) |
|---|---|---|---|
| Source and licence | Prime95: source public but under the GIMPS EULA, not free software (cannot go into this GPL-3.0 crate); mostly hand-written assembly (gwnum). y-cruncher: binaries only. | Separate download, no redistribution in packages | GPL-3.0, in tree |
| Single-core lightest SSE at max boost | yes | yes | yes (fft-sse, wake squares) |
| Heavy AVX / FMA, power limit | yes | yes | yes (fft-avx, `power`) |
| Integer multiplier | yes (BKT, N63) | yes | yes (ntt, 64×64→128) |
| Idle ↔ load transitions | only with our scheduler | coarse (process suspend) | ten patterns + wake bursts every cycle |
| Core-to-core switching, every pair | only with our scheduler | once per core turn (minutes) | 24 switches/s, every ordered pair |
| Data beyond L3 (Huge FFT, VT3) | yes | yes | no (largest kernel 1 MiB) |
| Result checked | per FFT size / per test | per test, by parsing logs | every chunk (45 µs - few ms), CPU named |

Counting the seven stress dimensions above, the binaries cover 4 fully and 2 coarsely, our own design 6 of 7
fully; a code port is not possible under the licences, and even a legal one would add only the beyond-L3 data
path, since transitions and core switching come from the schedule, not the arithmetic. Own design is the more
effective choice. Open gap: a beyond-L3 FFT (CoreCycler's default Huge FFT), a candidate for a later kernel.

Not proven here: no Ryzen was available, so whether `hop` fails a marginal offset sooner than the other parts
is a hypothesis from the sources above, not a measurement. Passing remains no proof of long-term stability.

Sources: [CoreCycler README](https://github.com/sp00n/corecycler),
[CoreCycler default config](https://github.com/sp00n/CoreCycler/blob/master/configs/default.config.ini),
[CoreCycler thread, Overclock.net](https://www.overclock.net/threads/corecycler-tool-for-testing-single-core-stability-e-g-curve-optimizer-settings.1777398/),
[TechPowerUp Zen Garden](https://www.techpowerup.com/forums/threads/ryzen-owners-zen-garden.231658/post-4539177),
[WHEA 18 at idle, TechPowerUp](https://techpowerup.com/forums/threads/whea-logger-event-id-18.297115/page-2),
[y-cruncher FAQ](http://www.numberworld.org/y-cruncher/faq.html), [Prime95 (Wikipedia, licence)](https://en.wikipedia.org/wiki/Prime95).
