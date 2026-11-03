#!/usr/bin/env python3
"""Offline localization integrity checks; run with Python 3, no Qt needed.

Checks coverage of the Rust-generated Optimization backend strings, which
lupdate cannot discover from the C++ dynamic translate() call alone.
"""
import json
import re
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GUI = ROOT / 'gui'
STR = r'"((?:\\.|[^"\\])*)"'
KNOB = re.compile(r'\bt\(\s*' + STR + r'\s*,\s*' + STR + r'\s*,\s*' + STR + r'\s*,\s*' + STR)
PH = re.compile(r'%(?:L?[1-9]\d?|n)')


def tunables():
    source = (ROOT / 'crates/centurion-helpers/src/tune.rs').read_text()
    return [tuple(json.loads('"' + x + '"') for x in match.groups())
            for match in KNOB.finditer(source)]


def catalogue():
    tree = ET.parse(GUI / 'i18n/centurion_tr.ts')
    return {context.findtext('name'): {m.findtext('source'): m
            for m in context.findall('message')}
            for context in tree.findall('context')}


class TurkishCatalogueTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rows = tunables()
        cls.catalog = catalogue()

    def test_all_optimization_help_translated_in_full(self):
        self.assertEqual(196, len(self.rows))
        texts = self.catalog['OptimizationBackend']
        for key, group, name, original in self.rows:
            with self.subTest(key=key):
                self.assertIn(original, texts)
                text = texts[original].findtext('translation') or ''
                self.assertGreater(len(text), 25)
                self.assertNotIn('Orijinal teknik açıklama (İngilizce)', text)
                self.assertRegex(text, r'[çğıöşüÇĞİÖŞÜ]')

    def test_all_optimization_setting_names(self):
        entries = self.catalog['OptimizationLabels']
        for key, group, label, help_text in self.rows:
            with self.subTest(key=key):
                self.assertIn(label, entries)
                self.assertTrue(entries[label].findtext('translation'))

    def test_groups_and_options_available(self):
        groups = self.catalog['OptimizationGroups']
        for _, group, _, _ in self.rows:
            self.assertIn(group, groups)
        self.assertIn('enabled', self.catalog['OptimizationOptions'])
        self.assertIn('disabled', self.catalog['OptimizationOptions'])

    def test_dynamic_sources_survive_lupdate(self):
        markers = (GUI / 'src/optimization_translation_sources.cpp').read_text()
        self.assertEqual(196, markers.count('QT_TRANSLATE_NOOP("OptimizationBackend",'))
        self.assertEqual(196, markers.count('QT_TRANSLATE_NOOP("OptimizationLabels",'))
        self.assertIn('src/optimization_translation_sources.cpp',
                      (GUI / 'CMakeLists.txt').read_text())

    def test_translations_nonempty_and_qt_placeholders(self):
        for context, messages in self.catalog.items():
            for source, message in messages.items():
                with self.subTest(context=context, source=source[:60]):
                    tr = message.find('translation')
                    self.assertIsNotNone(tr)
                    self.assertNotEqual('unfinished', tr.get('type'))
                    if message.get('numerus') == 'yes':
                        forms = [e.text for e in tr.findall('numerusform')]
                    else:
                        forms = [tr.text]
                    self.assertTrue(forms)
                    for translated in forms:
                        self.assertTrue(translated)
                        self.assertEqual(sorted(PH.findall(source)),
                                         sorted(PH.findall(translated)))


if __name__ == '__main__':
    unittest.main(verbosity=2)
