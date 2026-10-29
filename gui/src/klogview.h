#pragma once
// Health → Kernel log: the pure part of the view. Parses the saved lines (klogpage.cpp persist),
// collapses repeats, and tells whether a message was seen in an earlier boot. No widgets here;
// tests/klogview_test.cpp covers it.
//
// Saved line: "<local time>  boot=<8 chars> seq=<n>  <LEVEL>  [<s>.<us>] <message>".
#include <QHash>
#include <QString>
#include <optional>
#include <vector>

namespace klogview {

struct Line {
    QString text;       // the saved line, trimmed
    QString time;       // "yyyy-MM-dd HH:mm:ss"
    QString boot;       // boot tag, 8 characters
    quint64 seq = 0;
    int level = 4;      // 0 EMERG … 4 WARN (see levelIndex)
    QString msg;
    QString key;        // normalize(msg): the message with every digit run as '#'
};

/// Index of a kernel level name (EMERG 0 … WARN 4), -1 when unknown.
int levelIndex(const QString &name);

std::optional<Line> parse(const QString &saved);

/// A message with its numbers (counters, addresses, process ids) replaced by '#'. Repeats of one
/// message differ only in those, so they share a key.
QString normalize(const QString &msg);

struct Row {
    Line first;         // the first occurrence: what is shown
    int count = 1;      // occurrences in the input
    QString last;       // time of the last occurrence
};

/// One row per key when `groupRepeats`, otherwise one per line. Rows keep the order of first occurrence.
std::vector<Row> group(const std::vector<Line> &lines, bool groupRepeats);

/// Key → boot tag of its first occurrence. `all` must be in file (time) order and unfiltered, so
/// "first seen in this boot" is judged on the whole saved log.
QHash<QString, QString> firstBoots(const std::vector<Line> &all);

}  // namespace klogview
