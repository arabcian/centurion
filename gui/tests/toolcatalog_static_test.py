#!/usr/bin/env python3
"""Every System tool has an entry in the tool catalog (area, search words, badges), and the Health tab
keeps one navigation list. Run: python3 gui/tests/toolcatalog_static_test.py"""
from pathlib import Path
import re

SRC = Path(__file__).resolve().parents[1] / "src"
systools = (SRC / "systools.cpp").read_text()
catalog = (SRC / "toolcatalog.cpp").read_text()
health = (SRC / "healthtab.cpp").read_text()
mainwindow = (SRC / "mainwindow.cpp").read_text()

specs = systools[systools.index("const Spec specs[]"):systools.index("for (const Spec &s : specs)")]
keys = re.findall(r'^\s*\{(?:QT_TRANSLATE_NOOP\("systools", )?"([^"]+)"', specs, re.M)
keys += re.findall(r'setProperty\("toolKey", QStringLiteral\("([^"]+)"\)\)', systools)
keys += re.findall(r'addToolPage\([^;]*QStringLiteral\("([^"]+)"\)\)', mainwindow)
assert len(keys) >= 30, keys

checks = {f"catalog entry: {k}": re.search(r"\{\"" + re.escape(k) + r"\",\s*\{A::", catalog) is not None for k in set(keys) if k != "SMART"}
checks["no second tool tree"] = "toolNavigation_" not in health
checks["catalog search keeps its name"] = "healthToolSearch" in health
checks["tools remember pins and recents"] = 'health/pinned_tools' in health and 'health/recent_tools' in health
checks["last tool is not reopened at start-up"] = 'health/tool' not in health

for name, ok in sorted(checks.items()):
    print(("PASS" if ok else "FAIL") + " - " + name)
assert all(checks.values()), "toolcatalog static checks failed"
print(f"PASS: {len(checks)} static checks")
