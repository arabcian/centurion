#!/usr/bin/env python3
"""Dependency-free source/config and historical-log contract checks for the Health UI.

These checks complement, but never replace, a real Qt6 build and GUI smoke test.
Run: python3 gui/tests/health_ui_static_test.py
"""
from pathlib import Path
import re

SRC = Path(__file__).resolve().parents[1] / "src"
health = (SRC / "healthtab.cpp").read_text()
header = (SRC / "healthtab.h").read_text()
overview = (SRC / "overviewpage.cpp").read_text()
smart = (SRC / "smartpage.cpp").read_text()

checks = {
    "single-level Health stack": 'pages_ = new QStackedWidget' in health and 'sub_->' not in health,
    "searchable primary navigation": 'healthNavSearch' in health and 'navigation_->setCurrentItem' in health,
    "searchable system tool catalog": 'healthToolSearch' in health and 'toolPages_ = new QStackedWidget' in health,
    "original lazy system tools reused": 'systools::addPages(&toolsRegistry)' in health,
    "overview deep links preserved": 'OverviewPage::openPage' in health and 'showPage(page)' in health,
    "SMART remains accessible directly": 'addPage(hardware, smartTool' in health,
    "event/search filters": 'severityFilter_->addItem' in health and 'eventSearch_->setClearButtonEnabled' in health,
    "simple and technical event views": 'friendlyEvent(' in health and 'technicalEvents_' in header,
    "older saved event days": 'void HealthTab::loadHistoryDay()' in health and 'logPath() + QStringLiteral(".1")' in health,
    "archived rows excluded from live DBus events": 'if (list_->topLevelItem(i)->data(0, Qt::UserRole + 3).toBool()) continue' in health,
    "read errors visible in simple view": 'scanWarning_->setVisible(true)' in health,
    "user acknowledgment does not delete logs": 'void HealthTab::markSeen()' in health and 'guiSettings().setValue(QStringLiteral("health/seen_ms")' in health,
    "no invented hardware health verdict": 'Only logged and available indicators are assessed' in overview,
    "dashboard priority sorting": 'std::stable_sort(sorted.begin(), sorted.end()' in overview,
    "recommendations lead to actual pages": 'Q_EMIT openPage(recommendedTarget_)' in overview,
    "responsive overview": 'viewport()->width() >= 650 ? 2 : 1' in overview,
    "drive health summary and advanced table": 'detailsButton_->setCheckable(true)' in smart and 'cardsLayout_->addWidget(card)' in smart,
    "unknown drives not called healthy": 'insufficient ? tr("Not checked")' in smart,
}

# This represents the actual persisted line shape from HealthTab::persist.
line = ('2026-11-02 15:37:42  boot=12abc34d seq=1245  '
        'CRITICAL XID     Xid 79: GPU has fallen off the bus  |  NVRM: GPU has fallen off the bus')
pattern = r'^([0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2})\s+boot=([a-fA-F0-9]+)\s+seq=([0-9]+)\s+(\S+)\s+(\S+)\s+(.*?)\s+\|\s+(.*)$'
m = re.match(pattern, line)
checks['archived log record parsing example'] = bool(m and m.group(2) == '12abc34d' and m.group(3) == '1245' and m.group(6).startswith('Xid 79'))
checks['archived parser same in C++'] = pattern in health

for name, ok in checks.items():
    print(('PASS' if ok else 'FAIL') + ' - ' + name)
assert all(checks.values()), 'At least one static Health UI check failed.'
print(f'PASS: {len(checks)} static checks')
