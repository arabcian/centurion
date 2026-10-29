#include "klogview.h"

#include <QRegularExpression>
#include <QStringList>

namespace klogview {

static const QRegularExpression SAVED(QStringLiteral(R"(^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\s+boot=(\S+)\s+seq=(\d+)\s+(\S+)\s+\[[^\]]*\]\s?(.*)$)"));
static const QRegularExpression DIGITS(QStringLiteral(R"(\d+)"));

int levelIndex(const QString &name) {
    static const QStringList names{QStringLiteral("EMERG"), QStringLiteral("ALERT"), QStringLiteral("CRIT"),
                                   QStringLiteral("ERR"), QStringLiteral("WARN")};
    return int(names.indexOf(name));
}

std::optional<Line> parse(const QString &saved) {
    const QString text = saved.trimmed();
    const QRegularExpressionMatch m = SAVED.match(text);
    if (!m.hasMatch()) return std::nullopt;
    const int level = levelIndex(m.captured(4));
    if (level < 0) return std::nullopt;
    Line l;
    l.text = text;
    l.time = m.captured(1);
    l.boot = m.captured(2);
    l.seq = m.captured(3).toULongLong();
    l.level = level;
    l.msg = m.captured(5);
    l.key = normalize(l.msg);
    return l;
}

QString normalize(const QString &msg) {
    QString s = msg.trimmed();
    s.replace(DIGITS, QStringLiteral("#"));
    return s;
}

std::vector<Row> group(const std::vector<Line> &lines, bool groupRepeats) {
    std::vector<Row> rows;
    QHash<QString, size_t> at;  // key → its row
    for (const Line &l : lines) {
        if (groupRepeats) {
            if (const auto it = at.constFind(l.key); it != at.constEnd()) {
                Row &r = rows[it.value()];
                ++r.count;
                r.last = l.time;
                continue;
            }
            at.insert(l.key, rows.size());
        }
        rows.push_back(Row{l, 1, l.time});
    }
    return rows;
}

QHash<QString, QString> firstBoots(const std::vector<Line> &all) {
    QHash<QString, QString> first;
    for (const Line &l : all) {
        if (!first.contains(l.key)) first.insert(l.key, l.boot);
    }
    return first;
}

}  // namespace klogview
