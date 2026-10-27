#pragma once
// Interface language. The source strings are English; a translation is a compiled catalogue
// (gui/i18n/centurion_<lang>.ts → .qm, embedded in the executable under :/i18n/), so nothing is
// installed or looked up at run time. A string without a translation shows in English.
//
// Choice, in order: gui.ini "language" ("en" / "tr"), CENTURION_LANG, the desktop's language
// ("auto", the default). Changing it applies at the next start, like the colour theme.
#include <QList>
#include <QPair>
#include <QString>

class QCoreApplication;

namespace i18n {
/// (id, name) in menu order; each name is written in its own language. "auto" comes first.
QList<QPair<QString, QString>> languages();
/// The saved choice: "auto", "en" or "tr".
QString choice();
bool saveChoice(const QString &id);
/// The language in use: "en" or a translated one ("tr").
QString effective();
/// Installs the translators for effective() (the app's catalogue, and Qt's own for the standard
/// dialog buttons when the system has them). Call once, before any widget exists.
void install(QCoreApplication &app);

/// The language code `choice` / the environment / the desktop locale resolve to, among `available`.
/// Pure function of its arguments (tested).
QString resolve(const QString &saved, const QString &env, const QString &systemLocale, const QStringList &available);
}  // namespace i18n
