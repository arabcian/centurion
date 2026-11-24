#!/usr/bin/env python3
"""Dependency-free contracts for the Centurion Health / System Tools directory.

Static source checks only; NOT a Qt compilation or visual rendering test.
Run: python3 gui/tests/health_catalog_redesign_static_test.py
"""
from pathlib import Path

src = Path(__file__).resolve().parents[1] / 'src'
health = (src / 'healthtab.cpp').read_text()
header = (src / 'healthtab.h').read_text()
overview = (src / 'overviewpage.cpp').read_text()
triage = (src / 'advanceddiagnostics.cpp').read_text()


def has_all(*phrases):
    return all(p in health for p in phrases)


catalog = health.split('void HealthTab::rebuildToolCatalog() {', 1)[1].split('\nbool HealthTab::eventFilter(', 1)[0]
card = health.split('QWidget *HealthTab::toolButton(', 1)[1].split('\nvoid HealthTab::rebuildToolCatalog()', 1)[0]

checks = {
    'search and page title still available': has_all('healthDirectoryTitle', 'healthToolSearch', 'Search tools, error codes or symptoms'),
    'status counters, tiles and note removed from System Tools': all(x not in catalog for x in ('healthStatusTile', 'overview_->cards()', 'statusNote', 'Needs attention', 'Not checked')),
    'every H18 category is an entry point, as on System Overview': has_all('healthSymptomTile', 'investigations_->startTriage(key)', 'advanceddiagnostics::triageCategories()'),
    'all six H18 category keys still valid': all(f'QStringLiteral("{x}")' in triage for x in ('gpu', 'storage', 'radio', 'sleep', 'cpu', 'crash')),
    'symptom grid uses useful readable responsive widths': has_all('width >= 1110 ? 3 : (width >= 710 ? 2 : 1)', 'healthSymptomTile'),
    'symptom tiles size to contents rather than button text sizeHint': has_all('class DiagnosticEntryCard final : public QFrame', 'new DiagnosticEntryCard', 'setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred)'),
    'symptom tiles remain fully interactive by mouse and keyboard': has_all('mouseReleaseEvent(QMouseEvent *event)', 'keyPressEvent(QKeyEvent *event)', 'Qt::Key_Return', 'Qt::Key_Space', 'setActivation([this, key = symptom.key]'),
    'symptom labels and explanations word wrap': has_all('auto *name = new QLabel(label)', 'auto *description = new QLabel(QCoreApplication::translate("advanceddiagnostics", symptom.label))', 'description->setWordWrap(true)'),
    'favorites are the exclusive source of the Favorites cards': 'const QList<const Tool *> favorites = byKeys(toolList(QStringLiteral("health/pinned_tools")))' in catalog,
    'Favorites has a small empty state, not random default cards': all(x in catalog for x in ('No favorites yet.', 'if (!favorites.isEmpty())', 'v->addWidget(heading(tr("Favorites")')) and 'recommendedKeys' not in catalog,
    'quick access no hardcoded EDID/firmware/power defaults': all(k not in catalog for k in ('QStringLiteral("EDID / VRR")', 'QStringLiteral("Power Limit Source Map")', 'QStringLiteral("Hardware snapshot")')),
    'favorite cards honor available width': 'std::min(toolFeaturedColumns_, int(favorites.size()))' in catalog and 'width / 390, 1, 3' in catalog,
    'cards let text grow instead of clipping': 'setMaximumHeight' not in card and 'QFrame' in card and 'auto *description = new QLabel(about)' in card,
    'pinning does not open the underlying tool': has_all('healthCatalogPin', 'guiSettings().setValue(QStringLiteral("health/pinned_tools")', 'card->setActivation([this, page] { openTool(page); })'),
    'tool opening still uses lazy original page': has_all('toolPages_->setCurrentWidget(page)', 'systools::addPages(&toolsRegistry)'),
    'filtered directory remains two column': has_all('width / 480, 1, 2', 'healthCatalogSection', 'healthToolCategory'),
    'real tool safety badges and filters preserved': has_all('toolcatalog::badges(info.flags)', 'toolFilter_ == QLatin1String("changes")', 'toolFilter_ == QLatin1String("safe")'),
    'areas, recent items and pins remain filterable': has_all('healthToolAreaFilter', 'health/recent_tools', 'health/pinned_tools'),
    'collapse group settings still use stable area IDs': has_all('static_cast<int>(area)', 'health/catalog_collapsed_groups'),
    'catalog swaps do not delete a clicked widget immediately': has_all('takeWidget()', 'old->deleteLater()'),
    'Health navigation still links to System Tools': 'Browse System Tools' in overview and 'openPage(QStringLiteral("tools"))' in overview,
    'shortcut does not override global Ctrl+K': 'Qt::CTRL | Qt::SHIFT | Qt::Key_F' in health,
    'no eager background tool execution': 'toolPages_->setCurrentWidget(page)' in health,
}

for name, passed in checks.items():
    print(('PASS' if passed else 'FAIL') + ': ' + name)
assert all(checks.values())
print(f'PASS: {len(checks)} Health / System Tools UI contracts')
