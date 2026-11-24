#pragma once
// What the Health navigation needs to know about each page and System tool beyond its widget: the area it
// belongs to (by purpose: CPU, graphics, storage …), words a user may search for (symptoms, error codes,
// Turkish and English), badges (changes settings, may wake the dGPU, needs root, uses the internet), the
// pages and tools that fit a guided-triage category, and short explanations of technical terms.
// Qt Core only: keyed by the untranslated tool key ("toolKey" property) or Health page key, so it is testable
// without widgets.
#include <QList>
#include <QString>
#include <QStringList>

namespace toolcatalog {

enum class Area { Cpu, Graphics, Storage, Network, Power, Devices, Firmware, System };
/// Display order of the areas.
QList<Area> areas();
QString areaName(Area a);

enum Flag { ChangesSettings = 1, MayWakeGpu = 2, NeedsRoot = 4, UsesInternet = 8 };

struct Info {
    Area area = Area::System;
    int flags = 0;
    QStringList keywords;
};
/// A System tool by its untranslated key; an unknown key is a plain System tool.
Info tool(const QString &key);
/// Search words of a Health page (its key: "monitor", "sleep" …); empty for an unknown key.
QStringList pageKeywords(const QString &key);
/// The badges of `flags` as short translated words, in a fixed order.
QStringList badges(int flags);

/// Lower case without diacritics, dotless ı as i: "Isınma" and "ısınma" both become "isinma".
QString normalize(const QString &s);
/// Every word of `query` appears in `label` or one of `keywords` (normalized). An empty query matches.
bool matches(const QString &query, const QString &label, const QStringList &keywords);

/// Where to look next for a guided-triage category ("gpu", "cpu" …): "page:<key>" or "tool:<key>", in order.
QStringList relatedFor(const QString &triageKey);
/// The System tools (and pages) that cover the same ground as a Health page ("smart", "cooling" …); empty for none.
QStringList relatedForPage(const QString &pageKey);

/// Short explanations of the technical terms (AER, ASPM, GSP, D3cold …) found as whole words in `text`,
/// one "TERM — what it is" line each; empty when none.
QString explainTerms(const QString &text);

}  // namespace toolcatalog
