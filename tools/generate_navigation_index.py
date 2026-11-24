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
out += ['};','} // namespace tuningnavigation','']
path=ROOT/'gui/src/tuningnavigation.h'
content='\n'.join(out)
if '--check' in sys.argv:
    if path.read_text()!=content: raise SystemExit('navigation index stale; regenerate')
else: path.write_text(content)
