#!/usr/bin/env python3
"""Title Case captions and stable System Tools identifiers (source-only checks).

Run with: python3 gui/tests/title_case_static_test.py
This is not a substitute for Qt compilation or a GUI screenshot review.
"""
from pathlib import Path
import re
from lxml import etree

root = Path(__file__).resolve().parents[1]
src = root / 'src'
h = (src / 'healthtab.cpp').read_text()
tc = (src / 'toolcatalog.cpp').read_text()
st = (src / 'systools.cpp').read_text()
mw = (src / 'mainwindow.cpp').read_text()
xml = etree.parse(str(root / 'i18n/centurion_tr.ts'))

areas = ["Processor, Memory & Board", "GPU, Drivers & Displays", "Drives & File Systems", "Wi-Fi, Bluetooth & Network",
         "Power, Battery & Temperatures", "PCIe & USB Devices", "Kernel, Firmware & Security", "Config Files, Audio & Snapshot"]
nav = ["System Overview", "Events & Alerts", "Boot History", "Curve Stability", "Kernel Log",
       "Kernel Parameters", "Sleep & Wake", "Battery Use", "dGPU Power", "Drive Health",
       "Network Protection", "System Tools"]
# Symptom tiles come from advanceddiagnostics::triageCategories() (their own translations).
legacy = {
    "Hardware Snapshot": "Hardware snapshot",
    "NVIDIA Module Parameters": "NVIDIA module parameters",
    "PCIe Detail": "PCIe detail",
    "SMART Details": "SMART details",
    "Firmware Updates": "Firmware updates",
    "PCIe Link Check": "PCIe Link Negotiation Lab",
    "Wi-Fi & Bluetooth Check": "Wi-Fi / Bluetooth Firmware Doctor",
    "Power Limits": "Power Limit Source Map",
    "Memory Layout": "Memory Architecture Explorer",
    "ACPI & WMI Firmware": "ACPI / WMI Firmware Browser",
    "USB-C Charger": "USB-C PD / Charger Inspector",
    "Audio (PipeWire)": "PipeWire Audio Doctor",
    "Kernel Features & Firmware": "Kernel Feature & Firmware Check",
}
# Ensure changing display labels did not mutate any of the settings IDs.
assert all(f'QT_TRANSLATE_NOOP("toolcatalog", "{name}")' in tc for name in areas)
assert all(f'tr("{name}")' in h for name in nav)
assert all(f'QT_TRANSLATE_NOOP("systools", "{name}")' in st for name in legacy)
assert 'actionBar(stableToolKey(s.title)' in st
assert 'setProperty("toolKey", stableToolKey(s.title))' in st
for name, key in legacy.items():
    assert f'if (display == QLatin1String("{name}")) return QStringLiteral("{key}");' in st
    assert re.search(r'\{"' + re.escape(key) + r'",\s*\{A::', tc)
assert 'config->setProperty("toolKey", QStringLiteral("Configuration files"))' in st
for name in ("Network", "Storage", "Kernel"):
    assert f'tr("{name} Tuning"), QStringLiteral("{name} tuning")' in mw
assert '"health/pinned_tools"' in h and '"health/recent_tools"' in h
assert all(f'tr("{name}")' in h for name in ("Troubleshoot by Symptom", "Favorites", "All Tools"))
for bad in ('QT_TR_NOOP("GPU & display")', 'QT_TR_NOOP("CPU & stability")',
            'QT_TR_NOOP("Crashes & kernel")', 'QT_TRANSLATE_NOOP("toolcatalog", "CPU & memory")'):
    assert bad not in h + tc

translations = {}
for context in xml.getroot().findall('context'):
    translations[context.findtext('name')] = {
        message.findtext('source'): message.findtext('translation')
        for message in context.findall('message')
    }
for name in ("Favorites", "Troubleshoot by Symptom"):
    assert translations['HealthTab'].get(name), f'Missing Turkish translation: {name}'
for name in areas:
    assert translations['toolcatalog'].get(name), f'Missing Turkish category: {name}'
for name in legacy:
    assert translations['systools'].get(name), f'Missing Turkish tool name: {name}'

print('PASS: Health headings, symptom labels, all System Tools categories and keys')
print('PASS: stable legacy tool IDs, root action dispatch and saved favorites')
print('PASS: Turkish translation entries and TS XML validity')
