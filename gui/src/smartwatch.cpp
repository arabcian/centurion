#include "smartwatch.h"

#include "privileged.h"

#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTimer>
#include <QSet>
#include <QLockFile>
#include <QDateTime>
#include <cmath>

#include <grp.h>
#include <unistd.h>
#include <algorithm>
#include <vector>

namespace {

constexpr int MAX_DAYS = 730;
constexpr int CHECK_EVERY_MS = 60 * 60 * 1000;
constexpr int FIRST_CHECK_MS = 60 * 1000;  // not in the middle of start-up

QString historyFile() {
    QString dir = qEnvironmentVariable("XDG_STATE_HOME");
    if (dir.isEmpty()) dir = QDir::homePath() + QStringLiteral("/.local/state");
    return dir + QStringLiteral("/centurion/smart.json");
}

QString todayIso() { return QDate::currentDate().toString(Qt::ISODate); }

// Only a wheel/sudo admin gets the helper without a password, so only they get the unattended sample.
bool adminAtMachine() {
    std::vector<gid_t> gids(static_cast<size_t>(std::max(getgroups(0, nullptr), 0)));
    // Only the entries getgroups filled: on failure (-1) none, never zero-initialised gids.
    gids.resize(static_cast<size_t>(std::max(getgroups(static_cast<int>(gids.size()), gids.data()), 0)));
    gids.push_back(getgid());
    for (const char *name : {"wheel", "sudo"}) {
        const group *g = getgrnam(name);
        if (g && std::find(gids.begin(), gids.end(), g->gr_gid) != gids.end()) return true;
    }
    return false;
}

// Each alert fires when its condition starts, not on every check while it lasts.
QStringList judge(const QString &name, const QJsonObject &prev, const QJsonObject &cur) {
    QStringList out;
    const auto crit = smart::field(cur, "critical_warning");
    if (crit && *crit != 0 && smart::field(prev, "critical_warning").value_or(0) != *crit)
        out << QStringLiteral("%1: critical warning %2").arg(name, QString::number(*crit));

    const auto media = smart::field(cur, "media_errors"), mediaPrev = smart::field(prev, "media_errors");
    if (media && mediaPrev && *media > *mediaPrev)
        out << QStringLiteral("%1: media errors up by %2").arg(name, QString::number(*media - *mediaPrev));

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
    for(auto it=disks.cbegin();it!=disks.cend();++it) {
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

QStringList record(const QJsonArray &disks) {
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
    for (const QJsonValue &v : disks) {
        const QJsonObject d = v.toObject();
        const QString id = d.value(QStringLiteral("id")).toString();
        if (!usableIdentity(id) || duplicate.contains(id) || d.contains(QStringLiteral("error"))) continue;
        const QString model = d.value(QStringLiteral("model")).toString();

        QJsonObject disk = all.value(id).toObject();
        QJsonArray samples = disk.value(QStringLiteral("samples")).toArray();
        if(!samples.isEmpty() && samples.last().toObject().value("d").toString()>today) continue;
        // Baseline: the latest sample from an earlier day.
        QJsonObject prev;
        for (const QJsonValue &s : samples)
            if (s.toObject().value(QStringLiteral("d")).toString() != today) prev = s.toObject();
        alerts += judge(model.isEmpty() ? d.value(QStringLiteral("dev")).toString() : model, prev, d);

        QJsonObject s = d;
        for (const char *k : {"id", "dev", "model", "capacity_bytes"}) s.remove(QLatin1String(k));
        s[QStringLiteral("d")] = today;
        s[QStringLiteral("captured_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        if (!samples.isEmpty() && samples.last().toObject().value(QStringLiteral("d")).toString() == today) samples.removeLast();
        samples.append(s);
        while (samples.size() > MAX_DAYS) samples.removeFirst();
        disk[QStringLiteral("model")] = model;
        disk[QStringLiteral("samples")] = samples;
        all[id] = disk;
    }
    // A disk not seen for MAX_DAYS (a USB stick, a replaced drive) is dropped whole, so the file stays bounded.
    const QString oldest = QDate::currentDate().addDays(-MAX_DAYS).toString(Qt::ISODate);
    for (auto it = all.begin(); it != all.end();) {
        const QJsonArray samples = it->toObject().value(QStringLiteral("samples")).toArray();
        if (samples.isEmpty() || samples.last().toObject().value(QStringLiteral("d")).toString() < oldest) it = all.erase(it);
        else ++it;
    }
    hist[QStringLiteral("disks")] = all;
    hist[QStringLiteral("sampled")] = today;
    const auto bytes=QJsonDocument(hist).toJson(QJsonDocument::Compact);
    if(all.size()>512 || bytes.size()>16*1024*1024) return alerts;
    QSaveFile f(historyFile()); f.setDirectWriteFallback(false);
    if(f.open(QIODevice::WriteOnly) && f.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner) && f.write(bytes)==bytes.size()) f.commit();
    return alerts;
}

} // namespace smart

SmartWatch::SmartWatch(QObject *parent) : QObject(parent), timer_(new QTimer(this)) {
    timer_->setInterval(CHECK_EVERY_MS);
    connect(timer_, &QTimer::timeout, this, &SmartWatch::check);
    timer_->start();
    QTimer::singleShot(FIRST_CHECK_MS, this, &SmartWatch::check);
}

void SmartWatch::check() {
    if (busy_ || !adminAtMachine()) return;
    if (smart::loadHistory().value(QStringLiteral("sampled")).toString() == todayIso()) return;
    busy_ = true;
    privileged::run(privileged::helperPath(QStringLiteral("tune-helper")),
                    QJsonObject{{"op", "tool"}, {"tool", "smart_json"}}, this, [this](const privileged::Result &r) {
        busy_ = false;
        if (!r.ok()) return;
        const QStringList alerts = smart::record(r.json.value(QStringLiteral("disks")).toArray());
        if (!alerts.isEmpty()) Q_EMIT alert(alerts);
    }, 120000);
}
