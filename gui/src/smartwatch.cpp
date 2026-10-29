#include "smartwatch.h"

#include "privileged.h"

#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTimer>

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
    getgroups(static_cast<int>(gids.size()), gids.data());
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
    return v.toDouble();
}

QJsonObject loadHistory() {
    QFile f(historyFile());
    return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject{};
}

QStringList record(const QJsonArray &disks) {
    const QString today = todayIso();
    QJsonObject hist = loadHistory();
    QJsonObject all = hist.value(QStringLiteral("disks")).toObject();
    QStringList alerts;
    for (const QJsonValue &v : disks) {
        const QJsonObject d = v.toObject();
        const QString id = d.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || d.contains(QStringLiteral("error"))) continue;
        const QString model = d.value(QStringLiteral("model")).toString();

        QJsonObject disk = all.value(id).toObject();
        QJsonArray samples = disk.value(QStringLiteral("samples")).toArray();
        // Baseline: the latest sample from an earlier day.
        QJsonObject prev;
        for (const QJsonValue &s : samples)
            if (s.toObject().value(QStringLiteral("d")).toString() != today) prev = s.toObject();
        alerts += judge(model.isEmpty() ? d.value(QStringLiteral("dev")).toString() : model, prev, d);

        QJsonObject s = d;
        for (const char *k : {"id", "dev", "model", "capacity_bytes"}) s.remove(QLatin1String(k));
        s[QStringLiteral("d")] = today;
        if (!samples.isEmpty() && samples.last().toObject().value(QStringLiteral("d")).toString() == today) samples.removeLast();
        samples.append(s);
        while (samples.size() > MAX_DAYS) samples.removeFirst();
        disk[QStringLiteral("model")] = model;
        disk[QStringLiteral("samples")] = samples;
        all[id] = disk;
    }
    hist[QStringLiteral("disks")] = all;
    hist[QStringLiteral("sampled")] = today;
    QDir().mkpath(QFileInfo(historyFile()).absolutePath());
    QSaveFile f(historyFile());
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(hist).toJson(QJsonDocument::Compact));
        f.commit();
    }
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
