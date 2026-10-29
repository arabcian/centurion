#include "memtest.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>

namespace memtest {

QString toolName(Tool t) {
    switch (t) {
    case Tool::Stressapptest: return QStringLiteral("stressapptest");
    case Tool::Memtester: return QStringLiteral("memtester");
    case Tool::None: break;
    }
    return {};
}

qint64 availableKib(const QByteArray &meminfo) {
    static const QRegularExpression re(QStringLiteral("^MemAvailable:\\s+(\\d+) kB"), QRegularExpression::MultilineOption);
    const auto m = re.match(QString::fromLatin1(meminfo));
    return m.hasMatch() ? m.captured(1).toLongLong() : -1;
}

Plan plan(Tool t, qint64 availKib, int percent, int seconds, qint64 memlockBytes) {
    Plan p;
    p.mib = availKib > 0 ? availKib * percent / 100 / 1024 : 0;
    if (t == Tool::Memtester && memlockBytes >= 0 && memlockBytes / (1024 * 1024) < p.mib) {
        p.mib = memlockBytes / (1024 * 1024);
        p.clamped = true;
    }
    if (t == Tool::None) { p.error = QStringLiteral("no tool"); return p; }
    if (p.mib < MIN_MIB) { p.error = p.clamped ? QStringLiteral("memlock") : QStringLiteral("memory"); return p; }
    if (t == Tool::Stressapptest)
        p.args = {QStringLiteral("-M"), QString::number(p.mib), QStringLiteral("-s"), QString::number(seconds)};
    else
        p.args = {QString::number(p.mib) + QLatin1Char('M')};  // no loop count: runs until stopped at the deadline
    return p;
}

void feed(Run &r, Tool t, const QString &line) {
    if (t == Tool::Memtester) {
        if (line.contains(QLatin1String("FAILURE"))) ++r.errors;
        if (line.contains(QLatin1String("unlocked"))) r.unlocked = true;
        return;
    }
    if (line.contains(QLatin1String("Hardware Error:"))) ++r.errors;
    static const QRegularExpression stats(QStringLiteral("Found (\\d+) hardware incidents"));
    if (const auto m = stats.match(line); m.hasMatch()) r.errors = std::max(r.errors, m.captured(1).toInt());
    if (line.contains(QLatin1String("Status: PASS"))) r.statusPass = true;
    if (line.contains(QLatin1String("Status: FAIL"))) r.statusFail = true;
}

Verdict verdict(const Run &r, Tool t, int exitCode, bool crashed, bool stopped, bool cancelled, int mce) {
    // An error seen is real however the run ended.
    if (r.errors > 0 || r.statusFail || mce > 0) return Verdict::Fail;
    if (t == Tool::Memtester && !crashed && (exitCode & 0x06)) return Verdict::Fail;  // address-line / test failure bits
    if (cancelled) return Verdict::Incomplete;
    if (t == Tool::Stressapptest) return !crashed && exitCode == 0 && r.statusPass ? Verdict::Pass : Verdict::Incomplete;
    return stopped || (!crashed && exitCode == 0) ? Verdict::Pass : Verdict::Incomplete;
}

QString verdictName(Verdict v) {
    switch (v) {
    case Verdict::Pass: return QStringLiteral("pass");
    case Verdict::Fail: return QStringLiteral("fail");
    case Verdict::Incomplete: break;
    }
    return QStringLiteral("incomplete");
}

QString stateFile() {
    QString dir = qEnvironmentVariable("XDG_STATE_HOME");
    if (dir.isEmpty()) dir = QDir::homePath() + QStringLiteral("/.local/state");
    return dir + QStringLiteral("/centurion/memtest.json");
}

QJsonArray load() {
    QFile f(stateFile());
    if (!f.open(QIODevice::ReadOnly) || f.size() > 1024 * 1024) return {};
    return QJsonDocument::fromJson(f.readAll()).array();
}

bool append(const QJsonObject &result) {
    QJsonArray all = load();
    all.append(result);
    while (all.size() > KEEP) all.removeFirst();
    QDir().mkpath(QFileInfo(stateFile()).absolutePath());
    QSaveFile f(stateFile());
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(all).toJson(QJsonDocument::Compact));
    return f.commit();
}

} // namespace memtest
