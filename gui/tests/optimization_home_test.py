#!/usr/bin/env python3
"""Rows that moved off the Optimizations tab (tune.rs HOME_*) must not be set by a built-in preset."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class HomeRowsTest(unittest.TestCase):
    def test_moved_rows_are_not_in_builtin_presets(self):
        tune = (ROOT / 'crates/centurion-helpers/src/tune.rs').read_text()
        moved = set()
        for name in ('HOME_NETWORK', 'HOME_STORAGE', 'HOME_KERNEL'):
            body = re.search(r'pub const %s: &\[&str\] = &\[(.*?)\];' % name, tune, re.S).group(1)
            moved |= set(re.findall(r'"([^"]+)"', body))
        self.assertGreater(len(moved), 50)
        cpp = (ROOT / 'gui/src/optimizetab.cpp').read_text().splitlines()
        used = set(re.findall(r'"([a-z0-9_]+\.[a-z0-9_.]+)"\s*:', '\n'.join(cpp[90:260])))
        self.assertGreater(len(used), 30)
        self.assertEqual(set(), moved & used)


if __name__ == '__main__':
    unittest.main()
