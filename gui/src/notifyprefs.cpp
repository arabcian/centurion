#include "notifyprefs.h"

#include <QSettings>
#include <QStandardPaths>

namespace notifyprefs {

static QSettings guiIni() {
    return QSettings(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/centurion/gui.ini"),
                     QSettings::IniFormat);
}

Prefs load() {
    const QSettings st = guiIni();
    Prefs p;
    p.off = st.value(QStringLiteral("notify/off")).toStringList();
    p.criticalOnly = st.value(QStringLiteral("notify/critical_only"), false).toBool();
    p.deferGame = st.value(QStringLiteral("notify/defer_game"), false).toBool();
    return p;
}

void save(const Prefs &p) {
    QSettings st = guiIni();
    st.setValue(QStringLiteral("notify/off"), p.off);
    st.setValue(QStringLiteral("notify/critical_only"), p.criticalOnly);
    st.setValue(QStringLiteral("notify/defer_game"), p.deferGame);
}

Verdict decide(const Prefs &p, const QString &category, const QString &severity, bool gameRunning) {
    if (p.off.contains(category)) return Verdict::Hide;
    if (category == KERNEL && p.criticalOnly && severity != CRITICAL_LEVEL) return Verdict::Hide;
    if (p.deferGame && gameRunning) return Verdict::Defer;
    return Verdict::Show;
}

}  // namespace notifyprefs
