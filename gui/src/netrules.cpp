#include "netrules.h"
#include "firewallpreview.h"

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

#include <QJsonArray>
#include <QMap>
#include <QCoreApplication>

QStringList firewallConfigDiff(const QJsonObject &before, const QJsonObject &after) {
    auto tr = [](const char *s) { return QCoreApplication::translate("FirewallDiff", s); };
    struct Entry { QJsonObject rule; int position; };
    auto decode = [](const QJsonObject &o, QMap<qint64, Entry> &out) {
        if (!o.value("rules").isArray()) return false;
        int position = 0;
        for (const auto &v : o.value("rules").toArray()) {
            const auto r = v.toObject(); const qint64 id = r.value("id").toInteger(-1);
            if (id <= 0 || out.contains(id)) return false;
            out.insert(id, {r, ++position});
        }
        return true;
    };
    QMap<qint64, Entry> a,b;
    if (!decode(before,a) || !decode(after,b)) return {tr("Cannot compare: missing or duplicate rule identity.")};
    QStringList out;
    auto show = [&](const QJsonValue &v) { return v.isString() ? v.toString() : v.isBool() ? (v.toBool() ? tr("On") : tr("Off")) : v.isDouble() ? QString::number(v.toDouble()) : tr("Unknown"); };
    for (const char *key : {"enabled", "inbound", "outbound", "ping_in", "ping_out"}) {
        const auto k = QLatin1String(key);
        if (before.value(k) != after.value(k)) out << QStringLiteral("%1: %2 → %3").arg(k,show(before.value(k)),show(after.value(k)));
    }
    for (auto it = a.cbegin(); it != a.cend(); ++it) {
        const auto id = it.key(); const auto other = b.constFind(id);
        if (other == b.cend()) { out << tr("Rule %1 removed (position %2)").arg(id).arg(it->position); continue; }
        if (it->position != other->position) out << tr("Rule %1 position: %2 → %3").arg(id).arg(it->position).arg(other->position);
        for (const char *key : {"enabled", "dir", "action", "proto", "ports", "remote", "iface", "note"}) {
            const auto k = QLatin1String(key);
            const auto av = it->rule.value(k), bv = other->rule.value(k);
            if (av == bv) continue;
            if (av.isString() && bv.isString()) {
                const auto as = av.toString().toStdString(), bs = bv.toString().toStdString();
                if (k == QLatin1String("ports")) { auto x = firewallpreview::ports(as), y = firewallpreview::ports(bs); if (x && y && *x == *y) continue; }
                if (k == QLatin1String("remote")) { auto x = firewallpreview::remote(as), y = firewallpreview::remote(bs); if (x && y && *x == *y) continue; }
            }
            out << tr("Rule %1 · %2: %3 → %4").arg(id).arg(k,show(it->rule.value(k)),show(other->rule.value(k)));
        }
    }
    for (auto it = b.cbegin(); it != b.cend(); ++it) if (!a.contains(it.key()))
        out << tr("Rule %1 added (position %2)").arg(it.key()).arg(it->position);
    return out;
}

QString firewallConnectionPreview(const QJsonObject &config, const QString &direction,
                                  const QString &protocol, const QString &peer, int destinationPort,
                                  const QString &iface) {
    namespace fp = firewallpreview;
    auto tr = [](const char *s) { return QCoreApplication::translate("FirewallPreview", s); };
    const auto unknown = tr("Unknown — incomplete or unsupported saved configuration or packet fields.");
    if (!config.value("enabled").isBool() || !config.value("rules").isArray()) return unknown;
    const QSet<QString> configKeys{"enabled", "inbound", "outbound", "ping_in", "ping_out", "rules", "shadowed"};
    for (auto i = config.begin(); i != config.end(); ++i) if (!configKeys.contains(i.key())) return unknown;
    fp::Config c; c.enabled = config.value("enabled").toBool();
    c.inbound = fp::verdict(config.value("inbound").toString().toStdString());
    c.outbound = fp::verdict(config.value("outbound").toString().toStdString());
    const auto rules = config.value("rules").toArray(); if (rules.size() > 512) return unknown;
    QSet<qint64> ids;
    for (const auto &v : rules) {
        if (!v.isObject()) return unknown;
        const auto o = v.toObject();
        const QSet<QString> ruleKeys{"id", "enabled", "dir", "action", "proto", "ports", "remote", "iface", "note"};
        for (auto i = o.begin(); i != o.end(); ++i) if (!ruleKeys.contains(i.key())) return unknown;
        const auto id = o.value("id").toInteger(-1);
        if (id <= 0 || id > 4294967295LL || ids.contains(id) || !o.value("enabled").isBool()) return unknown;
        for (const char *key : {"dir", "action", "proto", "ports", "remote", "iface"}) if (!o.value(QLatin1String(key)).isString()) return unknown;
        ids.insert(id); fp::Rule r; r.id = uint32_t(id); r.enabled = o.value("enabled").toBool();
        const auto dir = o.value("dir").toString(); if (dir != "in" && dir != "out") return unknown;
        r.inbound = dir == "in"; r.action = fp::verdict(o.value("action").toString().toStdString());
        r.proto = o.value("proto").toString().toStdString(); r.iface = o.value("iface").toString().toStdString();
        const auto p = fp::ports(o.value("ports").toString().toStdString()); const auto a = fp::remote(o.value("remote").toString().toStdString());
        if (!p || !a || r.action == fp::Verdict::Unknown || (r.proto != "tcp" && r.proto != "udp" && r.proto != "any") || !fp::ifaceValid(r.iface)) return unknown;
        if (r.proto == "any" && p->empty() && a->kind == fp::Remote::Any && r.iface.empty()) return unknown;
        r.ports = *p; r.remote = *a; c.rules.push_back(std::move(r));
    }
    const auto a = fp::address(peer.toStdString());
    if (!a || (direction != "in" && direction != "out") || destinationPort < 1 || destinationPort > 65535) return unknown;
    const auto r = fp::evaluate(c, {direction == "in", protocol.toStdString(), iface.toStdString(), *a, unsigned(destinationPort)});
    if (r.value == fp::Verdict::Unknown) return unknown;
    if (r.value == fp::Verdict::Disabled) return tr("Saved firewall is disabled; effective verdict is unknown.");
    const auto decision = r.value == fp::Verdict::Allow ? tr("Allow within saved chain") : r.value == fp::Verdict::Block ? tr("Block within saved chain") : tr("Reject within saved chain");
    return r.rule ? tr("Rule %1: %2").arg(r.rule).arg(decision) : tr("Default policy: %1").arg(decision);
}
