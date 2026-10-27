#include "i18n.h"
#include <QCoreApplication>
#include <QFile>
#include <QLibraryInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QSettings>
#include <QTranslator>

namespace i18n {
namespace {
// Catalogues compiled into the executable (see CMakeLists.txt).
const QStringList AVAILABLE{QStringLiteral("tr")};

QSettings settings() {
    return QSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("centurion"), QStringLiteral("gui"));
}
}  // namespace

QList<QPair<QString, QString>> languages() {
    return {{QStringLiteral("auto"), QStringLiteral("Auto")}, {QStringLiteral("en"), QStringLiteral("English")}, {QStringLiteral("tr"), QStringLiteral("Türkçe")}};
}

QString choice() {
    const QString c = settings().value(QStringLiteral("language")).toString();
    for (const auto &l : languages()) if (l.first == c) return c;
    return QStringLiteral("auto");
}

bool saveChoice(const QString &id) {
    QSettings s = settings();
    s.setValue(QStringLiteral("language"), id);
    s.sync();
    return s.status() == QSettings::NoError;
}

QString resolve(const QString &saved, const QString &env, const QString &systemLocale, const QStringList &available) {
    auto code = [&](const QString &s) {  // "tr_TR.UTF-8" / "tr-TR" / "tr" -> "tr"
        const QString c = s.section(QRegularExpression(QStringLiteral("[_.@-]")), 0, 0).toLower();
        return available.contains(c) ? c : QStringLiteral("en");
    };
    if (saved == QLatin1String("en")) return QStringLiteral("en");
    if (!saved.isEmpty() && saved != QLatin1String("auto")) return code(saved);
    if (!env.isEmpty()) return code(env);
    return code(systemLocale);
}

QString effective() {
    return resolve(choice(), qEnvironmentVariable("CENTURION_LANG"), QLocale::system().name(), AVAILABLE);
}

void install(QCoreApplication &app) {
    const QString lang = effective();
    if (lang == QLatin1String("en")) return;
    auto *own = new QTranslator(&app);
    if (own->load(QStringLiteral(":/i18n/centurion_%1.qm").arg(lang))) app.installTranslator(own);
    else delete own;
    auto *qt = new QTranslator(&app);
    if (qt->load(QStringLiteral("qtbase_%1").arg(lang), QLibraryInfo::path(QLibraryInfo::TranslationsPath))) app.installTranslator(qt);
    else delete qt;
}
}  // namespace i18n
