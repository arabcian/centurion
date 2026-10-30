#include "cpustress.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>

namespace cpustress {

QString modeName(Mode m) { return m == Mode::PerCore ? QStringLiteral("percore") : QStringLiteral("allcore"); }

static QList<int> ints(const QJsonValue &v) {
    QList<int> out;
    for (const QJsonValue &x : v.toArray()) out.append(x.toInt());
    return out;
}

Info parseInfo(const QByteArray &infoJson) {
    const QJsonObject o = QJsonDocument::fromJson(infoJson).object();
    Info info;
    for (const QJsonValue &c : o.value(QStringLiteral("cores")).toArray()) {
        const QList<int> cpus = ints(c.toObject().value(QStringLiteral("cpus")));
        if (!cpus.isEmpty()) info.cores.append(cpus);
    }
    const QJsonObject loads = o.value(QStringLiteral("loads")).toObject();
    for (const char *l : {"sse", "avx2", "avx512"})
        if (loads.value(QLatin1String(l)).toBool()) info.loads.append(QLatin1String(l));
    return info;
}

Plan plan(Mode m, const QString &load, int seconds, bool transitions, int cycles) {
    Plan p;
    if (!QStringList{QStringLiteral("sse"), QStringLiteral("avx2"), QStringLiteral("avx512")}.contains(load)) { p.error = QStringLiteral("load"); return p; }
    if (seconds < 1 || (m == Mode::PerCore && (cycles < 1 || cycles > MAX_CYCLES))) { p.error = QStringLiteral("seconds"); return p; }
    p.args = {modeName(m), QStringLiteral("--load"), load, QStringLiteral("--seconds"), QString::number(seconds)};
    if (m == Mode::PerCore && cycles > 1) p.args << QStringLiteral("--cycles") << QString::number(cycles);
    if (transitions) p.args << QStringLiteral("--transitions");
    return p;
}

bool feed(Run &r, const QString &line) {
    const QJsonObject o = QJsonDocument::fromJson(line.toUtf8()).object();
    const QString ev = o.value(QStringLiteral("ev")).toString();
    if (ev == QLatin1String("start")) {
        r.cpus = ints(o.value(QStringLiteral("cpus")));
    } else if (ev == QLatin1String("core")) {
        r.cpu = o.value(QStringLiteral("cpu")).toInt(-1);
        return true;
    } else if (ev == QLatin1String("core_end")) {
        r.done.append(o.value(QStringLiteral("cpu")).toInt(-1));
    } else if (ev == QLatin1String("progress") || ev == QLatin1String("end")) {
        // progress / end carry the full count; the error lines are capped (200 a run).
        r.errors = std::max<qint64>(r.errors, o.value(QStringLiteral("errors")).toInteger());
        r.chunks = std::max<qint64>(r.chunks, o.value(QStringLiteral("chunks")).toInteger());
        if (ev == QLatin1String("end")) {
            r.ended = true;
            r.status = o.value(QStringLiteral("status")).toString();
            r.msg = o.value(QStringLiteral("msg")).toString();
            for (int c : ints(o.value(QStringLiteral("failed")))) if (!r.failed.contains(c)) r.failed.append(c);
        }
    } else if (ev == QLatin1String("error")) {
        r.errors = std::max(r.errors, ++r.errorLines);
        const int c = o.value(QStringLiteral("cpu")).toInt(-1);
        if (c >= 0 && !r.failed.contains(c)) r.failed.append(c);
    }
    return false;
}

bool isCpuFault(const QString &kind) { return kind == QLatin1String("mce") || kind == QLatin1String("lockup"); }

int eventCpu(const QString &text) {
    static const QRegularExpression re(QStringLiteral(R"(\bCPU ?#?(\d{1,4})\b)"));
    const QRegularExpressionMatch m = re.match(text);
    return m.hasMatch() ? m.captured(1).toInt() : -1;
}

Verdict verdict(const Run &r, int exitCode, bool crashed, bool cancelled, int cpuFaults) {
    // An error found is real however the run ended.
    if (r.errors > 0 || cpuFaults > 0) return Verdict::Fail;
    if (cancelled || crashed || !r.ended) return Verdict::Incomplete;
    return r.status == QLatin1String("pass") && exitCode == 0 ? Verdict::Pass : Verdict::Incomplete;
}

QString verdictName(Verdict v) {
    switch (v) {
    case Verdict::Pass: return QStringLiteral("pass");
    case Verdict::Fail: return QStringLiteral("fail");
    case Verdict::Incomplete: break;
    }
    return QStringLiteral("incomplete");
}

QString profileHash(const QByteArray &profileFile) {
    return QString::fromLatin1(QCryptographicHash::hash(profileFile, QCryptographicHash::Sha1).toHex().left(16));
}

QJsonObject verification(const QJsonArray &all, const QString &profile, const QString &hash) {
    const QString curve = QStringLiteral("profile:") + profile;
    for (qsizetype i = all.size() - 1; i >= 0; --i) {
        const QJsonObject o = all.at(i).toObject();
        const QString v = o.value(QStringLiteral("verdict")).toString();
        if (o.value(QStringLiteral("curve")).toString() != curve || o.value(QStringLiteral("profile_hash")).toString() != hash) continue;
        if (v == QLatin1String("pass") || v == QLatin1String("fail") || v == QLatin1String("crashed")) return o;
    }
    return {};
}

QString stateFile() {
    QString dir = qEnvironmentVariable("XDG_STATE_HOME");
    if (dir.isEmpty()) dir = QDir::homePath() + QStringLiteral("/.local/state");
    return dir + QStringLiteral("/centurion/cpustress.json");
}

QJsonArray load() {
    QFile f(stateFile());
    if (!f.open(QIODevice::ReadOnly) || f.size() > 1024 * 1024) return {};
    return QJsonDocument::fromJson(f.readAll()).array();
}

static bool save(const QJsonArray &all) {
    QDir().mkpath(QFileInfo(stateFile()).absolutePath());
    QSaveFile f(stateFile());
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(all).toJson(QJsonDocument::Compact));
    return f.commit();
}

bool store(const QJsonObject &result) {
    QJsonArray all = load();
    const QJsonObject last = all.isEmpty() ? QJsonObject() : all.last().toObject();
    if (last.value(QStringLiteral("verdict")).toString() == QLatin1String("running")
        && last.value(QStringLiteral("id")) == result.value(QStringLiteral("id")))
        all.removeLast();
    all.append(result);
    while (all.size() > KEEP) all.removeFirst();
    return save(all);
}

QJsonArray settle(const QString &bootId) {
    QJsonArray all = load(), crashed;
    bool changed = false;
    for (qsizetype i = 0; i < all.size(); ++i) {
        QJsonObject o = all.at(i).toObject();
        if (o.value(QStringLiteral("verdict")).toString() != QLatin1String("running")) continue;
        const bool other = o.value(QStringLiteral("boot")).toString() != bootId;
        o[QStringLiteral("verdict")] = other ? QStringLiteral("crashed") : QStringLiteral("incomplete");
        if (other) crashed.append(o);
        all[i] = o;
        changed = true;
    }
    if (changed) save(all);
    return crashed;
}

} // namespace cpustress
