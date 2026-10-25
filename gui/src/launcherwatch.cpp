#include "launcherwatch.h"
#include "optimizetab.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <sys/stat.h>
#include <unistd.h>

static constexpr int SLOW_MS = 15000, FAST_MS = 5000;
static constexpr int START_SWEEPS = 2, END_SWEEPS = 2;
static constexpr qint64 MAX_ENVIRON = 256 * 1024;

LauncherWatch::LauncherWatch(QObject *parent) : QObject(parent) {
    slow_ = new QTimer(this);
    slow_->setTimerType(Qt::VeryCoarseTimer);
    slow_->setInterval(SLOW_MS);
    connect(slow_, &QTimer::timeout, this, &LauncherWatch::slowTick);
    fast_ = new QTimer(this);
    fast_->setTimerType(Qt::CoarseTimer);
    fast_->setInterval(FAST_MS);
    connect(fast_, &QTimer::timeout, this, &LauncherWatch::fastTick);
    if (!qEnvironmentVariableIsSet("CENTURION_PGO_TRAIN")) setEnabled(configured());
}

// Quitting the GUI ends what it started: otherwise the session would only go away at tune-helper's next
// prune (owner gone), with the game scene still set.
LauncherWatch::~LauncherWatch() {
    for (const Game &g : std::as_const(games_)) if (g.active) gamemode(QStringLiteral("POST"), g.name);
}

bool LauncherWatch::configured() {
    QFile f(OptimizeTab::configFile());
    if (!f.open(QIODevice::ReadOnly) || f.size() > 256 * 1024) return false;
    return QJsonDocument::fromJson(f.readAll()).object().value("detect_launchers").toBool();
}

void LauncherWatch::setEnabled(bool on) {
    if (on == enabled_) return;
    enabled_ = on;
    if (!on) {
        slow_->stop();
        fast_->stop();
        for (const Game &g : std::as_const(games_)) if (g.active) gamemode(QStringLiteral("POST"), g.name);
        games_.clear();
        launcher_ = false;
        return;
    }
    slow_->start();
    slowTick();
}

static QList<int> ownPids() {
    QList<int> out;
    const uid_t me = ::getuid();
    const int self = ::getpid();
    for (const QString &e : QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool ok = false;
        const int pid = e.toInt(&ok);
        if (!ok || pid == self) continue;
        struct stat st {};
        if (::stat(QByteArray("/proc/") + e.toLatin1(), &st) == 0 && st.st_uid == me) out << pid;
    }
    return out;
}

static QByteArray readProc(int pid, const char *what, qint64 max = 4096) {
    QFile f(QStringLiteral("/proc/%1/%2").arg(pid).arg(QLatin1String(what)));
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    // /proc files report size 0: read in small chunks (QFile::read(max) would allocate `max` up front).
    QByteArray out;
    char buf[4096];
    while (out.size() < max) {
        const qint64 n = f.read(buf, qMin<qint64>(sizeof buf, max - out.size()));
        if (n <= 0) break;
        out.append(buf, int(n));
    }
    return out;
}

bool LauncherWatch::launcherRunning() const {
    for (int pid : ownPids()) {
        const QByteArray comm = readProc(pid, "comm", 64).trimmed();
        if (comm == "heroic" || comm == "bottles" || comm.startsWith("bottles-")) return true;
    }
    return false;
}

void LauncherWatch::slowTick() {
    if (!enabled_) return;
    launcher_ = launcherRunning();
    retune();
    if (launcher_ && !fast_->isActive()) fastTick();
}

void LauncherWatch::retune() {
    const bool want = enabled_ && (launcher_ || !games_.isEmpty());
    if (want && !fast_->isActive()) fast_->start();
    else if (!want) fast_->stop();
}

/// Wine's own programs and installers: never a game.
static bool systemExe(const QString &exe) {
    static const QSet<QString> sys{
        "services.exe", "winedevice.exe", "plugplay.exe", "explorer.exe", "rpcss.exe", "svchost.exe", "conhost.exe",
        "start.exe", "wineboot.exe", "winemenubuilder.exe", "tabtip.exe", "rundll32.exe", "msiexec.exe", "cmd.exe",
        "regedit.exe", "reg.exe", "winecfg.exe", "control.exe", "taskmgr.exe", "notepad.exe", "uninstaller.exe",
        "iexplore.exe", "wineconsole.exe", "mscorsvw.exe", "ngen.exe", "dxsetup.exe", "crashpad_handler.exe"};
    static const QRegularExpression inst(QStringLiteral("^(setup|install|unins|vc_?redist|vcredist|dotnet|ndp|directx|oalinst|physx|ue4prereq|uninstall)"),
                                         QRegularExpression::CaseInsensitiveOption);
    return sys.contains(exe) || inst.match(exe).hasMatch();
}

static bool bottlesPrefix(const QString &prefix) {
    // ~/.local/share/bottles/bottles/<name>, Flatpak ~/.var/app/com.usebottles.bottles/data/bottles/bottles/<name>.
    return prefix.contains(QLatin1String("/bottles/bottles/")) || prefix.contains(QLatin1String("com.usebottles.bottles"));
}

void LauncherWatch::fastTick() {
    if (!enabled_) return;
    QHash<QString, QPair<QString, QString>> now;  // key → (name, source)
    for (int pid : ownPids()) {
        const QByteArray env = readProc(pid, "environ", MAX_ENVIRON);
        if (env.isEmpty() || env.contains("CENTURION_WRAPPED=1")) continue;
        QString heroicApp, prefix;
        for (const QByteArray &kv : env.split('\0')) {
            if (kv.startsWith("HEROIC_APP_NAME=")) heroicApp = QString::fromUtf8(kv.mid(16));
            else if (kv.startsWith("WINEPREFIX=")) prefix = QString::fromUtf8(kv.mid(11));
        }
        if (!heroicApp.isEmpty()) {
            if (readProc(pid, "comm", 64).trimmed() == "heroic") continue;  // Heroic itself (never set there, but be sure)
            now.insert("heroic:" + heroicApp, {heroicApp.left(128), QStringLiteral("Heroic")});
            continue;
        }
        if (prefix.isEmpty() || !bottlesPrefix(prefix)) continue;
        // Wine sets argv[0] to the Windows path of the program.
        const QByteArray argv0 = readProc(pid, "cmdline", 4096).split('\0').value(0);
        QString exe = QString::fromUtf8(argv0).section(QLatin1Char('\\'), -1).section(QLatin1Char('/'), -1);
        if (!exe.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive)) continue;
        exe = exe.toLower();
        if (systemExe(exe)) continue;
        QString name = exe.left(exe.size() - 4);
        now.insert("bottles:" + prefix + '|' + exe, {name.left(128), QStringLiteral("Bottles")});
    }
    for (auto it = now.cbegin(); it != now.cend(); ++it) {
        Game &g = games_[it.key()];
        g.name = it.value().first;
        g.source = it.value().second;
        g.missing = 0;
        if (!g.active && ++g.seen >= START_SWEEPS) {
            g.active = true;
            gamemode(QStringLiteral("PRE"), g.name);
            Q_EMIT gameStarted(g.name, g.source);
        }
    }
    for (auto it = games_.begin(); it != games_.end();) {
        if (now.contains(it.key())) { ++it; continue; }
        if (!it->active) { it = games_.erase(it); continue; }
        if (++it->missing < END_SWEEPS) { ++it; continue; }
        gamemode(QStringLiteral("POST"), it->name);
        Q_EMIT gameEnded(it->name, it->source);
        it = games_.erase(it);
    }
    retune();
}

void LauncherWatch::gamemode(const QString &verb, const QString &game) {
    QString tool = QStandardPaths::findExecutable(QStringLiteral("centurion-gamemode"));
    if (tool.isEmpty()) tool = QStringLiteral("/usr/bin/centurion-gamemode");
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("CENTURION_GAME"), game);
    // Detached (PRE / POST can wait for polkit and helpers for minutes; POST must also run when the GUI
    // quits), so the parent chain no longer leads here: name the GUI as the session owner explicitly.
    env.insert(QStringLiteral("CENTURION_OWNER_PID"), QString::number(::getpid()));
    p.setProcessEnvironment(env);
    p.setProgram(tool);
    p.setArguments({verb});
    p.setStandardOutputFile(QProcess::nullDevice());
    p.startDetached();
}
