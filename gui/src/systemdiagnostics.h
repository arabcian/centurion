#pragma once
// Health → System tools: the interpreted pages (S04 … S18). Each reads sysfs / procfs as the user (no root,
// no device wake-up, no writes) and returns a diag::Report. `root` prefixes every path so tests run on a
// fake tree; the GUI passes "".
#include "diagreport.h"
#include "diagsources.h"

#include <QHash>
#include <QString>
#include <QStringList>

namespace systemdiagnostics {

diag::Report pcieLinks(const QString &root = {});                                     // S04
diag::Report radios(const QList<diagsrc::LogLine> &kernel, const QString &root = {});  // S07
diag::Report powerLimits(const QString &root = {});                                    // S08
diag::Report memory(const QString &root = {});                                         // S10
diag::Report firmware(const QString &root = {});                                       // S11
diag::Report charging(const QString &root = {});                                       // S13
/// `outputs`: "wpctl status" / "pactl info" → their output (empty = not installed / failed).
diag::Report audio(const QHash<QString, QString> &outputs, const QString &root = {});   // S15
/// `config`: the running kernel's config text (empty = not readable).
diag::Report kernelDeps(const QString &config, const QList<diagsrc::LogLine> &kernel, const QString &root = {});  // S18
/// /boot/config-<release>, else /proc/config.gz through zcat (bounded); empty when neither is readable.
QString kernelConfig();
/// "y", "m", "n" (explicitly not set), or "" when the option is not in the text.
QString configValue(const QString &config, const QString &option);
/// "32.0 GT/s PCIe" → 32.
double linkSpeed(const QString &s);

/// S04 … S18 by code; S15 is built from its command outputs by the page instead.
diag::Report byCode(const QString &code);

}  // namespace systemdiagnostics
