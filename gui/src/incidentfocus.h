#pragma once
// Evidence Lab incident focus: one subject (a CPU, a PCI device, an Xid code, a Wi-Fi/Bluetooth adapter …)
// kept while moving between the five reports and across rescans, so the same incident is read in every
// tool without retyping it. Candidates are taken from the reports' own text; nothing is inferred.
#include <QRegularExpression>
#include <QStringList>

namespace incidentfocus {

/// Focus tokens found in report text, first-seen order, de-duplicated: "CPU 5", PCI addresses
/// ("0000:01:00.0", the domain optional), "Xid 79", "bank 5" (MCA bank).
inline QStringList candidates(const QString &text, int limit = 40) {
    static const QRegularExpression re(QStringLiteral(
        "\\b(CPU ?\\d{1,4}|(?:[0-9a-fA-F]{4}:)?[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\\.[0-7]|Xid ?\\d{1,4}|bank ?\\d{1,3})\\b"),
        QRegularExpression::CaseInsensitiveOption);
    QStringList out;
    auto it = re.globalMatch(text);
    while (it.hasNext() && out.size() < limit) {
        QString t = it.next().captured(1);
        // "cpu5" and "CPU 5" are the same subject.
        static const QRegularExpression word(QStringLiteral("^(cpu|xid|bank) ?(\\d+)$"), QRegularExpression::CaseInsensitiveOption);
        if (const auto m = word.match(t); m.hasMatch()) {
            const QString w = m.captured(1).toLower();
            t = (w == QLatin1String("cpu") ? QStringLiteral("CPU") : w == QLatin1String("xid") ? QStringLiteral("Xid") : QStringLiteral("bank"))
                + QLatin1Char(' ') + m.captured(2);
        } else {
            t = t.toLower();
        }
        if (!out.contains(t)) out << t;
    }
    return out;
}

/// The text each report view filters by for a focus: the token itself, except that a PCI address with a
/// domain also matches reports that print it without one ("0000:01:00.0" → "01:00.0").
inline QString filterFor(const QString &focus) {
    static const QRegularExpression domain(QStringLiteral("^[0-9a-f]{4}:([0-9a-f]{2}:[0-9a-f]{2}\\.[0-7])$"));
    if (const auto m = domain.match(focus.trimmed().toLower()); m.hasMatch()) return m.captured(1);
    return focus.trimmed();
}

} // namespace incidentfocus
