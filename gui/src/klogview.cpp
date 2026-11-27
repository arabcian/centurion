#include "klogview.h"

#include <QRegularExpression>
#include <QDate>
#include <QTime>
#include <QStringList>
#include <algorithm>
#include <utility>

namespace klogview {

static const QRegularExpression SAVED(QStringLiteral(R"(^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\s+boot=(\S+)\s+seq=(\d+)\s+(\S+)\s+\[[ \t]*\d+(?:\.\d{1,6})?\]\s?(.*)$)"));
static const QRegularExpression DIGITS(QStringLiteral(R"(\d+)"));

bool fullBootId(const QString &id) {
    static const QRegularExpression uuid(QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
    return uuid.match(id).hasMatch();
}
static bool legacyBootId(const QString &id) {
    static const QRegularExpression prefix(QStringLiteral("^[0-9a-fA-F]{8}$"));
    return prefix.match(id).hasMatch();
}

bool Filter::valid() const {
    const auto dateValid=[](const QString &s) { return s.isEmpty() || (s.size()==10 && QDate::fromString(s,Qt::ISODate).isValid()); };
    return (boot.isEmpty() || fullBootId(boot) || legacyBootId(boot)) && dateValid(fromDate) && dateValid(toDate)
        && (fromDate.isEmpty() || toDate.isEmpty() || fromDate<=toDate) && code>=-1;
}
QRegularExpression Filter::devicePattern() const {
    return QRegularExpression(QStringLiteral("(?<![A-Za-z0-9_])%1(?![A-Za-z0-9_])").arg(QRegularExpression::escape(device)),QRegularExpression::CaseInsensitiveOption);
}
// Caller validates date and identity once before scanning.
bool Filter::matches(const Line &line,const QString &eventKind,int eventCode,const QRegularExpression &token) const {
    if (!boot.isEmpty() && line.boot.compare(boot,Qt::CaseInsensitive)!=0) return false;
    if (!kind.isEmpty() && (kind==QLatin1String("unclassified") ? !eventKind.isEmpty() : eventKind!=kind)) return false;
    if (code>=0 && code!=eventCode) return false;
    if (!device.isEmpty()) {
        // A literal token in the message, not an inferred hardware identity.
        // Token boundaries prevent nvme1 from matching nvme10.
        if (!token.match(line.msg).hasMatch()) return false;
    }
    const auto date=line.time.left(10);
    return (fromDate.isEmpty() || date>=fromDate) && (toDate.isEmpty() || date<=toDate);
}

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
    if (!QDate::fromString(l.time.left(10), Qt::ISODate).isValid()
        || !QTime::fromString(l.time.mid(11), QStringLiteral("HH:mm:ss")).isValid()) return std::nullopt;
    l.boot = m.captured(2).toLower();
    if (!fullBootId(l.boot) && !legacyBootId(l.boot)) return std::nullopt;
    bool seqOk=false;
    l.seq = m.captured(3).toULongLong(&seqOk);
    if (!seqOk) return std::nullopt;
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

QString stem(const QString &msg) {
    QString best, run;
    auto close = [&] { if (run.size() > best.size()) best = run; run.clear(); };
    for (const QString &w : msg.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        bool clean = !w.contains(QLatin1Char('|'));
        for (const QChar c : w) if (c.isDigit()) { clean = false; break; }
        if (!clean) { close(); continue; }
        run += run.isEmpty() ? w : QLatin1Char(' ') + w;
    }
    close();
    return best.size() >= 6 ? best : QString();
}

ContextWindow::ContextWindow(QString boot, quint64 seq, int before, int after)
    : boot_(std::move(boot)), seq_(seq), before_(std::max(0, before)), after_(std::max(0, after)) {}

void ContextWindow::append(const Line &line) {
    if (line.boot != boot_) return;
    if (std::any_of(lines_.cbegin(), lines_.cend(), [&](const Line &saved) {
        return saved.seq == line.seq && saved.text == line.text;
    })) return;
    if (mark_ >= 0) {
        if (int(lines_.size()) - mark_ - 1 < after_ && line.seq != seq_) lines_.push_back(line);
        return;
    }
    if (line.seq == seq_) {
        mark_ = int(lines_.size());
        lines_.push_back(line);
    } else {
        lines_.push_back(line);
        if (int(lines_.size()) > before_) lines_.pop_front();
    }
}

std::vector<Row> ContextWindow::rows() const {
    std::vector<Row> result;
    if (mark_ >= 0) for (const Line &l : lines_) result.push_back(Row{l, 1, l.time});
    return result;
}

std::vector<Row> group(const std::deque<Line> &lines, bool groupRepeats) {
    std::vector<Row> rows;
    QHash<QString, size_t> at;  // key → its row
    for (const Line &l : lines) {
        if (groupRepeats) {
            const auto identity = l.boot + QLatin1Char('/') + l.key;
            if (const auto it = at.constFind(identity); it != at.constEnd()) {
                Row &r = rows[it.value()];
                ++r.count;
                r.last = l.time;
                continue;
            }
            at.insert(identity, rows.size());
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
