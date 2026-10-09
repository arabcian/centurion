#include "scenes.h"
#include "fwattrtab.h"
#include "hometab.h"
#include "inteltab.h"
#include "lighting.h"
#include "lightingtab.h"
#include "mainwindow.h"
#include "nvidiatab.h"
#include "optimizetab.h"
#include "platformprofile.h"
#include "privileged.h"
#include "ryzentab.h"
#include <algorithm>
#include <QDir>
#include <QFile>
#include <QFileSystemWatcher>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QTimer>
#include <QGuiApplication>
#include <QSessionManager>
#include <QSocketNotifier>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <linux/netlink.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <cstdint>
#include <limits>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

namespace scenes {

static constexpr qint64 MAX_FILE = 64 * 1024;

static QString configDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/centurion");
}
QString dir() { return configDir() + QStringLiteral("/scenes"); }
static QString autoFile() { return configDir() + QStringLiteral("/scenes.json"); }
static QString sceneFile(const QString &n) { return dir() + '/' + n + QStringLiteral(".json"); }

bool validName(const QString &n) {
    // Same rule as the Ryzen/Intel profile names: it becomes a file name.
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9 _-]{0,63}\\z"));
    return re.match(n).hasMatch();
}

static QJsonObject readObject(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > MAX_FILE) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

static bool writeObject(const QString &path, const QJsonObject &o, QString *err) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) { if (err) *err = "cannot create " + QFileInfo(path).absolutePath(); return false; }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(o).toJson()) < 0 || !f.commit()) {
        if (err) *err = f.errorString();
        return false;
    }
    return true;
}

static Choice choiceFrom(const QJsonValue &v) {
    const QJsonObject o = v.toObject();
    if (o.value("reset").toBool()) return {Choice::Reset, {}};
    if (const QString n = o.value("profile").toString(); !n.isEmpty()) return {Choice::Profile, n};
    return {};
}

static QJsonValue choiceTo(const Choice &c) {
    switch (c.kind) {
    case Choice::Reset: return QJsonObject{{"reset", true}};
    case Choice::Profile: return QJsonObject{{"profile", c.name}};
    case Choice::Unchanged: break;
    }
    return QJsonValue::Undefined;
}

QStringList names() {
    QStringList out;
    for (QString f : QDir(dir()).entryList({"*.json"}, QDir::Files | QDir::Readable, QDir::Name)) {
        f.chop(5);
        if (validName(f)) out << f;
    }
    return out;
}

std::optional<Scene> load(const QString &name) {
    if (!validName(name) || !QFile::exists(sceneFile(name))) return std::nullopt;
    const QJsonObject o = readObject(sceneFile(name));
    Scene s;
    s.name = name;
    if (const QString p = o.value("platform_profile").toString(); pp::VALID_PROFILES.contains(p)) s.platformProfile = p;
    const QJsonObject fw = o.value("firmware").toObject();
    for (auto it = fw.begin(); it != fw.end(); ++it)
        if (it->isDouble()) s.firmware.insert(it.key(), it->toInt());
    s.cpu = choiceFrom(o.value("cpu_curve"));
    s.gpu = choiceFrom(o.value("gpu_curve"));
    s.tuning = choiceFrom(o.value("tuning"));
    const QJsonObject light = o.value("lighting").toObject();
    if (const int p = light.value("profile").toInt(-1); p >= 0 && p <= 6) s.lightProfile = p;
    if (const int b = light.value("brightness").toInt(-1); b >= 0 && b <= 9) s.lightBrightness = b;
    if (const QJsonValue f = o.value("fan_fullspeed"); f.isBool()) s.fanFullSpeed = f.toBool() ? 1 : 0;
    const QJsonArray ft = o.value("fan_table").toArray();
    if (ft.size() == 10) {
        QVector<int> lv;
        for (const QJsonValue &v : ft) lv << v.toInt(0);
        if (std::all_of(lv.begin(), lv.end(), [](int x) { return x >= 1 && x <= 10; })) s.fanTable = lv;
    }
    s.command = o.value("command").toString().trimmed();
    return s;
}

bool save(const Scene &s, QString *err) {
    if (!validName(s.name)) { if (err) *err = "invalid scene name"; return false; }
    QJsonObject o{{"version", 1}};
    if (!s.platformProfile.isEmpty()) o["platform_profile"] = s.platformProfile;
    if (!s.firmware.isEmpty()) {
        QJsonObject fw;
        for (auto it = s.firmware.cbegin(); it != s.firmware.cend(); ++it) fw[it.key()] = it.value();
        o["firmware"] = fw;
    }
    for (const auto &[key, c] : {std::pair{"cpu_curve", s.cpu}, {"gpu_curve", s.gpu}, {"tuning", s.tuning}})
        if (c.kind != Choice::Unchanged) o[QLatin1String(key)] = choiceTo(c);
    if (s.lightProfile >= 0 || s.lightBrightness >= 0) {
        QJsonObject light;
        if (s.lightProfile >= 0) light["profile"] = s.lightProfile;
        if (s.lightBrightness >= 0) light["brightness"] = s.lightBrightness;
        o["lighting"] = light;
    }
    if (s.fanFullSpeed >= 0) o["fan_fullspeed"] = s.fanFullSpeed == 1;
    if (s.fanTable.size() == 10) { QJsonArray a; for (int x : s.fanTable) a << x; o["fan_table"] = a; }
    if (!s.command.isEmpty()) o["command"] = s.command;
    return writeObject(sceneFile(s.name), o, err);
}

bool remove(const QString &name) { return validName(name) && QFile::remove(sceneFile(name)); }

Auto loadAuto() {
    const QJsonObject o = readObject(autoFile());
    Auto a;
    a.enabled = o.value("auto").toBool();
    a.onAc = o.value("on_ac").toString();
    a.onBattery = o.value("on_battery").toString();
    a.paused = o.value("paused").toBool();
    a.onResume = o.value("on_resume").toBool();
    if (!validName(a.onAc)) a.onAc.clear();
    if (!validName(a.onBattery)) a.onBattery.clear();
    return a;
}

bool saveAuto(const Auto &a, QString *err) {
    return writeObject(autoFile(), {{"auto", a.enabled}, {"on_ac", a.onAc}, {"on_battery", a.onBattery}, {"paused", a.paused}, {"on_resume", a.onResume}}, err);
}

std::optional<bool> onAc(bool lenient) {
    // Charger first: a Legion under full load can draw more than its brick
    // delivers, so the battery reads "Discharging" while plugged in — that
    // must not flip the machine into the battery scene mid-game.
    //
    // Reading is strict about errors: pp::readText() returns "" when open() worked but
    // read() failed (EIO/EBUSY while the EC is busy), and nullopt when the attribute does
    // not exist. An empty `online` used to compare unequal to "1" and meant "on battery":
    // one failed read during a scene's own EC writes was enough to flip the source.
    const QDir d(QStringLiteral("/sys/class/power_supply"));
    bool anySupply = false, anyBattery = false, discharging = false;
    bool supplyUnreadable = false, batteryUnreadable = false;
    for (const QString &e : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System, QDir::Name)) {
        const QString p = d.filePath(e);
        // Wireless mice, headsets and game pads register as power supplies too ("Device" scope):
        // their battery says nothing about how this machine is powered.
        if (pp::readText(p + "/scope").value_or(QString()) == QLatin1String("Device")) continue;
        const auto type = pp::readText(p + "/type");
        if (!type) continue;                                  // not a power-supply node
        if (type->isEmpty()) { supplyUnreadable = true; continue; }
        if (*type == QLatin1String("Mains") || *type == QLatin1String("USB")) {  // USB = UCSI / PD source
            anySupply = true;
            const auto online = pp::readText(p + "/online");
            if (online && *online == QLatin1String("1")) return true;
            if (online && online->isEmpty()) supplyUnreadable = true;
        } else if (*type == QLatin1String("Battery")) {
            anyBattery = true;
            const auto st = pp::readText(p + "/status");
            if (st && st->isEmpty()) batteryUnreadable = true;
            discharging |= st && *st == QLatin1String("Discharging");
        }
    }
    if (supplyUnreadable && !lenient) return std::nullopt;    // cannot tell: keep the last known source
    if (anySupply) return false;              // chargers present, none online
    if (batteryUnreadable && !lenient) return std::nullopt;
    if (anyBattery) return !discharging;      // no charger nodes: go by the battery
    return std::nullopt;
}

static QString stateFile() {
    return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/centurion/scene.json");
}

/// scene.json is shared with centurion-gamemode: every read-modify-write holds scene.json.lock (flock), so
/// neither side can drop the other's fields. Held for the file update only, never across a helper call.
namespace {
struct StateLock {
    int fd = -1;
    StateLock() {
        const QString dir = QFileInfo(stateFile()).absolutePath();
        QDir().mkpath(dir);
        fd = ::open(QFile::encodeName(dir + QStringLiteral("/scene.json.lock")).constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd >= 0) while (::flock(fd, LOCK_EX) != 0 && errno == EINTR) {}
    }
    ~StateLock() { if (fd >= 0) ::close(fd); }
    StateLock(const StateLock &) = delete;
    StateLock &operator=(const StateLock &) = delete;
};
}

QString activeScene() {
    const QString n = readObject(stateFile()).value("active").toString();
    return validName(n) ? n : QString();
}

void setActiveScene(const QString &name) {
    const StateLock lock;
    QJsonObject o = readObject(stateFile());
    o["active"] = name;
    writeObject(stateFile(), o, nullptr);
}

/// /proc/<pid>/stat field 22 (start time): pid + start identify one process.
static std::optional<quint64> procStart(qint64 pid) {
    if (pid <= 1) return std::nullopt;
    QFile f(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QByteArray s = f.readAll();
    const int r = s.lastIndexOf(')');
    if (r < 0) return std::nullopt;
    const QList<QByteArray> fields = s.mid(r + 1).simplified().split(' ');
    if (fields.size() < 20) return std::nullopt;
    bool ok = false;
    const quint64 v = fields.at(19).toULongLong(&ok);
    return ok ? std::optional<quint64>(v) : std::nullopt;
}

int gameSessions() {
    // /run/centurion/tune/state.json is root-owned but world-readable.
    // Sessions whose launcher has exited are not counted: tune-helper ends
    // them itself at its next call (same rule as centurion_helpers::live_game_sessions).
    const QJsonObject st = readObject(QStringLiteral("/run/centurion/tune/state.json"));
    const QJsonValue ss = st.value("sessions");
    if (!ss.isArray()) return st.value("refcount").toInt();
    int n = 0;
    for (const QJsonValue &v : ss.toArray()) {
        const QJsonObject o = v.toObject();
        if (!o.value("pid").isDouble() || !o.value("start").isDouble()) { ++n; continue; }  // untracked
        const auto start = procStart(o.value("pid").toInteger());
        if (start && *start == quint64(o.value("start").toInteger())) ++n;
    }
    return n;
}

/// centurion-gamemode has switched to a game scene and not switched back yet.
static bool gameSceneActive() {
    const QJsonValue v = readObject(stateFile()).value("game_scene");
    return v.isString() && !v.toString().isEmpty();
}

/// centurion-gamemode holds this lock while a game is starting (scene + preset).
static bool gameStarting() {
    const QString path = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation)
                         + QStringLiteral("/centurion/gamemode-start.lock");
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool busy = ::flock(fd, LOCK_EX | LOCK_NB) != 0;
    if (!busy) ::flock(fd, LOCK_UN);
    ::close(fd);
    return busy;
}

static QString bootId() {
    QFile f(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    return f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.readAll()).trimmed() : QString();
}

QString bootGuardReason() {
    const QJsonObject o = readObject(QStringLiteral("/var/lib/centurion/boot-guard.json"));
    if (o.value("tripped").toBool()) return o.value("reason").toString(QStringLiteral("boot presets are paused"));
    // Same rule as centurion-boot-guard check: the previous boot died armed.
    if (o.value("state").toString() == QLatin1String("armed") && o.value("boot_id").toString() != bootId())
        return QStringLiteral("the previous boot did not stay up after the presets were applied");
    return {};
}

static QString loginGuardFile() {
    const QString base = qEnvironmentVariableIsEmpty("XDG_STATE_HOME")
        ? QDir::homePath() + QStringLiteral("/.local/state") : qEnvironmentVariable("XDG_STATE_HOME");
    return base + QStringLiteral("/centurion/login-guard.json");
}

QString loginGuardReason() {
    const QJsonObject o = readObject(loginGuardFile());
    return o.value("tripped").toBool() ? o.value("reason").toString() : QString();
}

void resumeLoginGuard() { writeObject(loginGuardFile(), {{"state", "ok"}, {"boot_id", bootId()}}, nullptr); }

} // namespace scenes

// ── engine ──────────────────────────────────────────────────────────────────

using namespace scenes;

// Fast cadence only while something is pending (a debounce, a deferred switch,
// a game scene); otherwise charger changes arrive as kernel uevents and the
// slow tick is just a safety net (and notices a game scene appearing).
static constexpr int POWER_POLL_MS = 3000, IDLE_POLL_MS = 20000, STABLE_READS = 2, STARTUP_DELAY_MS = 4000;
// After a wake the firmware / EC may have reset limits and the drivers need a moment: first pass after
// RESUME_DELAY_MS, then RESUME_PASSES-1 more RESUME_REPASS_MS apart (catches a late firmware override).
static constexpr int RESUME_TICK_MS = 2000, RESUME_DELAY_MS = 5000, RESUME_REPASS_MS = 30000, RESUME_PASSES = 2;
static constexpr long long RESUME_GAP_NS = 1000000000LL;  // sleeping longer than 1 s counts as a suspend
static constexpr int GAME_ORPHAN_READS = 5;  // ~15 s without a game while the game scene is still set
// Power-source debounce (by time). Going to battery waits longer: a USB-C PD hard reset or a brick that
// browns out under load drops "online" for up to a couple of seconds, and the battery scene (lower limits)
// is the one that would then be applied for nothing.
static constexpr int DEBOUNCE_TO_BATTERY_MS = 4000, DEBOUNCE_TO_AC_MS = 2000;
static constexpr int UEVENT_COALESCE_MS = 300;   // one sysfs sweep per burst of power_supply uevents
static constexpr int UNKNOWN_LENIENT_MS = 15000; // source unreadable this long: stop being strict about read errors
// Flap breaker: this many committed source changes inside the window = something is oscillating (a brick that
// collapses under the AC scene's higher limits, a loose cable). Automatic switching holds the current scene
// until the source has been quiet for FLAP_QUIET_MS, then applies the scene for wherever it settled.
static constexpr int FLAP_WINDOW_MS = 120000, FLAP_MAX_SWITCHES = 4, FLAP_QUIET_MS = 30000;
// The EC can override the platform profile a few seconds after a plug/unplug: look once more after this.
static constexpr int VERIFY_DELAY_MS = 8000;

/// centurion-calibrate holds the power context while it measures (/run/centurion/calibrating.json,
/// world-readable): a scene switched under it would be measured as if it were the knob under test, and
/// the root helpers refuse the writes anyway. A hold whose process is gone is no hold.
static bool calibrationHold() {
    const QJsonObject o = readObject(QStringLiteral("/run/centurion/calibrating.json"));
    const qint64 pid = o.value(QStringLiteral("pid")).toInteger(0);
    return pid > 0 && QFile::exists(QStringLiteral("/proc/%1").arg(pid));
}

SceneEngine::SceneEngine(MainWindow *win) : QObject(win), win_(win), auto_(loadAuto()) {
    powerTimer_ = new QTimer(this);
    powerTimer_->setInterval(POWER_POLL_MS);
    connect(powerTimer_, &QTimer::timeout, this, [this] { pollPower(true); });
    clock_.start();
    for (QTimer **t : {&debounce_, &uevTimer_, &flapRelease_, &verify_}) {
        *t = new QTimer(this);
        (*t)->setSingleShot(true);
    }
    connect(debounce_, &QTimer::timeout, this, [this] { pollPower(false); });
    connect(uevTimer_, &QTimer::timeout, this, [this] { pollPower(false); });
    connect(verify_, &QTimer::timeout, this, &SceneEngine::verifyPass);
    connect(flapRelease_, &QTimer::timeout, this, [this] {
        flapHold_ = false;
        flips_.clear();
        if (!auto_.enabled || auto_.paused) return;
        Q_EMIT finished(QString(), true, {QStringLiteral("power source is stable again — applying the scene for it")});
        if (const auto now = onAc()) ac_ = now;
        if (ac_) applyForSource(*ac_);
    });
    ac_ = onAc();
    // No automatic scenes in install.sh's PGO training run (it uses the real
    // GUI, and privileged::run is disabled there anyway).
    if (qEnvironmentVariableIsSet("CENTURION_PGO_TRAIN")) return;
    watchUevents();
    watchSceneState();
    retunePoll();
    retuneResume();
    // Session start: bring the machine to the scene for the current source,
    // after the tabs have finished their own startup reads.
    // A theme change re-execs the app: the hardware is already in the scene's state.
    // (`ac_` may still be unknown here after a failed read: startupApply() reads it again.)
    if (auto_.enabled && !QCoreApplication::arguments().contains(QStringLiteral("--theme-restart")))
        QTimer::singleShot(STARTUP_DELAY_MS, this, &SceneEngine::startupApply);
}

// The login scene can carry a CPU/GPU curve: if one is unstable, applying it
// at every login would crash every login. A small user-level guard (like
// centurion-boot-guard for the boot presets) marks the apply as in progress and
// clears it once the session has survived LOGIN_WINDOW_MS; a login that died
// in between pauses the automatic apply until the user resumes it (Home).
static constexpr int LOGIN_WINDOW_MS = 120000;

void SceneEngine::startupApply() {
    if (!ac_) ac_ = onAc();
    if (!auto_.enabled || auto_.paused) return;
    // A calibration scheduled for this boot holds the machine at its boot state: the login scene waits.
    if (calibrationHold()) {
        calDeferred_ = true;
        Q_EMIT finished(QString(), true, {QStringLiteral("a calibration holds the machine at its boot state — the login scene is applied when it ends")});
        retunePoll();
        return;
    }
    // Source still unreadable (EC busy at login): the login scene used to be dropped for good here.
    // pollPower() runs this again at the first successful read (and polls fast until then).
    if (!ac_) { startupPending_ = true; retunePoll(); return; }
    const QString file = loginGuardFile();
    QJsonObject g = readObject(file);
    const QString cur = bootId();
    // A login that was still inside its window when the machine went down
    // trips the guard -- unless that boot is known to have shut down cleanly
    // (centurion-boot-guard records it at service stop): a normal reboot or poweroff
    // soon after login is not a crash.
    const QString prevBoot = g.value("boot_id").toString();
    const bool cleanEnd = !prevBoot.isEmpty() && readObject(QStringLiteral("/var/lib/centurion/boot-guard.json"))
                                                     .value("clean_shutdown").toString() == prevBoot;
    if (!g.value("tripped").toBool() && g.value("state").toString() == QLatin1String("applying")
        && prevBoot != cur && !cleanEnd) {
        g["tripped"] = true;
        g["reason"] = QStringLiteral("the last login ended without a clean shutdown within 2 minutes of applying its scene");
        writeObject(file, g, nullptr);
    }
    QString why = g.value("tripped").toBool() ? g.value("reason").toString() : QString();
    if (why.isEmpty()) why = bootGuardReason();
    if (!why.isEmpty()) {
        Q_EMIT finished(QString(), false, {QStringLiteral("automatic scene at login skipped — ") + why});
        return;
    }
    writeObject(file, {{"state", "applying"}, {"boot_id", cur}}, nullptr);
    // Cleared after the window, and on a clean quit (logout / shutdown / Quit).
    QTimer::singleShot(LOGIN_WINDOW_MS, this, [file, cur] { writeObject(file, {{"state", "ok"}, {"boot_id", cur}}, nullptr); });
    const auto markOk = [file, cur] {
        const QJsonObject o = readObject(file);
        if (o.value("state").toString() == QLatin1String("applying")) writeObject(file, {{"state", "ok"}, {"boot_id", cur}}, nullptr);
    };
    connect(qApp, &QCoreApplication::aboutToQuit, this, markOk);
    // X11 session managers announce logout/shutdown here before the app is
    // killed; SIGTERM/SIGHUP end in aboutToQuit (main.cpp).
    connect(qGuiApp, &QGuiApplication::commitDataRequest, this, [markOk](QSessionManager &) { markOk(); });
    applyForSource(*ac_);
}

bool SceneEngine::setAuto(const Auto &a, QString *err) {
    const bool wasOn = auto_.enabled;
    if (!saveAuto(a, err)) return false;
    auto_ = a;
    retuneResume();
    if (a.enabled && !wasOn && ac_) applyForSource(*ac_);
    return true;
}

bool SceneEngine::setPaused(bool on, QString *err) {
    if (auto_.paused == on) return true;
    Auto a = auto_;
    a.paused = on;
    if (!saveAuto(a, err)) return false;
    auto_ = a;
    Q_EMIT pausedChanged(on);
    Q_EMIT finished(QString(), true, {on ? QStringLiteral("scenes paused — no automatic scene changes until resumed")
                                         : QStringLiteral("scenes resumed")});
    // Resuming catches up with the current power source.
    if (!on && auto_.enabled && ac_) applyForSource(*ac_);
    return true;
}

static long long bootOffsetNs() {
    timespec b{}, m{};
    if (clock_gettime(CLOCK_BOOTTIME, &b) != 0 || clock_gettime(CLOCK_MONOTONIC, &m) != 0) return -1;
    return (b.tv_sec - m.tv_sec) * 1000000000LL + (b.tv_nsec - m.tv_nsec);
}

/// Runs the suspend detector only while "reapply after resume" is on (and automatic scenes are enabled),
/// so nothing wakes up for it otherwise.
void SceneEngine::retuneResume() {
    const bool want = auto_.enabled && auto_.onResume && !qEnvironmentVariableIsSet("CENTURION_PGO_TRAIN");
    if (!want) {
        if (resumeTick_) resumeTick_->stop();
        if (resumeNotifier_) resumeNotifier_->setEnabled(false);
        if (resumePass_) resumePass_->stop();
        resumePassesLeft_ = 0;
        return;
    }
    if (!resumeTick_) {
        resumeTick_ = new QTimer(this);
        resumeTick_->setTimerType(Qt::CoarseTimer);
        resumeTick_->setInterval(RESUME_TICK_MS);
        connect(resumeTick_, &QTimer::timeout, this, &SceneEngine::resumeTick);
        resumePass_ = new QTimer(this);
        resumePass_->setSingleShot(true);
        connect(resumePass_, &QTimer::timeout, this, &SceneEngine::resumePass);
        // A resume sets the wall clock (the sleep time is added to it), and a CLOCK_REALTIME timerfd armed
        // with TFD_TIMER_CANCEL_ON_SET becomes readable when that happens: the detector then costs nothing
        // while the machine is awake, where the tick woke the tray app every 2 s for its whole life. An NTP
        // step or `date -s` raises it too; resumeTick() tells them apart by the BOOTTIME offset as before.
        resumeFd_ = ::timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC);
        if (resumeFd_ >= 0 && !armResumeFd()) { ::close(resumeFd_); resumeFd_ = -1; }
        if (resumeFd_ >= 0) {
            resumeNotifier_ = new QSocketNotifier(resumeFd_, QSocketNotifier::Read, this);
            resumeNotifier_->setEnabled(false);
            connect(resumeNotifier_, &QSocketNotifier::activated, this, [this] {
                std::uint64_t ticks = 0;
                [[maybe_unused]] const auto n = ::read(resumeFd_, &ticks, sizeof ticks);  // ECANCELED: the clock was set
                armResumeFd();  // one notification per arming
                resumeTick();
            });
            connect(this, &QObject::destroyed, [fd = resumeFd_] { ::close(fd); });
        }
    }
    if (resumeNotifier_) {
        if (!resumeNotifier_->isEnabled()) {
            bootOffsetNs_ = bootOffsetNs();
            armResumeFd();
            resumeNotifier_->setEnabled(true);
        }
        return;  // pollPower()'s slow tick doubles as the safety net
    }
    if (!resumeTick_->isActive()) {
        bootOffsetNs_ = bootOffsetNs();
        resumeTick_->start();
    }
}

/// (Re)arms the wall-clock-set notification: an absolute timer at the end of time, only its cancellation matters.
bool SceneEngine::armResumeFd() {
    if (resumeFd_ < 0) return false;
    itimerspec its{};
    its.it_value.tv_sec = std::numeric_limits<time_t>::max();
    if (::timerfd_settime(resumeFd_, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &its, nullptr) == 0) return true;
    its.it_value.tv_sec = std::numeric_limits<std::int32_t>::max();  // kernels that reject the 64-bit maximum
    return ::timerfd_settime(resumeFd_, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &its, nullptr) == 0;
}

// CLOCK_BOOTTIME keeps counting in suspend, CLOCK_MONOTONIC does not: their difference jumps by the sleep time.
void SceneEngine::resumeTick() {
    const long long off = bootOffsetNs();
    if (off < 0) { resumeTick_->stop(); return; }  // no CLOCK_BOOTTIME: detection unavailable
    const bool woke = bootOffsetNs_ >= 0 && off - bootOffsetNs_ > RESUME_GAP_NS;
    bootOffsetNs_ = off;
    if (!woke) return;
    resumePassesLeft_ = RESUME_PASSES;  // a new suspend restarts the sequence
    resumePass_->start(RESUME_DELAY_MS);
}

void SceneEngine::resumePass() {
    if (!auto_.enabled || !auto_.onResume || resumePassesLeft_ <= 0) { resumePassesLeft_ = 0; return; }
    --resumePassesLeft_;
    // The charger may have been (un)plugged while asleep; take the fresh state, applyForSource honours
    // pause, a running game (deferred) and a scene already being applied (queued).
    if (const auto now = onAc()) {
        if (!ac_ || *now != *ac_) { ac_ = now; candidate_.reset(); debounce_->stop(); Q_EMIT powerSourceChanged(*ac_); }
    }
    if (ac_) applyForSource(*ac_);
    if (resumePassesLeft_ > 0) resumePass_->start(RESUME_REPASS_MS);
}

void SceneEngine::retunePoll() {
    const bool busy = startupPending_ || candidate_ || deferred_ || calDeferred_ || sawGameScene_ || gameGoneReads_ > 0 || uevFd_ < 0;
    const int want = busy ? POWER_POLL_MS : IDLE_POLL_MS;
    powerTimer_->setTimerType(busy ? Qt::CoarseTimer : Qt::VeryCoarseTimer);
    if (powerTimer_->interval() != want || !powerTimer_->isActive()) powerTimer_->start(want);
}

/// Kernel uevents (NETLINK_KOBJECT_UEVENT, group 1 — readable without
/// privileges): a charger plug/unplug wakes us at once instead of a 3 s poll.
void SceneEngine::watchUevents() {
    const int fd = ::socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) return;
    sockaddr_nl sa{};
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = 1;
    if (::bind(fd, reinterpret_cast<sockaddr *>(&sa), sizeof sa) != 0) { ::close(fd); return; }
    uevFd_ = fd;
    auto *n = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(n, &QSocketNotifier::activated, this, [this, fd] {
        static constexpr char KEY[] = "SUBSYSTEM=power_supply";
        char buf[8192];
        bool power = false;
        for (ssize_t len; (len = ::recv(fd, buf, sizeof buf, 0)) > 0;)
            power |= ::memmem(buf, size_t(len), KEY, sizeof KEY - 1) != nullptr;
        if (!power) return;
        // Coalesced: a plug/unplug (and every battery % change) sends several events within a few ms;
        // each used to be a full sysfs sweep on the GUI thread, i.e. more ACPI/EC calls while the EC is busy.
        if (!uevTimer_->isActive()) uevTimer_->start(UEVENT_COALESCE_MS);
    });
    connect(this, &QObject::destroyed, [fd] { ::close(fd); });
}

// centurion-gamemode (PRE / POST / SCENE) switches scenes without the GUI: it only rewrites scene.json. Nothing
// told the GUI, so the Scenes tab, the tray tooltip and the notifications stayed on the old scene until
// something else refreshed them (e.g. toggling "Switch scenes with the power source"). Watch the state
// directory (the file is replaced by rename, so the file itself would lose the watch) and report changes.
void SceneEngine::watchSceneState() {
    const QString dir = QFileInfo(stateFile()).absolutePath();
    QDir().mkpath(dir);
    seenActive_ = activeScene();
    stateTimer_ = new QTimer(this);
    stateTimer_->setSingleShot(true);
    connect(stateTimer_, &QTimer::timeout, this, &SceneEngine::externalStateChanged);
    stateWatch_ = new QFileSystemWatcher(this);
    stateWatch_->addPath(dir);
    connect(stateWatch_, &QFileSystemWatcher::directoryChanged, this, [this] { stateTimer_->start(150); });
}

void SceneEngine::externalStateChanged() {
    const QString now = activeScene();
    if (now == seenActive_) return;
    seenActive_ = now;
    if (busy_ || now.isEmpty()) return;  // our own apply reports through started()/finished()
    Q_EMIT activeSceneChanged(now, gameSceneActive());
}

void SceneEngine::pollPower(bool timerTick) {
    struct Retune { SceneEngine *e; ~Retune() { e->retunePoll(); } } retune{this};
    // The game-end bookkeeping counts reads as "seconds" (STABLE_READS, GAME_ORPHAN_READS): only the
    // poll timer may advance it, not a burst of uevents.
    if (timerTick) checkGameEnd();
    if (timerTick && calDeferred_ && !calibrationHold()) {
        calDeferred_ = false;
        if (auto_.enabled && !auto_.paused && ac_) {
            Q_EMIT finished(QString(), true, {QStringLiteral("calibration finished — applying the scene for the power source")});
            applyForSource(*ac_);
        }
    }
    // Safety net for the event-driven suspend detector, on a tick that runs anyway.
    if (timerTick && resumeNotifier_ && resumeNotifier_->isEnabled()) resumeTick();
    auto now = onAc();
    if (!now) {
        // Unreadable (EC busy) or no supplies: keep the last known source. If it stays unreadable for a long
        // time the attribute is probably broken for good -- then read errors count as "offline" again.
        if (!unknownClock_.isValid()) unknownClock_.start();
        else if (unknownClock_.elapsed() > UNKNOWN_LENIENT_MS) now = onAc(true);
        if (!now) return;
    } else {
        unknownClock_.invalidate();
    }
    if (!ac_) {
        ac_ = now;
        Q_EMIT powerSourceChanged(*ac_);
        if (std::exchange(startupPending_, false)) startupApply();
        return;
    }
    if (*now == *ac_) { candidate_.reset(); debounce_->stop(); return; }
    // Debounce by time: the new state must hold continuously for `need` ms.
    const int need = *now ? DEBOUNCE_TO_AC_MS : DEBOUNCE_TO_BATTERY_MS;
    if (candidate_ != now) { candidate_ = now; candidateClock_.start(); debounce_->start(need); return; }
    if (const qint64 el = candidateClock_.elapsed(); el < need) {
        if (!debounce_->isActive()) debounce_->start(int(need - el));
        return;
    }
    candidate_.reset();
    debounce_->stop();
    commitPowerChange(*now);
}

void SceneEngine::commitPowerChange(bool onAc) {
    ac_ = onAc;
    Q_EMIT powerSourceChanged(onAc);
    if (!auto_.enabled) return;
    // Paused: nothing is switched, so there is nothing to hold or to report either
    // (the flap breaker used to announce "staying on the battery scene" while paused).
    if (auto_.paused) { deferred_ = false; return; }
    // Flap breaker. A scene switch changes the machine's power draw (limits, profile, dGPU), and on a marginal
    // brick/PD contract that can itself drop the source again -> battery scene -> source back -> AC scene -> ...
    const qint64 t = clock_.elapsed();
    flips_.append(t);
    while (!flips_.isEmpty() && t - flips_.first() > FLAP_WINDOW_MS) flips_.removeFirst();
    if (flapHold_ || flips_.size() >= FLAP_MAX_SWITCHES) {
        const bool first = !flapHold_;
        flapHold_ = true;
        flapRelease_->start(FLAP_QUIET_MS);  // restarts at every change: needs a quiet half minute
        if (first) {
            Q_EMIT finished(QString(), false, {QStringLiteral("power source switched %1 times in 2 minutes — staying on the battery scene until it is stable "
                                                               "(check the charger and cable; a charger weaker than the AC scene's power limits can do this)").arg(flips_.size())});
            // Hold on the conservative side: the battery scene's limits are the ones a marginal charger can sustain,
            // and the machine must not sit in the AC scene while the source is actually the battery.
            if (activeScene() != auto_.onBattery) applyForSource(false);
        }
        return;
    }
    applyForSource(onAc);
}

// After a game: centurion-gamemode's last POST returns to the right scene itself.
// Two cases were left hanging before and are handled here:
//  * no game scene configured: a power-source change during the game was
//    deferred and then never applied;
//  * the launcher died without POST: the game scene (and, until tune-helper's
//    next call, the game tuning) stayed active indefinitely.
void SceneEngine::checkGameEnd() {
    const bool sceneOn = gameSceneActive();
    if (sceneOn) sawGameScene_ = true;
    if (!deferred_ && !sceneOn) { gameGoneReads_ = 0; sawGameScene_ = false; return; }
    if (busy_ || gameSessions() > 0 || gameStarting()) { gameGoneReads_ = 0; return; }
    ++gameGoneReads_;
    if (!sceneOn) {
        if (sawGameScene_) {  // POST left the game scene and applied the right one itself
            deferred_ = sawGameScene_ = false;
            gameGoneReads_ = 0;
            return;
        }
        // No game scene: nobody else will apply the deferred switch.
        if (gameGoneReads_ < STABLE_READS) return;
        gameGoneReads_ = 0;
        deferred_ = false;
        if (auto_.enabled && ac_) applyForSource(*ac_);
        return;
    }
    // Game scene still set with no game left: give a normal POST time to
    // switch back (it clears game_scene first), then clean up ourselves.
    if (gameGoneReads_ < GAME_ORPHAN_READS) return;
    gameGoneReads_ = 0;
    deferred_ = sawGameScene_ = false;
    QString before;
    {
        // Claim the clean-up under the lock: a POST that got there first has already left the scene.
        const StateLock lock;
        QJsonObject st = readObject(stateFile());
        if (const QJsonValue g = st.value("game_scene"); !g.isString() || g.toString().isEmpty()) return;
        before = st.value("before_game").toString();
        st["game_scene"] = QJsonValue::Null;
        st["before_game"] = QJsonValue::Null;
        writeObject(stateFile(), st, nullptr);
    }
    Q_EMIT finished(QString(), true, {QStringLiteral("the game launcher exited without its POST hook — leaving the game scene")});
    const QString target = auto_.enabled && ac_ ? (*ac_ ? auto_.onAc : auto_.onBattery) : before;
    // Ends the dead session and restores the game tuning (tune-helper prune).
    privileged::run(privileged::helperPath("tune-profile-helper"), QJsonObject{{"op", "prune"}}, this,
                    [this, target](const privileged::Result &) { if (validName(target) && !auto_.paused) apply(target); }, 120000);
}

void SceneEngine::applyForSource(bool onAc) {
    if (auto_.paused) { deferred_ = false; return; }
    if (calibrationHold()) {
        if (!calDeferred_) Q_EMIT finished(QString(), true, {QStringLiteral("a calibration is measuring — scene switch deferred until it ends")});
        calDeferred_ = true;
        retunePoll();
        return;
    }
    // Never switch scenes under a running game: centurion-gamemode POST returns to
    // the scene for the then-current power source when the last game exits.
    // (A game that is still starting holds gamemode-start.lock: same rule.)
    // A game scene that is still set counts too: while the session is momentarily untracked (owner exited,
    // PRE only half done) a charger flap used to drop straight to the daily scene under the running game.
    // checkGameEnd() clears an orphaned game scene after ~15 s and applies the right scene itself.
    if (gameSessions() > 0 || gameStarting() || gameSceneActive()) {
        if (!deferred_) Q_EMIT finished(QString(), true, {QStringLiteral("power source changed — scene switch deferred until the game exits")});
        deferred_ = true;
        return;
    }
    deferred_ = false;
    const QString n = onAc ? auto_.onAc : auto_.onBattery;
    if (n.isEmpty()) return;
    if (busy_ && n == current_) {
        // The scene being applied right now is already the right one: drop a stale request for the other source
        // instead of applying it and then switching straight back.
        pending_.clear();
        pendingAuto_ = false;
        return;
    }
    nextAuto_ = true;
    apply(n);
}

void SceneEngine::apply(const QString &name) {
    if (busy_) { pending_ = name; pendingAuto_ = std::exchange(nextAuto_, false); return; }
    const auto s = load(name);
    if (!s) {
        nextAuto_ = false;
        Q_EMIT finished(name, false, {QStringLiteral("✗ scene '%1' not found").arg(name)});
        return;
    }
    start(*s);
}

/// The NVIDIA dGPU is on the bus, has a driver bound AND the driver actually
/// initialised it (/proc/driver/nvidia/gpus lists it; "nvidia-smi: No devices were
/// found" = empty). After iGPU-only / a firmware power cut the PCI function can stay
/// listed with the module loaded while the GPU is dead: every NVIDIA / WMI-GPU call
/// against it can then hang in the kernel (the helper sits in D state), so a scene
/// skips those steps instead of trying them. Directory listing only: no GPU wake-up.
static bool nvidiaUsable() {
    const QDir d(QStringLiteral("/sys/bus/pci/devices"));
    bool onBus = false;
    for (const QString &e : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System)) {
        auto rd = [&](const char *f) { QFile x(d.filePath(e) + '/' + QLatin1String(f)); return x.open(QIODevice::ReadOnly) ? x.readAll().trimmed() : QByteArray(); };
        if (rd("vendor") == "0x10de" && rd("class").startsWith("0x03") && QFileInfo::exists(d.filePath(e) + QStringLiteral("/driver"))
            && rd("power/runtime_status") != "error") { onBus = true; break; }
    }
    return onBus && !QDir(QStringLiteral("/proc/driver/nvidia/gpus")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty();
}

void SceneEngine::addStep(const QString &what, Step step) { steps_.append({what, std::move(step)}); }

void SceneEngine::helper(const QString &name, const QJsonObject &req, Done done,
                         std::function<QString(const QJsonObject &)> describe) {
    privileged::run(privileged::helperPath(name), req, this, [done, describe](const privileged::Result &r) {
        if (!r.ok()) { done(false, r.message().isEmpty() ? QStringLiteral("failed") : r.message()); return; }
        done(true, describe ? describe(r.json) : QString());
    }, 120000);
}

/// Applies a saved CPU profile (Ryzen Curve Optimizer or Intel undervolt, by vendor) straight from its
/// file through the helper. Used by the scene step and by the tray: nothing is loaded into the tab's
/// editor (unsaved values there stay as they are) and no dialog is ever opened.
/// `fromScene`: Intel only -- the profile is recorded for centurion-intel-uv-daemon (see intel_uv_daemon).
void SceneEngine::applyCpuProfile(const QString &name, bool fromScene, Done done) {
    if (win_->ryzen()) {
        QFile f(ryzen::profilesDir() + '/' + name + ".json");
        if (!f.open(QIODevice::ReadOnly) || f.size() > 256 * 1024) { done(false, "profile '" + name + "' not found"); return; }
        const QJsonObject d = QJsonDocument::fromJson(f.readAll()).object();
        const int ccds = std::max(1, ryzen::detect().ccdCount);
        QJsonArray entries;
        for (const auto &v : d.value("cores").toArray()) {
            const QJsonObject o = v.toObject();
            const int ccd = o.value("ccd").toInt(), ccx = o.value("ccx").toInt();
            const int slot = o.contains("slot") ? o.value("slot").toInt() : o.value("core").toInt();
            if (ccx != 0 || ccd >= ccds || o.value("disabled").toBool() || !o.value("coper").isDouble()) continue;
            entries.append(QJsonObject{{"ccd", ccd}, {"ccx", 0}, {"core", slot}, {"coper", o.value("coper").toInt()}});
        }
        const QJsonValue coall = d.value("coall");
        if (!coall.isDouble() && entries.isEmpty()) { done(false, "profile '" + name + "' has no offsets"); return; }
        auto perCore = [this, entries, done, name] {
            if (entries.isEmpty()) { done(true, "'" + name + "'"); return; }
            helper("ryzen-co-helper", {{"op", "set_coper_batch"}, {"params", QJsonObject{{"entries", entries}}}},
                   [done, name](bool ok, const QString &m) { done(ok, ok ? "'" + name + "'" : m); });
        };
        // All-core first, per-core on top — same order as the tab.
        if (coall.isDouble())
            helper("ryzen-co-helper", {{"op", "set_coall"}, {"params", QJsonObject{{"value", coall.toInt()}}}},
                   [perCore, done](bool ok, const QString &m) { if (ok) perCore(); else done(false, m); });
        else perCore();
        return;
    }
    if (win_->intel()) {
        QFile f(inteluv::profilesDir() + '/' + name + ".json");
        if (!f.open(QIODevice::ReadOnly) || f.size() > 64 * 1024) { done(false, "profile '" + name + "' not found"); return; }
        helper("intel-uv-helper", {{"op", "apply"}, {"hold", fromScene}, {"profile", QJsonDocument::fromJson(f.readAll()).object()}},
               [done, name](bool ok, const QString &m) { done(ok, ok ? "'" + name + "'" : m); });
        return;
    }
    done(false, "no CPU curve backend for this CPU");
}

void SceneEngine::start(const Scene &s) {
    busy_ = true;
    ok_ = true;
    log_.clear();
    steps_.clear();
    current_ = s.name;
    autoApply_ = std::exchange(nextAuto_, false);
    expectedProfile_ = s.platformProfile;
    verify_->stop();
    Q_EMIT started(s.name);

    // 0. EC fan boost FIRST: turning it off before the profile change means the fans never
    //    spin up for the new profile and then drop again.
    if (s.fanFullSpeed >= 0) {
        addStep("Fan boost", [this, on = s.fanFullSpeed == 1](Done done) {
            privileged::run(privileged::helperPath("legion-profile-helper"),
                            QJsonObject{{"device", "fan_fullspeed"}, {"value", on ? "1" : "0"}}, this,
                            [done, on](const privileged::Result &r) {
                                done(r.ok(), r.ok() ? QString(on ? "turbo (EC full speed)" : "auto") : r.message());
                            });
        });
    }

    // 1. Platform profile — first: firmware limits depend on it being Custom.
    //    The firmware may settle on another profile than the one asked for (some modes are refused on battery,
    //    or the EC overrides it a moment later). That used to be reported as a success under the requested
    //    name, with the Firmware tab told "profile = requested": now it is a problem line with the real profile.
    if (!s.platformProfile.isEmpty()) {
        addStep("Power profile", [this, p = s.platformProfile](Done done) {
            const auto h = pp::primaryHandler();
            QJsonObject req{{"profile", p}};
            if (h) req["handler"] = h->node;
            privileged::run(privileged::helperPath("legion-profile-helper"), req, this, [this, p, h, done](const privileged::Result &r) {
                if (!r.ok()) { done(false, r.message().isEmpty() ? QStringLiteral("failed") : r.message()); return; }
                const QString eff = r.json.value("effective").toString();
                auto settle = [this, p, done](const QString &now) {
                    Q_EMIT win_->home()->profileChanged(now);  // Firmware tab relocks now, not at its next poll
                    if (now == p) { done(true, HomeTab::profileLabel(p)); return; }
                    done(false, QStringLiteral("asked for %1 but the firmware is on %2 (that mode may not be available in this power state)")
                                    .arg(HomeTab::profileLabel(p), HomeTab::profileLabel(now)));
                };
                if (eff.isEmpty() || eff == p) { settle(p); return; }
                // The EC can still be settling: look once more before calling it a mismatch.
                QTimer::singleShot(1000, this, [h, eff, settle] {
                    const auto cur = pp::currentProfile(h);
                    settle(cur && !cur->isEmpty() ? *cur : eff);
                });
            }, 120000);
        });
    }

    // The EC may reset the flag on a profile change: re-assert only if it no longer matches.
    if (s.fanFullSpeed >= 0 && !s.platformProfile.isEmpty()) {
        addStep("Fan boost check", [this, on = s.fanFullSpeed == 1](Done done) {
            const QString h = privileged::helperPath("legion-profile-helper");
            privileged::run(h, QJsonObject{{"fan_fullspeed", "get"}}, this, [this, h, on, done](const privileged::Result &r) {
                if (!r.ok() || r.json.value("on").toBool() == on) { done(true, "unchanged"); return; }
                privileged::run(h, QJsonObject{{"device", "fan_fullspeed"}, {"value", on ? "1" : "0"}}, this,
                                [done](const privileged::Result &r2) { done(r2.ok(), r2.ok() ? QStringLiteral("re-applied after the profile change") : r2.message()); });
            });
        });
    }

    // 2. Firmware limits (sysfs attributes + the WMI-only GPU knobs).
    if (!s.firmware.isEmpty()) {
        addStep("Firmware limits", [this, fw = s.firmware](Done done) {
            if (pp::currentProfile(pp::primaryHandler()) != QStringLiteral("custom")) {
                done(false, "needs the Custom power profile (set it in this scene)");
                return;
            }
            QJsonArray batch;
            QJsonObject wmi;
            int unknown = 0;
            QMap<QString, FwAttr> byName;
            for (const FwAttr &a : FwattrTab::discover()) byName.insert(a.name, a);
            for (auto it = fw.cbegin(); it != fw.cend(); ++it) {
                const auto a = byName.constFind(it.key());
                if (a == byName.cend()) { ++unknown; continue; }
                if (a->viaWmi()) wmi[a->wmiKey] = it.value();
                else if (a->ranged) batch.append(QJsonObject{{"path", a->path}, {"value", it.value()}});
                else ++unknown;
            }
            const QString note = unknown ? QStringLiteral(" (%1 not present here)").arg(unknown) : QString();
            auto afterSysfs = [this, wmi, note, done, n = batch.size()](bool ok, const QString &msg) {
                if (!ok) { done(false, msg); return; }
                if (wmi.isEmpty()) { done(true, QStringLiteral("%1 value(s)").arg(n) + note); return; }
                if (!nvidiaUsable()) { done(true, QStringLiteral("%1 value(s); %2 GPU value(s) skipped (NVIDIA dGPU is off)").arg(n).arg(wmi.size()) + note); return; }
                helper("legion-gpu-helper", {{"op", "apply"}, {"values", wmi}}, [done, n, note, w = wmi.size()](bool ok, const QString &m) {
                    done(ok, ok ? QStringLiteral("%1 value(s) + %2 GPU (WMI)").arg(n).arg(w) + note : "GPU (WMI): " + m);
                });
            };
            if (batch.isEmpty()) { afterSysfs(true, {}); return; }
            privileged::run(privileged::helperPath("fwattr-helper"), QJsonDocument(batch).toJson(QJsonDocument::Compact), this,
                                [afterSysfs](const privileged::Result &r) {
                                    if (r.ok()) { afterSysfs(true, {}); return; }
                                    QStringList bad;
                                    for (const auto &x : r.json.value("results").toArray())
                                        if (!x.toObject().value("ok").toBool()) bad << x.toObject().value("error").toString();
                                    afterSysfs(false, bad.isEmpty() ? r.message() : bad.join("; "));
                                }, 120000);
        });
    }

    // 3. CPU curve: Ryzen Curve Optimizer or Intel undervolt, by vendor.
    if (s.cpu.kind != Choice::Unchanged) {
        if (win_->ryzen()) {
            addStep("CPU curve", [this, c = s.cpu](Done done) {
                if (c.kind == Choice::Reset) { helper("ryzen-co-helper", {{"op", "reset"}}, [done](bool ok, const QString &m) { done(ok, ok ? "reset (0)" : m); }); return; }
                applyCpuProfile(c.name, true, done);
            });
        } else if (win_->intel()) {
            addStep("CPU undervolt", [this, c = s.cpu](Done done) {
                if (c.kind == Choice::Reset) { helper("intel-uv-helper", {{"op", "reset"}, {"hold", true}}, [done](bool ok, const QString &m) { done(ok, ok ? "reset (0 mV)" : m); }); return; }
                applyCpuProfile(c.name, true, done);
            });
        }
    }

    // 4. NVIDIA V/F curve.
    const bool cpuStep = s.cpu.kind != Choice::Unchanged && (win_->ryzen() || win_->intel());
    if (s.gpu.kind != Choice::Unchanged) {
        addStep("GPU curve", [this, c = s.gpu, cpuStep](Done done) {
            // Always a log line: the step used to vanish without a word when the dGPU was
            // off at start-up (no NVIDIA tab), even after the card had been brought back.
            if (!nvidiaUsable()) { done(true, NvidiaTab::present() ? "skipped (NVIDIA dGPU is off)" : "skipped (no NVIDIA GPU on the bus)"); return; }
            win_->ensureNvidiaTab();  // the card came back after start-up
            // Same 2 s gap after a CPU curve as centurion-gamemode (UNDERVOLT_GAP).
            QTimer::singleShot(cpuStep ? 2000 : 0, this, [this, c, done] {
                // Two NvAPI sessions writing the ClockBoostTable at once is asking for trouble.
                if (win_->nvidia() && win_->nvidia()->busy()) { done(false, "the NVIDIA tab is busy; skipped"); return; }
                const QJsonObject req = c.kind == Choice::Reset ? QJsonObject{{"op", "reset_gpu_curve"}}
                                                                : QJsonObject{{"op", "apply_named_profile"}, {"name", c.name}};
                helper("nvcurve-root-helper", req, [done, c](bool ok, const QString &m) {
                    done(ok, ok ? (c.kind == Choice::Reset ? QStringLiteral("reset") : "'" + c.name + "'") : m);
                });
            });
        });
    }

    // 5. Optimizations preset — "replace": knobs the new preset does not set
    //    go back to their originals, and a running game's tuning is left alone.
    if (s.tuning.kind != Choice::Unchanged) {
        addStep("Optimizations", [this, c = s.tuning](Done done) {
            // By name: tune-profile-helper takes the values from the root-owned approved store, never from this request.
            QJsonObject req{{"op", "apply_preset"}, {"mode", "manual"}, {"replace", true}, {"preset", QJsonValue::Null}};
            auto run = [this, done, c](const QJsonObject &rq) {
                privileged::run(privileged::helperPath("tune-profile-helper"), rq, this, [done, c](const privileged::Result &r) {
                    if (r.reached && r.json.value("game_active").toBool()) { done(true, "left alone (game session active)"); return; }
                    if (!r.ok()) { done(false, r.message()); return; }
                    done(true, c.kind == Choice::Reset ? QStringLiteral("originals restored") : "'" + c.name + "'");
                }, 120000);
            };
            if (c.kind != Choice::Profile) { run(req); return; }
            const QJsonObject p = win_->optimize()->presetObject(c.name);
            if (p.isEmpty()) { done(false, "preset '" + c.name + "' not found"); return; }
            req["preset"] = c.name;
            const QJsonObject values = p.value("values").toObject();
            if (OptimizeTab::storeFresh(c.name, values)) { run(req); return; }
            // First use, or the preset changed since it was approved (a built-in updated with the
            // program): approve (store root-owned) once, then apply by name.
            privileged::run(privileged::helperPath("tune-helper"),
                            QJsonObject{{"op", "preset_save"}, {"name", c.name}, {"values", values}}, this,
                            [run, req, done, name = c.name, values](const privileged::Result &r) {
                                if (!r.ok()) { done(false, r.message()); return; }
                                OptimizeTab::noteApproved(name, values);
                                run(req);
                            }, 120000);
        });
    }

    // 5a. Custom-mode fan curve — only while the Custom power profile is active (the EC follows
    //     the table there and nowhere else); skipped with a note otherwise, not an error.
    if (s.fanTable.size() == 10) {
        addStep("Fan curve", [this, lv = s.fanTable](Done done) {
            if (pp::currentProfile(pp::primaryHandler()) != QStringLiteral("custom")) {
                done(true, "skipped (the EC uses its own curve outside the Custom power profile)");
                return;
            }
            QJsonArray a;
            for (int x : lv) a << x;
            helper("legion-profile-helper", QJsonObject{{"fan_table", "set"}, {"levels", a}}, done,
                   [](const QJsonObject &) { return QStringLiteral("table written and verified"); });
        });
    }

    // 6. Keyboard lighting — as the user when the udev rule allows it, pkexec otherwise.
    if (s.lightProfile >= 0 || s.lightBrightness >= 0) {
        addStep("Lighting", [this, p = s.lightProfile, b = s.lightBrightness](Done done) {
            if (!lighting::present()) { done(true, "no Spectrum keyboard here; skipped"); return; }
            QJsonObject req{{"op", "set"}};
            if (p >= 0) req["profile"] = p;
            if (b >= 0) req["brightness"] = b;
            lighting::run(req, this, [this, done](const privileged::Result &r) {
                if (!r.ok()) { done(false, r.message().isEmpty() ? QStringLiteral("failed") : r.message()); return; }
                if (LightingTab *lt = win_->lighting()) lt->refreshIfClean();
                done(true, QStringLiteral("profile %1 · brightness %2").arg(r.json.value("profile").toInt())
                               .arg(r.json.value("brightness").toInt()));
            });
        });
    }

    // 7. User command (display mode, audio profile…) — as the user, no shell.
    if (!s.command.isEmpty()) {
        addStep("Command", [cmd = s.command](Done done) {
            QStringList argv = QProcess::splitCommand(cmd);
            if (argv.isEmpty()) { done(false, "empty command"); return; }
            const QString prog = argv.takeFirst();
            done(QProcess::startDetached(prog, argv), prog);
        });
    }

    if (steps_.isEmpty()) log_ << QStringLiteral("nothing to change (every component is 'unchanged')");
    next();
}

void SceneEngine::next() {
    // An automatic (power-source) switch that is already running when a game starts used to carry on
    // with its remaining steps next to centurion-gamemode PRE: both write the power profile, limits and tuning,
    // so the machine ended up half in the daily scene and half in the game scene. The game wins.
    if (autoApply_ && !steps_.isEmpty() && (gameSessions() > 0 || gameStarting() || gameSceneActive())) {
        log_ << QStringLiteral("a game started — the remaining steps of this automatic switch were skipped");
        steps_.clear();
        gameTookOver_ = true;
    }
    if (steps_.isEmpty()) { finish(); return; }
    const auto [what, step] = steps_.takeFirst();
    step([this, what = what](bool ok, const QString &msg) {
        ok_ &= ok;
        log_ << (ok ? QString() : QStringLiteral("✗ ")) + what + (msg.isEmpty() ? QString() : QStringLiteral(": ") + msg);
        // Queued: a step may complete synchronously; never recurse through the chain.
        QTimer::singleShot(0, this, &SceneEngine::next);
    });
}

void SceneEngine::finish() {
    busy_ = false;
    // Never record the daily scene as active over a running game's scene (the GUI reads this file to decide
    // what is applied), and never run a queued automatic switch under a game that started meanwhile.
    const bool gameOwns = gameTookOver_ || (autoApply_ && (gameSessions() > 0 || gameStarting() || gameSceneActive()));
    gameTookOver_ = false;
    if (!gameOwns) { setActiveScene(current_); seenActive_ = current_; }
    Q_EMIT finished(current_, ok_, log_);
    if (gameOwns) {
        pending_.clear();
        pendingAuto_ = false;
        deferred_ = true;  // checkGameEnd() applies the right scene when the game ends
        return;
    }
    if (!pending_.isEmpty()) {
        const QString n = std::exchange(pending_, QString());
        nextAuto_ = std::exchange(pendingAuto_, false);
        if (nextAuto_ && (gameSessions() > 0 || gameStarting() || gameSceneActive())) { nextAuto_ = false; deferred_ = true; return; }
        apply(n);
        return;
    }
    armVerify();
}

/// After an automatic switch the EC may still change the platform profile on its own (it re-evaluates the
/// power budget when the charger comes or goes). One look a few seconds later, one re-apply if it did.
void SceneEngine::armVerify() {
    if (!autoApply_ || expectedProfile_.isEmpty()) return;
    verifyTriesLeft_ = 1;
    verify_->start(VERIFY_DELAY_MS);
}

void SceneEngine::verifyPass() {
    if (verifyTriesLeft_ <= 0 || busy_ || !auto_.enabled || auto_.paused || expectedProfile_.isEmpty()) return;
    if (gameSessions() > 0 || gameStarting() || activeScene() != current_) return;  // someone else (a game scene) owns the machine now
    const auto cur = pp::currentProfile(pp::primaryHandler());
    if (!cur || cur->isEmpty() || *cur == expectedProfile_) return;
    --verifyTriesLeft_;
    Q_EMIT finished(QString(), true, {QStringLiteral("the firmware changed the power profile to %1 after the switch — applying '%2' again")
                                          .arg(HomeTab::profileLabel(*cur), current_)});
    nextAuto_ = false;  // one retry only: it is not verified again
    apply(current_);
}
