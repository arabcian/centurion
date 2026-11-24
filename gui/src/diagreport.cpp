#include "diagreport.h"

#include <QCoreApplication>
#include <QRegularExpression>

namespace diag {

QString levelName(Level l) {
    switch (l) {
    case Level::Attention: return QCoreApplication::translate("diag", "Needs attention");
    case Level::Review: return QCoreApplication::translate("diag", "Review");
    case Level::Unknown: return QCoreApplication::translate("diag", "Unknown");
    case Level::Ok: return QCoreApplication::translate("diag", "No problem found");
    case Level::Info: break;
    }
    return QCoreApplication::translate("diag", "Information");
}

Finding &Report::add(Level l, const QString &title, const QString &detail, const QString &action) {
    findings.append(Finding{l, title, detail, action, {}, {}});
    return findings.last();
}

int Report::count(Level l) const {
    int n = 0;
    for (const Finding &f : findings) n += f.level == l;
    return n;
}

Level Report::verdict() const {
    if (count(Level::Attention)) return Level::Attention;
    if (count(Level::Review)) return Level::Review;
    if (count(Level::Ok)) return Level::Ok;
    if (count(Level::Unknown)) return Level::Unknown;
    return Level::Info;
}

QString Report::headline() const {
    const int a = count(Level::Attention), r = count(Level::Review), u = count(Level::Unknown);
    QString s;
    switch (verdict()) {
    case Level::Attention:
        s = a == 1 ? QCoreApplication::translate("diag", "1 finding needs attention") : QCoreApplication::translate("diag", "%1 findings need attention").arg(a);
        if (r) s += QCoreApplication::translate("diag", ", %1 to review").arg(r);
        break;
    case Level::Review: s = r == 1 ? QCoreApplication::translate("diag", "1 finding to review") : QCoreApplication::translate("diag", "%1 findings to review").arg(r); break;
    case Level::Ok: s = QCoreApplication::translate("diag", "No problem found in the data that could be read"); break;
    case Level::Unknown: return QCoreApplication::translate("diag", "Not enough data for a verdict");
    case Level::Info: return QCoreApplication::translate("diag", "Information only");
    }
    if (u) s += QCoreApplication::translate("diag", " · %1 source(s) unavailable").arg(u);
    return s;
}

QString Report::toText() const { return toTextWithVerdict(*this); }

QString Report::toTextWithVerdict(const Report &original) const {
    QStringList out;
    out << QStringLiteral("%1 — %2").arg(code, title);
    if (!about.isEmpty()) out << about;
    out << QCoreApplication::translate("diag", "Verdict: %1 — %2").arg(levelName(original.verdict()), original.headline()) << QString();
    for (const Finding &f : findings) {
        out << QStringLiteral("[%1] %2").arg(levelName(f.level), f.title);
        if (!f.detail.isEmpty()) out << QStringLiteral("    ") + f.detail;
        if (!f.action.isEmpty()) out << QStringLiteral("    → ") + f.action;
    }
    for (const Table &t : tables) {
        out << QString() << QStringLiteral("== %1 ==").arg(t.title);
        // Column widths from the longest cell, capped so one long value does not push the rest away.
        QList<int> w;
        for (const QString &h : t.headers) w << qMin(int(h.size()), 48);
        for (const QStringList &row : t.rows)
            for (int i = 0; i < row.size() && i < w.size(); ++i) w[i] = qMin(qMax(w[i], int(row[i].size())), 48);
        auto line = [&w](const QStringList &cells) {
            QStringList c;
            for (int i = 0; i < cells.size(); ++i) c << (i < w.size() && i + 1 < cells.size() ? cells[i].leftJustified(w[i]) : cells[i]);
            return c.join(QStringLiteral("  ")).trimmed();
        };
        if (!t.headers.isEmpty()) out << line(t.headers);
        for (const QStringList &row : t.rows) out << line(row);
        if (t.rows.isEmpty()) out << QCoreApplication::translate("diag", "(nothing to list)");
        if (!t.note.isEmpty()) out << t.note;
    }
    if (!limits.isEmpty()) {
        out << QString() << QCoreApplication::translate("diag", "Limits of this check:");
        for (const QString &l : limits) out << QStringLiteral("  • ") + l;
    }
    if (!raw.trimmed().isEmpty()) out << QString() << QCoreApplication::translate("diag", "Technical details:") << raw.trimmed();
    return out.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

QString redactPrivateText(QString text) {
    // MACs require six octets, unlike PCI addresses. Keep error codes and PCI BDFs.
    static const QList<QRegularExpression> patterns{
        QRegularExpression(QStringLiteral(R"((?i)\b(?:[0-9a-f]{2}:){5}[0-9a-f]{2}\b)")),
        QRegularExpression(QStringLiteral(R"(\b(?:[0-9]{1,3}\.){3}[0-9]{1,3}\b)")),
        QRegularExpression(QStringLiteral(R"((?i)(?<![0-9a-f:])(?:[0-9a-f]{0,4}:){2,7}[0-9a-f]{0,4}(?![0-9a-f:]))")),
        QRegularExpression(QStringLiteral(R"((?i)(?:/home/|/Users/)[^/\s]+)")),
        QRegularExpression(QStringLiteral(R"((?im)\b(?:serial(?: number)?|hostname|host name|user name|username|seri numarası|makine adı|kullanıcı adı)\s*[:=]\s*[^\r\n]+)"))
    };
    // Preserve PCI addresses: IPv6 detection is deliberately conservative and excludes BDF suffixes.
    for (int i = 0; i < patterns.size(); ++i) {
        if (i != 2) { text.replace(patterns[i], QStringLiteral("[redacted]")); continue; }
        static const QRegularExpression pci(QStringLiteral(R"((?i)^[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}$)"));
        auto matches = patterns[i].globalMatch(text);
        QList<QPair<qsizetype, qsizetype>> spans;
        while (matches.hasNext()) {
            const auto m = matches.next();
            const auto end = m.capturedEnd();
            if (pci.match(m.captured()).hasMatch() && end < text.size() && text[end] == QLatin1Char('.')) continue;
            spans.append(qMakePair(m.capturedStart(), m.capturedLength()));
        }
        for (auto it = spans.crbegin(); it != spans.crend(); ++it) text.replace(it->first, it->second, QStringLiteral("[redacted]"));
    }
    return text;
}

void redactPrivateFields(Report &report) {
    static const QStringList privateHeaders{QStringLiteral("serial"), QStringLiteral("serial number"),
        QStringLiteral("serial no"), QStringLiteral("seri numarası"), QStringLiteral("seri no"),
        QStringLiteral("hostname"), QStringLiteral("host name"), QStringLiteral("makine adı"),
        QStringLiteral("username"), QStringLiteral("user name"), QStringLiteral("kullanıcı adı"),
        QStringLiteral("mac"), QStringLiteral("mac address"), QStringLiteral("bssid"), QStringLiteral("address (bssid)"),
        QStringLiteral("ip"), QStringLiteral("ip address"), QStringLiteral("ip adresi")};
    for (auto &table : report.tables)
        for (qsizetype i=0; i<table.headers.size(); ++i)
            if (privateHeaders.contains(table.headers[i].simplified().toLower()))
                for (auto &row : table.rows)
                    if (i<row.size() && !row[i].isEmpty()) row[i]=QStringLiteral("[redacted]");
}

}  // namespace diag
