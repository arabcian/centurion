#!/usr/bin/env python3
"""Native, hardware-independent tests of the product-report features that need no Qt:
scene apply summary, next-boot read-back check, Spectrum → 4-zone preview, V/F coverage, frametime direction."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]
code = r'''
#include "sceneapplysummary.h"
#include "nextbootcheck.h"
#include "spectrumzone4.h"
#include "vfcoverage.h"
#include "frametimepair.h"
#include <cassert>
#include <random>
int main() {
    using namespace sceneapplysummary;
    Summary s;
    s.record("Fan boost", "running"); s.record("Fan boost", "returned_ok", "turbo");
    s.record("CPU curve", "returned_unverified");
    s.record("GPU curve", "deferred_pending"); s.record("Panel refresh", "failed", "unsupported");
    s.record("", "failed");  // no step name: not a component
    assert(s.items().size() == 4 && s.items()[0].detail == "turbo");
    assert(s.count(Bucket::Applied) == 2 && s.count(Bucket::Failed) == 1 && s.count(Bucket::Deferred) == 1 && s.mixed());
    s.record("GPU curve", "deferred_failed", "refused");  // the deferred result replaces the pending one
    assert(s.count(Bucket::Deferred) == 0 && s.count(Bucket::Failed) == 2 && s.items()[2].detail == "refused");
    Summary ok; ok.record("Power profile", "returned_ok"); assert(!ok.mixed());
    assert(trigger("power", "on battery") == "power: on battery" && trigger("", "") == "unknown" && trigger("tray", "") == "tray");
    assert(bucket("deferred_superseded") == Bucket::Skipped && bucket("bogus") == Bucket::Unknown);

    using nextbootcheck::State;
    const std::map<std::string, int64_t> want{{"boost_mhz", 200}, {"curve_optimizer", -20}, {"pbo_scalar", 10}};
    auto rows = nextbootcheck::check(want, "boot-a", "boot-a", {{"boost_mhz", 0}});
    for (const auto &r : rows) assert(r.state == State::Pending);
    assert(!nextbootcheck::settled(rows));
    rows = nextbootcheck::check(want, "boot-a", "boot-b", {{"boost_mhz", 200}, {"curve_optimizer", -15}});
    assert(rows[0].state == State::Matched && rows[1].state == State::Limited && *rows[1].readBack == -15 && rows[2].state == State::Unreadable);
    assert(nextbootcheck::settled(rows));
    assert(nextbootcheck::check(want, "", "boot-b", {})[0].state == State::Pending);  // unknown write boot: never judged

    using namespace spectrumzone4;
    assert(zoneOf(0) == 0 && zoneOf(0.24) == 0 && zoneOf(0.25) == 1 && zoneOf(0.99) == 3 && zoneOf(1) == 3 && zoneOf(-1) == 0);
    const auto z = convert({{0.1, Rgb{255, 0, 0}}, {0.2, Rgb{0, 0, 255}}, {0.6, Rgb{0, 255, 0}}, {0.9, std::nullopt}}, 2);
    assert(z.zones[0] && *z.zones[0] == (Rgb{128, 0, 128}) && !z.zones[1] && *z.zones[2] == (Rgb{0, 255, 0}) && !z.zones[3]);
    assert(z.keysUsed == 3 && z.keysWithoutColor == 1 && z.effectsDropped == 2);

    const std::vector<double> curve{300, 600, 900, 1200, 1500};
    assert(vfcoverage::pointFor(curve, 100) == 0 && vfcoverage::pointFor(curve, 600) == 1 && vfcoverage::pointFor(curve, 601) == 2);
    assert(vfcoverage::pointFor(curve, 9999) == 4 && vfcoverage::pointFor({}, 5) == -1);
    vfcoverage::Coverage c;
    for (double mhz : {1400.0, 1450.0, 1500.0, 0.0, 700.0}) c = vfcoverage::add(c, curve, mhz);
    assert(c.samples == 4 && c.counts[4] == 3 && c.counts[2] == 1 && c.lowest() == 2 && c.highest() == 4 && c.observedPoints() == 2);
    c = vfcoverage::add(c, {100, 200}, 150);  // a new curve shape restarts the recording
    assert(c.samples == 1 && c.counts.size() == 2);
    std::mt19937 gen(25);
    for (int i = 0; i < 20000; ++i) {
        const double mhz = gen() % 3000;
        const int p = vfcoverage::pointFor(curve, mhz);
        assert(p >= 0 && p < 5 && (curve[p] >= mhz || p == 4) && (p == 0 || curve[p - 1] < mhz));
    }

    frametimepair::Comparison cmp{};
    cmp.delta = {-5, -4, 2, 6, 5};   assert(frametimepair::direction(cmp) == frametimepair::Direction::MeanBetterTailWorse);
    cmp.delta = {4, 3, -1, -8, -4};  assert(frametimepair::direction(cmp) == frametimepair::Direction::MeanWorseTailBetter);
    cmp.delta = {-5, -4, -2, -6, 5}; assert(frametimepair::direction(cmp) == frametimepair::Direction::Consistent);
    cmp.delta = {-0.5, 0, 0, 3, 0};  assert(frametimepair::direction(cmp) == frametimepair::Direction::Consistent);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'test.cpp'; src.write_text(code)
    for flags in [[], ['-O2'], ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']]:
        exe = Path(tmp) / 'test'
        subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', *flags, '-I', str(root / 'src'), str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        print('PASS:', ' '.join(flags) or 'normal')
