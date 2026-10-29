#include "gpustress.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>

namespace gpustress {

QString modeName(Mode m) { return m == Mode::Vram ? QStringLiteral("vram") : QStringLiteral("core"); }

QList<Device> parseDevices(const QByteArray &listJson) {
    QList<Device> out;
    for (const QJsonValue &v : QJsonDocument::fromJson(listJson).array()) {
        const QJsonObject o = v.toObject();
        const QString type = o.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("cpu") || o.value(QStringLiteral("pci")).toString().isEmpty()) continue;
        out.append({o.value(QStringLiteral("pci")).toString(), o.value(QStringLiteral("name")).toString(), type,
                    o.value(QStringLiteral("vram_mib")).toInteger()});
    }
    return out;
}

int preselect(const QList<Device> &devices) {
    for (qsizetype i = 0; i < devices.size(); ++i)
        if (devices.at(i).type == QLatin1String("discrete")) return int(i);
    return 0;
}

Plan plan(Mode m, const QString &pci, int percent, int seconds) {
    Plan p;
    if (pci.isEmpty()) { p.error = QStringLiteral("device"); return p; }
    if (m == Mode::Vram && (percent < MIN_PERCENT || percent > MAX_PERCENT)) { p.error = QStringLiteral("percent"); return p; }
    if (seconds < 1) { p.error = QStringLiteral("seconds"); return p; }
    p.args = {modeName(m), QStringLiteral("--device"), pci};
    if (m == Mode::Vram) p.args << QStringLiteral("--percent") << QString::number(percent);
    p.args << QStringLiteral("--seconds") << QString::number(seconds);
    return p;
}

void feed(Run &r, const QString &line) {
    const QJsonObject o = QJsonDocument::fromJson(line.toUtf8()).object();
    const QString ev = o.value(QStringLiteral("ev")).toString();
    if (ev == QLatin1String("start")) {
        r.bytes = o.value(QStringLiteral("bytes")).toInteger();
        r.name = o.value(QStringLiteral("name")).toString();
    } else if (ev == QLatin1String("progress") || ev == QLatin1String("end")) {
        // progress / end carry the full count; the error lines are capped (16 a pass, 200 a run).
        r.errors = std::max<qint64>(r.errors, o.value(QStringLiteral("errors")).toInteger());
        r.passes = std::max<qint64>(r.passes, o.value(QStringLiteral("passes")).toInteger());
        if (ev == QLatin1String("end")) {
            r.ended = true;
            r.status = o.value(QStringLiteral("status")).toString();
            r.msg = o.value(QStringLiteral("msg")).toString();
            if (r.status == QLatin1String("device_lost")) r.deviceLost = true;
        }
    } else if (ev == QLatin1String("error")) {
        r.errors = std::max(r.errors, ++r.errorLines);
    }
}

bool isGpuFault(const QString &kind, int code) {
    return (kind == QLatin1String("xid") && code != 45 && code != 154) || kind == QLatin1String("amdgpu");
}

Verdict verdict(const Run &r, int exitCode, bool crashed, bool cancelled, int gpuFaults) {
    // An error found is real however the run ended.
    if (r.errors > 0 || r.deviceLost || gpuFaults > 0) return Verdict::Fail;
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

QString stateFile() {
    QString dir = qEnvironmentVariable("XDG_STATE_HOME");
    if (dir.isEmpty()) dir = QDir::homePath() + QStringLiteral("/.local/state");
    return dir + QStringLiteral("/centurion/gpustress.json");
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

} // namespace gpustress
