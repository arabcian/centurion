#pragma once
// Health → tool pages: one sub-tab per system tool (sensors, lscpu, lspci,
// dmidecode, smartctl, …). A page runs its commands the first time it is
// shown and again only on "Run again" — nothing runs in the background.
// User-level commands run directly; root-only ones through tune-helper's
// fixed whitelist ({"op":"tool"}).
class QString;
class QTabWidget;

namespace systools {
void addPages(QTabWidget *tabs);
/// The command that installs `tool` on this distribution ("emerge sys-apps/memtester"), or just its name.
QString installCommand(const QString &tool);
}
