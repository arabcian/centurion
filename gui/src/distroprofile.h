#pragma once
// Which distribution this is, and the backup defaults that follow from its standard file layout:
// the system image's exclude list (package caches, re-downloadable repository data, snapshot stores),
// where a restore usually goes, and what the configuration backup's "system profile" holds.
// Mirrors crates/centurion-helpers/src/distro.rs (a Rust test checks the exclude patterns).
#include <QList>
#include <QString>
#include <QStringList>

namespace distroprofile {

/// "gentoo", "arch", "debian", "fedora", "opensuse", "void", "alpine", or "" (unknown), from the
/// ID and ID_LIKE lines of an os-release file.
QString familyOf(const QString &osRelease);
/// This system's family (read once).
QString family();
/// PRETTY_NAME of this system ("Arch Linux"), or the family label.
QString prettyName();
/// Human name of a family ("Debian / Ubuntu").
QString familyLabel(const QString &family);
/// Every family the defaults know, in menu order ("" = generic last).
QStringList families();

/// The default exclude list of a system image on `family`.
QStringList imageExcludes(const QString &family);
/// One line on what the family's own excludes are ("pacman sync databases").
QString excludeSummary(const QString &family);
/// The previous built-in (Gentoo-only) default: a saved list equal to it was never edited.
QString legacyDefaults();
/// Swap files in use (/proc/swaps). The helper always leaves them out of an image.
QStringList swapFiles();

/// Large, regenerable or separately backed-up data a user may want to leave out too.
struct Optional { QString pattern, label, about; };
QList<Optional> optionalExcludes();

/// Placeholder of the restore target field.
QString restoreHint(const QString &family);
/// Tooltip of the "System profile" box.
QString profileAbout(const QString &family);
/// Whether a tar image of / suits this system at all (image-based systems: NixOS, Fedora Atomic, …); the reason when not.
QString imageCaveat();

}  // namespace distroprofile
