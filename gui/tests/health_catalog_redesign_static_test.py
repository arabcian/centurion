#!/usr/bin/env python3
"""Dependency-free source contracts for the Health / System Tools UI experiment.

These are NOT a substitute for Qt compilation or interactive rendering.
Run: python3 gui/tests/health_catalog_redesign_static_test.py
"""
from pathlib import Path
import re

src = Path(__file__).resolve().parents[1] / 'src'
health = (src / 'healthtab.cpp').read_text()
header = (src / 'healthtab.h').read_text()
overview = (src / 'overviewpage.cpp').read_text()
triage = (src / 'advanceddiagnostics.cpp').read_text()

def has_all(*phrases):
    return all(p in health for p in phrases)

checks = {
    'system tools real header and symptom search': has_all('healthDirectoryTitle', 'healthToolSearch', 'Search tools, error codes or symptoms'),
    'status derived from real overview cards': has_all('overview_->cards()', 'level == 2', 'level == 1', 'level == 0', 'healthCards.isEmpty()'),
    'missing measurements not displayed as healthy': has_all('tr("Not checked")', 'countUnavailable'),
    'status cards link to original overview': has_all('healthStatusTile', 'showPage(overview_)'),
    'six H18 symptom entry points': has_all('healthSymptomTile', 'investigations_->startTriage(key)'),
    'H18 category keys remain consistent': all(f'QStringLiteral("{x}")' in triage for x in ('gpu', 'storage', 'radio', 'sleep', 'cpu', 'crash')),
    'tool details still have lazy original implementation': has_all('toolPages_->setCurrentWidget(page)', 'systools::addPages(&toolsRegistry)'),
    'dense four-column grid replaced with two-column': has_all('width / 480, 1, 2', 'healthCatalogSection', 'healthToolCategory'),
    'tool selection and safety metadata retained': has_all('toolcatalog::badges(info.flags)', 'toolcatalog::tool(t.key)'),
    'explicit warning flags support useful filtering': has_all('toolFilter_ == QLatin1String("changes")', 'toolFilter_ == QLatin1String("safe")'),
    'area, recent, and pinned filters wired': has_all('healthToolAreaFilter', 'health/recent_tools', 'health/pinned_tools'),
    'collapsible groups do not persist translated names': has_all('static_cast<int>(area)', 'health/catalog_collapsed_groups'),
    'favorite actions do not open the underlying tool': has_all('healthCatalogPin', 'guiSettings().setValue(QStringLiteral("health/pinned_tools")'),
    'catalog replacement defers deletion during click': has_all('takeWidget()', 'old->deleteLater()'),
    'direct return to directory': has_all('showToolCatalog()', 'QTreeWidget::itemClicked'),
    'Health overview links directly to tools': 'Browse System Tools' in overview and 'openPage(QStringLiteral("tools"))' in overview,
    'shortcut does not replace global Ctrl+K': 'Qt::CTRL | Qt::SHIFT | Qt::Key_F' in health,
    'no eager background tools execution introduced': 'toolPages_->setCurrentWidget(page)' in health,
}

for name, passed in checks.items():
    print(('PASS' if passed else 'FAIL') + ': ' + name)
assert all(checks.values())
print(f'PASS: {len(checks)} redesign source contracts')
