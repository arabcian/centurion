#include "smartwatch.h"
#include "smarttrend.h"
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QLockFile>
#include <QDateTime>
#include <QSettings>
#include <cmath>

namespace {
constexpr int MAX_DAYS = 730;
QString historyFile() {
    QString dir = qEnvironmentVariable("XDG_STATE_HOME");
    if (dir.isEmpty()) dir = QDir::homePath() + QStringLiteral("/.local/state");
    return dir + QStringLiteral("/centurion/smart.json");
}

QString todayIso() { return QDate::currentDate().toString(Qt::ISODate); }

// Each alert fires when its condition starts, not on every check while it lasts.
QStringList judge(const QString &name, const QJsonObject &prev, const QJsonObject &cur) {
    QStringList out;
    const auto crit = smart::counter(cur, "critical_warning");
    if (crit && *crit != 0 && smart::counter(prev, "critical_warning") != crit)
        out << QStringLiteral("%1: critical warning %2").arg(name, QString::number(qlonglong(*crit)));

    const auto delta = smarttrend::increase(smart::counter(prev, "media_errors"), smart::counter(cur, "media_errors"));
    if (delta && *delta > 0)
        out << QStringLiteral("%1: media errors up by %2").arg(name, QString::number(qlonglong(*delta)));

    auto low = [](const QJsonObject &o) {
        const auto spare = smart::field(o, "spare_pct"), threshold = smart::field(o, "spare_threshold_pct");
        return spare && threshold && *spare < *threshold;
    };
    if (low(cur) && !low(prev))
        out << QStringLiteral("%1: spare capacity %2 % is below its threshold")
                   .arg(name, QString::number(smart::field(cur, "spare_pct").value_or(0)));
    return out;
}

} // namespace

namespace smart {

std::optional<double> field(const QJsonObject &o, const char *key) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isDouble()) return std::nullopt;
    const double n=v.toDouble();
    return std::isfinite(n)?std::optional<double>(n):std::nullopt;
}

std::optional<std::int64_t> counter(const QJsonObject &o, const char *key) {
    const auto v = o.value(QLatin1String(key));
    // QJsonValue::toInteger rejects fractions and out-of-range numbers in Qt6.
    if (!v.isDouble()) return {};
    const auto n = v.toInteger(-1);
    if (n < 0) return {};
    return std::int64_t(n);
}
bool automaticSamplingEnabled() { return QSettings().value(QStringLiteral("health/smartAutomaticSampling"), false).toBool(); }
void setAutomaticSamplingEnabled(bool enabled) { QSettings().setValue(QStringLiteral("health/smartAutomaticSampling"), enabled); }

bool usableIdentity(const QString &id) {
    // Low 48 bits of the FNV offset basis == empty serial hash from old helpers.
    if(id.isEmpty() || id.size()>512 || id.endsWith(QStringLiteral("|9ce484222325"))) return false;
    const auto hash=id.section(QLatin1Char('|'),-1), capacity=id.section(QLatin1Char('|'),-2,-2), model=id.section(QLatin1Char('|'),0,-3);
    bool ok=false;const auto cap=capacity.toULongLong(&ok);
    if(!ok || cap==0 || model.trimmed().isEmpty() || hash.size()!=12) return false;
    for(const auto ch:hash) if(!((ch>=QLatin1Char('0')&&ch<=QLatin1Char('9'))||(ch>=QLatin1Char('a')&&ch<=QLatin1Char('f')))) return false;
    return true;
}
static std::optional<QJsonObject> readHistory() {
    QFile f(historyFile());
    if(!f.exists()) return QJsonObject{};
    if(!f.open(QIODevice::ReadOnly) || f.size()>16*1024*1024) return std::nullopt;
    const auto bytes=f.read(16*1024*1024+1); if(bytes.size()>16*1024*1024 || !f.atEnd()) return std::nullopt;
    QJsonParseError error; const auto doc=QJsonDocument::fromJson(bytes,&error);
    if(error.error!=QJsonParseError::NoError || !doc.isObject() || !doc.object().value("disks").isObject()) return std::nullopt;
    const auto disks=doc.object().value("disks").toObject();
    if(disks.size()>512) return std::nullopt;
    for(auto it=disks.constBegin();it!=disks.constEnd();++it) {
        const auto disk=it.value().toObject();const auto samples=disk.value("samples").toArray();
        if(!disk.value("samples").isArray() || samples.size()>MAX_DAYS) return std::nullopt;
        QString previous;
        for(const auto &v:samples) {
            const auto date=v.toObject().value("d").toString();
            if(!QDate::fromString(date,Qt::ISODate).isValid() || (!previous.isEmpty() && date<=previous)) return std::nullopt;
            previous=date;
        }
    }
    return doc.object();
}
std::optional<QJsonObject> historySnapshot() { return readHistory(); }
QJsonObject loadHistory() { return historySnapshot().value_or(QJsonObject{}); }

RecordResult record(const QJsonArray &disks) {
    if (disks.isEmpty()) return {RecordStatus::NoUsableSamples, {}};
    const QString today = todayIso();
    if(!QDir().mkpath(QFileInfo(historyFile()).absolutePath())) return {};
    QLockFile lock(historyFile()+QStringLiteral(".lock")); if(!lock.tryLock(0)) return {};
    const auto stored=readHistory(); if(!stored) return {}; // never overwrite unreadable history
    QJsonObject hist=*stored;
    QSet<QString> seen,duplicate;
    for(const auto &v:disks) {
        const auto id=v.toObject().value("id").toString();
        if(seen.contains(id)) duplicate.insert(id);
        seen.insert(id);
    }
    QJsonObject all = hist.value(QStringLiteral("disks")).toObject();
    QStringList alerts;
    qsizetype accepted = 0;
    for (const QJsonValue &v : disks) {
        const QJsonObject d = v.toObject();
        const QString id = d.value(QStringLiteral("id")).toString();
        if (!usableIdentity(id) || duplicate.contains(id) || d.contains(QStringLiteral("error"))) continue;
        const QString model = d.value(QStringLiteral("model")).toString();

        QJsonObject disk = all.value(id).toObject();
        QJsonArray samples = disk.value(QStringLiteral("samples")).toArray();
        if(!samples.isEmpty() && samples.last().toObject().value("d").toString()>today) continue;
        // Only known health fields are persisted. An identity-only or error
        // response is not a successful daily measurement.
        QJsonObject s;
        for (const char *key : {"temp_c", "life_used_pct", "spare_pct", "spare_threshold_pct", "written_tb"}) {
            if (field(d, key)) s.insert(QLatin1String(key), d.value(QLatin1String(key)));
        }
        for (const char *key : {"media_errors", "unsafe_shutdowns", "power_on_hours", "critical_warning"}) {
            if (counter(d, key)) s.insert(QLatin1String(key), d.value(QLatin1String(key)));
        }
        if (d.value("passed").isBool()) s.insert("passed", d.value("passed"));
        if (s.isEmpty()) continue;
        ++accepted;
        // Latest observation, including today's earlier check, avoids replaying
        // a warning every time the page is opened on the same day.
        const QJsonObject prev = samples.isEmpty() ? QJsonObject{} : samples.last().toObject();
        alerts += judge(model.isEmpty() ? d.value(QStringLiteral("dev")).toString() : model, prev, s);
        s[QStringLiteral("d")] = today;
        s[QStringLiteral("captured_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        if (!samples.isEmpty() && samples.last().toObject().value(QStringLiteral("d")).toString() == today) samples.removeLast();
        samples.append(s);
        while (samples.size() > MAX_DAYS) samples.removeFirst();
        disk[QStringLiteral("model")] = model;
        disk[QStringLiteral("samples")] = samples;
        all[id] = disk;
    }
    if (accepted == 0) return {RecordStatus::NoUsableSamples, {}};
    // A disk not seen for MAX_DAYS (a USB stick, a replaced drive) is dropped whole, so the file stays bounded.
    const QString oldest = QDate::currentDate().addDays(-MAX_DAYS).toString(Qt::ISODate);
    for (auto it = all.begin(); it != all.end();) {
        const QJsonArray samples = it.value().toObject().value(QStringLiteral("samples")).toArray();
        if (samples.isEmpty() || samples.last().toObject().value(QStringLiteral("d")).toString() < oldest) it = all.erase(it);
        else ++it;
    }
    hist[QStringLiteral("disks")] = all;
    const bool complete = accepted == disks.size();
    if (complete) hist[QStringLiteral("sampled")] = today;
    const auto bytes=QJsonDocument(hist).toJson(QJsonDocument::Compact);
    if(all.size()>512 || bytes.size()>16*1024*1024) return {RecordStatus::Unavailable, alerts};
    QSaveFile f(historyFile()); f.setDirectWriteFallback(false);
    if(!f.open(QIODevice::WriteOnly) || !f.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner) || f.write(bytes)!=bytes.size() || !f.commit())
        return {RecordStatus::Unavailable, alerts};
    return {complete ? RecordStatus::Saved : RecordStatus::Partial, alerts};
}

} // namespace smart

