#include "driftwatch.h"
#include "fwattrtab.h"
#include "mainwindow.h"
#include "optimizetab.h"
#include "platformprofile.h"
#include "privileged.h"
#include "scenes.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <QSet>

static constexpr int PERIOD_MS = 10 * 60 * 1000;
static constexpr int AFTER_SOURCE_MS = 30000;   // TLP and friends act on the charger event
static constexpr int AFTER_APPLY_MS = 20000;    // let a scene's helpers and the EC settle first

DriftWatch::DriftWatch(MainWindow *win) : QObject(win), win_(win) {
    periodic_ = new QTimer(this);
    periodic_->setTimerType(Qt::VeryCoarseTimer);
    periodic_->setInterval(PERIOD_MS);
    connect(periodic_, &QTimer::timeout, this, [this] { run(false); });
    soon_ = new QTimer(this);
    soon_->setSingleShot(true);
    // A check still running would swallow the new reference (run() returns while busy): try again shortly.
    connect(soon_, &QTimer::timeout, this, [this] {
        if (busy_) { soon_->start(3000); return; }
        run(wantSnapshot_);
        wantSnapshot_ = false;
    });
    if (qEnvironmentVariableIsSet("CENTURION_PGO_TRAIN")) return;
    SceneEngine *eng = win_->scenes();
    // A finished scene (ours or a game's) is a new reference.
    connect(eng, &SceneEngine::finished, this, [this](const QString &n) { if (!n.isEmpty()) { wantSnapshot_ = true; soon_->start(AFTER_APPLY_MS); } });
    connect(eng, &SceneEngine::activeSceneChanged, this, [this] { wantSnapshot_ = true; soon_->start(AFTER_APPLY_MS); });
    connect(eng, &SceneEngine::powerSourceChanged, this, [this] { if (!soon_->isActive()) soon_->start(AFTER_SOURCE_MS); });
    periodic_->start();
    QTimer::singleShot(60000, this, [this] { run(true); });  // first reference after start-up
}

QString DriftWatch::culprits() {
    // Long-running tools that write the same knobs; TLP runs only on events, so it counts when installed and enabled.
    static const QSet<QByteArray> daemons{"power-profiles-", "tuned", "auto-cpufreq", "thermald", "system76-power", "ananicy-cpp",
                                          "laptop_mode", "powertop", "cpupower", "gamemoded", "tlp"};
    QStringList found;
    for (const QString &e : QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!e.front().isDigit()) continue;
        QFile f(QStringLiteral("/proc/") + e + QStringLiteral("/comm"));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray c = f.read(32).trimmed();
        for (const QByteArray &d : daemons)
            if (c.startsWith(d) && !found.contains(QString::fromLatin1(c))) found << QString::fromLatin1(c);
    }
    bool tlpEnabled = QFileInfo::exists(QStringLiteral("/etc/systemd/system/multi-user.target.wants/tlp.service"));
    for (const QString &rl : QDir(QStringLiteral("/etc/runlevels")).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        tlpEnabled |= QFileInfo::exists(QStringLiteral("/etc/runlevels/") + rl + QStringLiteral("/tlp"));
    if (tlpEnabled && !found.contains(QStringLiteral("tlp"))) found << QStringLiteral("TLP (enabled service)");
    return found.join(QStringLiteral(", "));
}

void DriftWatch::run(bool snapshot) {
    if (busy_) return;
    busy_ = true;
    auto *p = new QProcess(this);
    QPointer<QProcess> guard(p);
    connect(p, &QProcess::finished, this, [this, p, snapshot](int, QProcess::ExitStatus) {
        busy_ = false;
        const QByteArray out = p->readAllStandardOutput().trimmed();
        p->deleteLater();
        const QJsonObject d = QJsonDocument::fromJson(out.mid(out.lastIndexOf('\n') + 1)).object();
        if (d.value("ok").toBool()) onDescribe(d, snapshot);
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) { if (e == QProcess::FailedToStart) { busy_ = false; p->deleteLater(); } });
    QTimer::singleShot(20000, p, [guard] { if (guard && guard->state() != QProcess::NotRunning) guard->kill(); });
    p->start(OptimizeTab::helperPathForDescribe(), {});
    p->write(R"({"op":"describe","light":true})");
    p->closeWriteChannel();
}

void DriftWatch::onDescribe(const QJsonObject &d, bool snapshot) {
    // A game holds its own layer and calibration measures on purpose: neither is drift.
    if (scenes::gameSessions() > 0 || QFileInfo::exists(QStringLiteral("/run/centurion/calibrating.json"))) return;
    const QJsonObject st = d.value("state").toObject();
    QStringList keys;
    for (const QJsonValue &k : st.value("keys").toArray()) keys << k.toString();
    preset_ = st.value("preset").toString();
    const QString sig = st.value("source").toString() + '|' + preset_ + '|' + keys.join(',');
    QHash<QString, QString> now;
    for (const QJsonValue &v : d.value("tunables").toArray()) {
        const QJsonObject r = v.toObject();
        const QString k = r.value("key").toString();
        if (!keys.contains(k) || r.value("cached").toBool() || !r.value("available").toBool()) continue;
        const QJsonValue c = r.value("current");
        if (c.isString()) now.insert("tune:" + k, c.toString());
    }
    // Firmware limits and the power profile of the active scene.
    if (const auto s = scenes::load(scenes::activeScene())) {
        if (!s->firmware.isEmpty()) {
            for (const FwAttr &a : FwattrTab::discover())
                if (s->firmware.contains(a.name) && !a.viaWmi()) now.insert("fw:" + a.name, QString::number(a.current));
        }
        if (!s->platformProfile.isEmpty())
            if (const auto cur = pp::currentProfile(pp::primaryHandler())) now.insert(QStringLiteral("profile"), *cur);
    }
    if (snapshot || sig != refSig_ || ref_.isEmpty()) {
        ref_ = now;
        refSig_ = sig;
        if (!last_.isEmpty()) { last_.clear(); Q_EMIT drift({}, {}); }
        return;
    }
    QStringList lines;
    for (auto it = ref_.cbegin(); it != ref_.cend(); ++it) {
        const auto n = now.constFind(it.key());
        if (n == now.cend() || *n == it.value()) continue;
        QString key = it.key();
        key = key.startsWith(QLatin1String("tune:")) ? key.mid(5) : key.startsWith(QLatin1String("fw:")) ? key.mid(3) + QStringLiteral(" (firmware)") : QStringLiteral("power profile");
        lines << QStringLiteral("%1: now %2 (was %3 after Centurion applied it)").arg(key, *n, it.value());
    }
    if (lines == last_) return;
    last_ = lines;
    Q_EMIT drift(lines, lines.isEmpty() ? QString() : culprits());
}

void DriftWatch::reapply() {
    const QString scene = scenes::activeScene();
    if (!scene.isEmpty() && scenes::gameSessions() == 0) win_->scenes()->apply(scene);
    else if (!preset_.isEmpty()) win_->optimize()->applyNamedPreset(preset_);
    last_.clear();
    Q_EMIT drift({}, {});
}
