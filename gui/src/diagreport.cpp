#include "diagreport.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
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
        QRegularExpression(QStringLiteral(R"((?i)(?<![\w:])(?:[0-9a-f]{0,4}:){2,7}[0-9a-f]{0,4}(?![\w:]))")),
        QRegularExpression(QStringLiteral(R"((?i)(?:/home/|/Users/)[^/\s]+)")),
        QRegularExpression(QStringLiteral(R"((?i)\b(?:boot(?: id)?|machine(?: id)?|disk uuid|device uuid|gpu uuid|uuid)[ \t]*[:=][ \t]*[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}\b)")),
        QRegularExpression(QStringLiteral(R"((?im)\b(?:serial(?: number)?|hostname|host name|user name|username|seri numarası|makine adı|kullanıcı adı)[ \t]*[:=][ \t]*[^\r\n]+)")),
        QRegularExpression(QStringLiteral(R"((?i)\b(?:PARTUUID|UUID|GPU UUID|machine[- ]id|boot[- ]id)[ \t]*[:=][ \t]*(?:GPU-)?(?:[0-9a-f]{32}|[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}|[0-9a-f]{4}-[0-9a-f]{4})\b)")),
        QRegularExpression(QStringLiteral(R"((?i)\bGPU-[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}\b)")),
        QRegularExpression(QStringLiteral(R"((?i)(?:/run/media/|/media/)[^/\s]+)")),
        QRegularExpression(QStringLiteral(R"((?im)^[ \t]*(?:SSID|BSSID|ID_SERIAL(?:_SHORT)?|ID_FS_UUID|ID_PART_ENTRY_UUID|POWER_SUPPLY_SERIAL_NUMBER|DMI_PRODUCT_UUID|PRETTY_HOSTNAME)[ \t]*[:=][ \t]*[^\r\n]+)"))
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
            // An IPv6 address has all eight groups or a "::"; "18:22:05" and "0C14:00:" have neither.
            if (!m.captured().contains(QStringLiteral("::")) && m.captured().count(QLatin1Char(':')) != 7) continue;
            if (pci.match(m.captured()).hasMatch() && end < text.size() && text[end] == QLatin1Char('.')) continue;
            spans.append(qMakePair(m.capturedStart(), m.capturedLength()));
        }
        for (auto it = spans.crbegin(); it != spans.crend(); ++it) text.replace(it->first, it->second, QStringLiteral("[redacted]"));
    }
    static const QRegularExpression unameHost(QStringLiteral(R"((?m)(^\$ uname -a\r?\nLinux[ \t]+)[^ \t\r\n]+)"));
    text.replace(unameHost, QStringLiteral("\\1[redacted]"));
    return text;
}

void redactPrivateFields(Report &report) {
    static const QStringList privateHeaders{QStringLiteral("serial"), QStringLiteral("serial number"),
        QStringLiteral("serial no"), QStringLiteral("seri numarası"), QStringLiteral("seri no"),
        QStringLiteral("hostname"), QStringLiteral("host name"), QStringLiteral("makine adı"),
        QStringLiteral("username"), QStringLiteral("user name"), QStringLiteral("kullanıcı adı"),
        QStringLiteral("mac"), QStringLiteral("mac address"), QStringLiteral("bssid"), QStringLiteral("address (bssid)"),
        QStringLiteral("ip"), QStringLiteral("ip address"), QStringLiteral("ip adresi"),
        QStringLiteral("uuid"), QStringLiteral("machine id"), QStringLiteral("boot id"), QStringLiteral("boot kimliği"), QStringLiteral("host"), QStringLiteral("ssid"), QStringLiteral("mac adresi"),
        QStringLiteral("disk uuid"), QStringLiteral("device uuid"), QStringLiteral("gpu uuid"), QStringLiteral("partition uuid"),
        QStringLiteral("partuuid"), QStringLiteral("machine-id"), QStringLiteral("boot-id"), QStringLiteral("serial number field"),
        QStringLiteral("mountpoint"), QStringLiteral("mount point"), QStringLiteral("mountpoints"), QStringLiteral("bağlama noktası")};
    for (auto &table : report.tables)
        for (qsizetype i=0; i<table.headers.size(); ++i)
            if (privateHeaders.contains(table.headers[i].simplified().toLower()))
                for (auto &row : table.rows)
                    if (i<row.size() && !row[i].isEmpty()) row[i]=QStringLiteral("[redacted]");
}

QJsonObject archiveSnapshot(const Report &source) {
    Report report = source;
    redactPrivateFields(report);
    QJsonArray records;
    auto safe = [](const QString &text) { return redactPrivateText(text); };
    for (const Finding &f : report.findings)
        records.append(QJsonArray{QStringLiteral("finding"), int(f.level), safe(f.title), safe(f.detail), safe(f.action)});
    for (const Table &t : report.tables) {
        // EDID binary data can encode serials even after textual masking.
        if (t.privateBinary) continue;
        QJsonArray headers; for (const auto &h : t.headers) headers.append(safe(h));
        records.append(QJsonArray{QStringLiteral("table"), safe(t.title), headers, safe(t.note)});
        for (qsizetype i = 0; i < t.rows.size(); ++i) {
            QJsonArray cells; for (const auto &v : t.rows.at(i)) cells.append(safe(v));
            records.append(QJsonArray{QStringLiteral("row"), safe(t.title), headers, cells, int(t.levels.value(i, Level::Info))});
        }
    }
    for (const auto &limit : report.limits) records.append(QJsonArray{QStringLiteral("limit"), safe(limit)});
    return QJsonObject{{"version", 1}, {"code", report.code}, {"title", safe(report.title)}, {"verdict", int(report.verdict())}, {"records", records}};
}

QStringList archiveDiff(const QJsonObject &before, const QJsonObject &after) {
    auto valid = [](const QJsonObject &o) { return o.value(QStringLiteral("version")).toInt() == 1 && !o.value(QStringLiteral("code")).toString().isEmpty() && o.value(QStringLiteral("records")).isArray(); };
    if (!valid(before) || !valid(after) || before.value(QStringLiteral("code")) != after.value(QStringLiteral("code")))
        return {QCoreApplication::translate("diag", "Incompatible diagnostic snapshot schema or report code.")};
    QMap<QByteArray, int> delta;
    for (const auto &v : before.value(QStringLiteral("records")).toArray()) {
        if (!v.isArray()) return {QCoreApplication::translate("diag", "Invalid diagnostic snapshot record.")};
        --delta[QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact)];
    }
    for (const auto &v : after.value(QStringLiteral("records")).toArray()) {
        if (!v.isArray()) return {QCoreApplication::translate("diag", "Invalid diagnostic snapshot record.")};
        ++delta[QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact)];
    }
    QStringList out;
    if (before.value(QStringLiteral("verdict")) != after.value(QStringLiteral("verdict"))) out << QCoreApplication::translate("diag", "Report verdict changed.");
    for (auto it = delta.cbegin(); it != delta.cend(); ++it) {
        if (it.value() == 0) continue;
        out << QStringLiteral("%1%2 × %3").arg(it.value() > 0 ? QStringLiteral("+") : QStringLiteral("−")).arg(qAbs(it.value())).arg(QString::fromUtf8(it.key()));
    }
    return out;
}

}  // namespace diag
