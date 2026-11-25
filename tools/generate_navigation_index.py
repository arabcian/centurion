#!/usr/bin/env python3
"""Regenerate the read-only search metadata from the real Rust tunables."""
from pathlib import Path
import re, json, sys
ROOT = Path(__file__).resolve().parents[1]
src = (ROOT / 'crates/centurion-helpers/src/tune.rs').read_text()
pat = r'"((?:\\.|[^"\\])*)"'
records = re.findall(r'\bt\(\s*'+pat+r'\s*,\s*'+pat+r'\s*,\s*'+pat+r'\s*,\s*'+pat, src)
homes = {}
for home in ('network','storage','kernel'):
    m = re.search(r'pub const HOME_'+home.upper()+r':[^=]+=\s*&\[(.*?)\];',src,re.S)
    for k in re.findall(pat,m.group(1)): homes[k]=home
assert len(records)>100 and len({r[0] for r in records}) == len(records)
out = ['// Generated from tune.rs by tools/generate_navigation_index.py. Do not edit.', '#pragma once', 'namespace tuningnavigation {', 'struct Entry { const char *key, *group, *label, *home; };', 'inline constexpr Entry entries[] = {']
for k,g,l,h in records: out.append('    {'+', '.join(json.dumps(x,ensure_ascii=False) for x in (k,g,l,homes.get(k,'')))+'},')
out += ['};']
# Which knob writes which kernel file: lets the read-only Kernel Parameters page name the knob behind a row.
# '*' stands for one path component (cpuN, policyN, domainN); per-device targets (disks, NICs, IRQs) are left out.
body = src[src.index('pub const TUNABLES'):src.index('pub const RETIRED_KEYS')]
paths = []
for part in re.split(r'(?=\bt\(\s*")', body)[1:]:
    key = re.match(r't\(\s*"([^"]+)"', part).group(1)
    found = []
    for m in re.finditer(r'Target::File\("([^"]+)"\)', part): found.append(m.group(1))
    for m in re.finditer(r'Target::AnyFile\(&\[([^\]]*)\]\)', part): found += re.findall(pat, m.group(1))
    for m in re.finditer(r'Target::DirtyBytes\s*\{\s*bytes:\s*"([^"]+)",\s*ratio:\s*"([^"]+)"', part): found.append(m.group(1))
    if re.search(r'Target::SchedFeature\(', part): found.append('/sys/kernel/debug/sched/features')
    for m in re.finditer(r'Target::(?:PerPolicy|PerCcdPolicy|PerCoreType)\("([^"]+)"', part): found.append('/sys/devices/system/cpu/cpufreq/policy*/' + m.group(1))
    for m in re.finditer(r'Target::(?:PerCpu|PerCcdCpu)\("([^"]+)"', part): found.append('/sys/devices/system/cpu/cpu*/' + m.group(1))
    for m in re.finditer(r'Target::SchedDomain\("[A-Z]+",\s*"([^"]+)"\)', part): found.append('/sys/kernel/debug/sched/domains/cpu*/domain*/' + m.group(1))
    if re.search(r'Target::MinFreq\b', part): found.append('/sys/devices/system/cpu/cpufreq/policy*/scaling_min_freq')
    if re.search(r'Target::FloorFreq\b', part): found.append('/sys/devices/system/cpu/cpufreq/policy*/amd_pstate_floor_freq')
    if re.search(r'Target::Boost\b', part): found += ['/sys/devices/system/cpu/cpufreq/policy*/boost', '/sys/devices/system/cpu/cpufreq/boost', '/sys/devices/system/cpu/intel_pstate/no_turbo']
    if re.search(r'Target::CState\b', part): found.append('/sys/devices/system/cpu/cpu*/cpuidle/state*/disable')
    for f in found:
        if (f, key) not in paths: paths.append((f, key))
assert len(paths) > 150
out += ['struct PathEntry { const char *path, *key; };', 'inline constexpr PathEntry paths[] = {']
for f,k in paths: out.append('    {'+json.dumps(f)+', '+json.dumps(k)+'},')
out += ['};','} // namespace tuningnavigation','']
path=ROOT/'gui/src/tuningnavigation.h'
content='\n'.join(out)
if '--check' in sys.argv:
    if path.read_text()!=content: raise SystemExit('navigation index stale; regenerate')
else: path.write_text(content)
