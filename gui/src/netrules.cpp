#include "netrules.h"

#include <QHostAddress>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QUrl>

namespace {
// One "n" or "a-b" item of a port list.
bool portItem(const QString &item, int &lo, int &hi) {
    const QStringList p = item.split(QLatin1Char('-'));
    if (p.size() > 2) return false;
    bool ok1 = false, ok2 = false;
    lo = p.first().trimmed().toInt(&ok1);
    hi = p.last().trimmed().toInt(&ok2);
    return ok1 && ok2 && lo >= 1 && hi <= 65535 && lo <= hi;
}

bool validNetItem(const QString &e) {
    QHostAddress a;
    if (a.setAddress(e)) return true;
    if (e.contains(QLatin1Char('/'))) {
        const auto net = QHostAddress::parseSubnet(e);
        return !net.first.isNull() && net.second > 0;  // the helper refuses /0
    }
    const int dash = e.indexOf(QLatin1Char('-'));
    if (dash <= 0) return false;
    QHostAddress lo, hi;
    return lo.setAddress(e.left(dash).trimmed()) && hi.setAddress(e.mid(dash + 1).trimmed()) && lo.protocol() == hi.protocol();
}
}  // namespace

QList<int> expandPorts(const QString &text, int max) {
    QList<int> out;
    QSet<int> seen;
    for (const QString &item : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        int lo = 0, hi = 0;
        if (!portItem(item, lo, hi)) return {};
        if (hi - lo >= max) return {};
        for (int p = lo; p <= hi; ++p) {
            if (seen.contains(p)) continue;
            seen.insert(p);
            out << p;
            if (out.size() > max) return {};
        }
    }
    return out;
}

bool validPortList(const QString &text) {
    const QStringList items = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (items.size() > 32) return false;
    for (const QString &item : items) {
        int lo = 0, hi = 0;
        if (!item.trimmed().isEmpty() && !portItem(item, lo, hi)) return false;
    }
    return true;
}

bool validRemote(const QString &text) {
    const QString t = text.trimmed();
    if (t.isEmpty() || t == QLatin1String("lan")) return true;
    for (const QString &item : t.split(QLatin1Char(','), Qt::SkipEmptyParts))
        if (!validNetItem(item.trimmed())) return false;
    return true;
}

bool validInterface(const QString &text) {
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_.-]{1,15}\\*?$"));
    return text.isEmpty() || (text != QLatin1String("lo") && text.size() <= 15 && re.match(text).hasMatch());
}

QString lookupHost(const QString &text) {
    QString t = text.trimmed();
    if (t.contains(QStringLiteral("://"))) t = QUrl(t).host();
    QHostAddress a;
    if (a.setAddress(t) || t.contains(QLatin1Char('/'))) return t;   // an address or a CIDR block stays as typed
    const QByteArray ace = QUrl::toAce(t);
    return ace.isEmpty() ? t : QString::fromLatin1(ace);
}

bool validHost(const QString &host) {
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_.:][A-Za-z0-9_.:-]{0,252}$"));
    return re.match(host).hasMatch();
}

QString reverseDnsName(const QString &ip) {
    QHostAddress a;
    if (!a.setAddress(ip)) return {};
    QStringList parts;
    if (a.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 v = a.toIPv4Address();
        for (int i = 0; i < 4; ++i) parts << QString::number((v >> (8 * i)) & 0xff);
        return parts.join(QLatin1Char('.')) + QStringLiteral(".in-addr.arpa");
    }
    const Q_IPV6ADDR v = a.toIPv6Address();
    for (int i = 15; i >= 0; --i) {
        parts << QString::number(v.c[i] & 0xf, 16);
        parts << QString::number(v.c[i] >> 4, 16);
    }
    return parts.join(QLatin1Char('.')) + QStringLiteral(".ip6.arpa");
}
