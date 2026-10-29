#include "blocklistsubs.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

bool blocklistUrlOk(const QString &url) {
    const QUrl u(url);
    return url.size() <= 512 && u.isValid() && u.scheme() == QLatin1String("https") && !u.host().isEmpty();
}

bool blocklistDue(const BlocklistSub &s, qint64 nowSecs) {
    return s.updated == 0 || nowSecs - s.updated >= qint64(s.hours) * 3600;
}

ParsedBlocklist parseBlocklist(const QByteArray &text) {
    static const QRegularExpression dash(QStringLiteral("\\s*-\\s*")), sep(QStringLiteral("[,;\\s]+"));
    ParsedBlocklist out;
    const QStringList lines = QString::fromUtf8(text).split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.section(QLatin1Char('#'), 0, 0).replace(dash, QStringLiteral("-"));  // "a - b" stays one token
        for (const QString &tok : line.split(sep, Qt::SkipEmptyParts)) {
            if (out.entries.size() == BLOCKLIST_MAX_ENTRIES) {
                out.entries.clear();
                out.error = QCoreApplication::translate("blocklistsubs", "The list has more than %1 entries.").arg(BLOCKLIST_MAX_ENTRIES);
                return out;
            }
            out.entries << tok;
        }
    }
    if (out.entries.isEmpty()) out.error = QCoreApplication::translate("blocklistsubs", "The list has no entries.");
    return out;
}

QString blocklistSettingsPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/centurion/gui.ini");
}

QList<BlocklistSub> loadBlocklistSubs(const QString &path) {
    QSettings st(path, QSettings::IniFormat);
    QList<BlocklistSub> out;
    const int n = st.beginReadArray(QStringLiteral("netguard_subs"));
    for (int i = 0; i < n; ++i) {
        st.setArrayIndex(i);
        BlocklistSub s;
        s.name = st.value(QStringLiteral("name")).toString();
        s.url = st.value(QStringLiteral("url")).toString();
        s.hours = st.value(QStringLiteral("hours"), 168).toInt();
        s.updated = st.value(QStringLiteral("updated"), 0).toLongLong();
        s.entries = st.value(QStringLiteral("entries"), 0).toInt();
        s.error = st.value(QStringLiteral("error")).toString();
        out << s;
    }
    st.endArray();
    return out;
}

void saveBlocklistSubs(const QString &path, const QList<BlocklistSub> &subs) {
    QSettings st(path, QSettings::IniFormat);
    st.beginWriteArray(QStringLiteral("netguard_subs"), subs.size());
    for (int i = 0; i < subs.size(); ++i) {
        const BlocklistSub &s = subs.at(i);
        st.setArrayIndex(i);
        st.setValue(QStringLiteral("name"), s.name);
        st.setValue(QStringLiteral("url"), s.url);
        st.setValue(QStringLiteral("hours"), s.hours);
        st.setValue(QStringLiteral("updated"), s.updated);
        st.setValue(QStringLiteral("entries"), s.entries);
        st.setValue(QStringLiteral("error"), s.error);
    }
    st.endArray();
    st.sync();
}
