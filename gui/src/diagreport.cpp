#include "diagreport.h"

#include <QCoreApplication>

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

QString Report::toText() const {
    QStringList out;
    out << QStringLiteral("%1 — %2").arg(code, title);
    if (!about.isEmpty()) out << about;
    out << QCoreApplication::translate("diag", "Verdict: %1 — %2").arg(levelName(verdict()), headline()) << QString();
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

}  // namespace diag
