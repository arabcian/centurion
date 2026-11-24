#include "cosearchdialog.h"
#include "coarchive.h"
#include <QUuid>
#include "coinsights.h"
#include "notify.h"
#include "curvepage.h"
#include "mainwindow.h"
#include "privileged.h"
#include "ryzentab.h"
#include "scenes.h"
#include "theme.h"
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QMap>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QProgressBar>
#include <QSplitter>
#include <QSettings>
#include <QStandardPaths>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <memory>
#include <algorithm>

using namespace cosearch;

static QString stateDir() {
    return (qEnvironmentVariableIsEmpty("XDG_STATE_HOME") ? QDir::homePath() + "/.local/state"
                                                           : qEnvironmentVariable("XDG_STATE_HOME")) + "/centurion";
}
/// Written before every trial, removed when the search ends: still there at the next start = it was interrupted.
static QString journalPath() { return stateDir() + "/co-search-session.json"; }
static QString crashGuardPath() { return stateDir() + "/co-search-crash-guards.json"; }
static QString allCoreGuardPath() { return stateDir() + "/co-search-allcore-guard.json"; }
static QString settingsPath() { return QFileInfo(scenes::dir()).path() + "/co-search.json"; }
/// BIOS version the firmware reports; CO behaviour changes with the AGESA a BIOS update brings.
static QString biosVersion() {
    QFile f(QStringLiteral("/sys/class/dmi/id/bios_version"));
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.read(128)).trimmed() : QString();
}
/// Rated top clock of a CPU in MHz (cpufreq cpuinfo_max_freq; amd-pstate: its highest-performance clock), 0 = unknown.
static int ratedMhz(int cpu) {
    QFile f(QStringLiteral("/sys/devices/system/cpu/cpu%1/cpufreq/cpuinfo_max_freq").arg(cpu));
    return f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.read(32)).trimmed().toInt() / 1000 : 0;
}
static QString readBootId() {
    QFile boot(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    return boot.open(QIODevice::ReadOnly) ? QString::fromUtf8(boot.readAll()).trimmed() : QString();
}

// The in-flight trial is written AFTER Search::next(), not reconstructed from a pre-trial
// snapshot. The signature allows rejecting corrupt/stale journals before applying any CO.
static QJsonObject trialSignature(const Trial &t) {
    QJsonArray per;
    for (int x : t.perCore) per << x;
    return QJsonObject{{"what", t.what},
                       {"all", t.allCore ? QJsonValue(*t.allCore) : QJsonValue(QJsonValue::Null)},
                       {"per", per}, {"seconds", t.seconds()}};
}

static std::optional<QJsonArray> readCrashGuards() {
    QFile f(crashGuardPath());
    if (!f.exists()) return QJsonArray{};
    // A damaged/unreadable guard MUST NOT silently become an empty guard list.
    if (!f.open(QIODevice::ReadOnly) || f.size() > (1LL << 20)) return std::nullopt;
    QJsonParseError err;
    const QJsonDocument d = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !d.isObject()) return std::nullopt;
    const QJsonObject j = d.object();
    if (j.value("version").toInt() != 1 || !j.value("guards").isArray()) return std::nullopt;
    const QJsonArray guards = j.value("guards").toArray();
    if (guards.size() > 512) return std::nullopt;
    for (const QJsonValue &v : guards) {
        if (!v.isObject()) return std::nullopt;
        const QJsonObject g = v.toObject();
        if (g.value("ccd").toInt(-1) < 0 || g.value("slot").toInt(-1) < 0 ||
            g.value("failed").toInt(1) >= 0 || g.value("failed").toInt(-99) < -50 ||
            !g.value("cpus").isArray() || g.value("cpus").toArray().isEmpty()) return std::nullopt;
    }
    return guards;
}

/// The all-core offset that crashed this machine: {"version", "failed", "bios"}, an empty object when none is
/// recorded; nullopt when the file cannot be read (like the per-core guards: never read as "no crash").
static std::optional<QJsonObject> readAllCoreGuard() {
    QFile f(allCoreGuardPath());
    if (!f.exists()) return QJsonObject{};
    if (!f.open(QIODevice::ReadOnly) || f.size() > (64LL << 10)) return std::nullopt;
    const QJsonObject j = QJsonDocument::fromJson(f.readAll()).object();
    const int failed = j.value("failed").toInt(1);
    if (j.value("version").toInt() != 1 || !j.value("failed").isDouble() || failed >= 0 || failed < -50) return std::nullopt;
    return j;
}

// Retain failed candidates across NEW searches too, so "start again" cannot silently
// rediscover the same destructive offset. A record is specific to the physical topology.
static bool saveCrashGuard(const Search &s, const Trial &t) {
    if (t.allCore.has_value()) {
        // One offset on every core (descent or confirmation): guarded for all-core searches. The least negative
        // crash is kept. A per-core probe tests one core alone and is not limited by it.
        const auto old = readAllCoreGuard();
        if (!old) return false;
        const int failed = old->isEmpty() ? *t.allCore : std::max(*t.allCore, old->value("failed").toInt());
        if (failed >= 0) return true;
        QDir().mkpath(stateDir());
        QSaveFile f(allCoreGuardPath());
        const QByteArray data = QJsonDocument(QJsonObject{{"version", 1}, {"failed", failed}, {"bios", biosVersion()}}).toJson();
        return f.open(QIODevice::WriteOnly) && f.write(data) == data.size() && f.commit();
    }
    if (t.perCore.size() != s.cores().size()) return true;
    int suspect = -1;
    for (int i = 0; i < t.perCore.size(); ++i) {
        if (t.perCore[i] >= 0) continue;
        if (suspect >= 0) return true;  // combined confirmation: no unambiguous culprit
        suspect = i;
    }
    if (suspect < 0) return true;
    const Core &c = s.cores()[suspect];
    const auto old = readCrashGuards();
    if (!old) return false;
    QJsonArray logical;
    for (int cpu : c.cpus) logical << cpu;
    QJsonArray fresh;
    int mostConservative = t.perCore[suspect];
    for (const QJsonValue &v : *old) {
        const QJsonObject item = v.toObject();
        if (item.value("ccd").toInt(-1) == c.ccd && item.value("slot").toInt(-1) == c.slot) {
            mostConservative = std::max(mostConservative, item.value("failed").toInt(-50));
        } else fresh << item;
    }
    fresh << QJsonObject{{"ccd", c.ccd}, {"slot", c.slot}, {"cpus", logical},
                         {"failed", mostConservative}, {"bios", biosVersion()}};
    QDir().mkpath(stateDir());
    QSaveFile f(crashGuardPath());
    if (!f.open(QIODevice::WriteOnly)) return false;
    const QByteArray data = QJsonDocument(QJsonObject{{"version", 1}, {"guards", fresh}}).toJson();
    return f.write(data) == data.size() && f.commit();
}

/// Recorded crash guards by coreKey(); empty when there are none or the file is unreadable.
static int coreKey(int ccd, int slot) { return ccd * 1000 + slot; }
static constexpr int DEFAULT_START = -15;  // per-core Start of a core with no typed / preset value
static QHash<int, int> crashGuardMap() {
    QHash<int, int> out;
    for (const QJsonValue &v : readCrashGuards().value_or(QJsonArray{})) {
        const QJsonObject o = v.toObject();
        out.insert(coreKey(o.value("ccd").toInt(), o.value("slot").toInt()), o.value("failed").toInt());
    }
    return out;
}

/// Physical cores that can be searched (parked CCDs left out), in CCD / slot order.
static QList<Core> coresOf(const ryzen::Layout &layout) {
    QList<Core> cores;
    for (auto it = layout.cores.cbegin(); it != layout.cores.cend(); ++it) {
        if (layout.parked.contains(it.key())) continue;
        for (int i = 0; i < it.value().size(); ++i)
            cores << Core{it.key(), i, it.value()[i].cpus, it.value()[i].highestPerf.value_or(-1)};
    }
    return cores;
}

static bool applyCrashGuards(Search &s) {
    const auto guards = readCrashGuards();
    const auto all = readAllCoreGuard();
    if (!guards || !all) return false;
    if (!all->isEmpty()) s.guardAllCore(all->value("failed").toInt());
    for (const QJsonValue &v : *guards) {
        const QJsonObject o = v.toObject();
        QList<int> cpus;
        for (const QJsonValue &cpu : o.value("cpus").toArray()) cpus << cpu.toInt(-1);
        s.guardAgainstCrash(o.value("ccd").toInt(-1), o.value("slot").toInt(-1), cpus,
                            o.value("failed").toInt(0));
    }
    return true;
}

/// One coloured log line; `stamp` empty = indented continuation line (no time).
static void logLine(QPlainTextEdit *log, const QString &stamp, const QString &text, const char *color) {
    log->appendHtml(QStringLiteral("<span style=\"color:%1;\">%2</span><span style=\"color:%3; white-space:pre;\">%4</span>")
        .arg(theme::MUTED, stamp.isEmpty() ? QStringLiteral("          ") : stamp + QStringLiteral("  "), color,
             text.toHtmlEscaped().replace('\n', "<br>")));
}

static QString statusSheet(const char *tone) {
    return theme::banner(tone ? tone : theme::MUTED, "#coStatus") + QStringLiteral(" #coStatus { padding: 5px 10px; }");
}

CoSearchDialog::CoSearchDialog(QWidget *parent) : QDialog(parent), notifier_(new Notifier(this)) {
    setWindowTitle(tr("Ryzen CO Search Assistant (Experimental)"));
    resize(1000, 680);
    auto *v = new QVBoxLayout(this);
    auto *explainer = new QLabel(tr(
        "Finds negative Curve Optimizer offsets with FFT / integer workload and boost/idle transitions. "
        "All-core runs 60-second cycles (40 s steady + 20 s transitions) to find one "
        "offset for every core, from its Start. Per-core tests one core at a time (the others at stock) from that "
        "core's Start (typed in the table, or taken from a saved preset), going more negative by your step; the "
        "first failure ends that core and keeps its last passing value, and nothing as negative as the failed "
        "value is tried on it again. If a core's Start fails, it climbs back towards stock by the step. A value "
        "that crashed the machine is never tried on that core again, in any later search either. Untick a core to "
        "leave it at stock. The final per-core set is then verified for 20 minutes with the whole curve applied, "
        "single-core loads alternating with all-core loads up to the power limit. "
        "The curve is reset to stock when the search ends; the result is saved as a Ryzen Curve profile, not applied. "
        "Passing is NOT proof of long-term stability. A hard freeze may still require rebooting: the search can resume after it."));
    explainer->setWordWrap(true);
    explainer->setObjectName("coExplainer");
    explainer->setStyleSheet(theme::banner(theme::INFO, "#coExplainer") + QStringLiteral(" #coExplainer { padding: 6px 10px; }"));
    v->addWidget(explainer);

    auto *form = new QFormLayout;
    auto *row1 = new QHBoxLayout;
    mode_ = new QComboBox;
    mode_->addItem(tr("All-core"), int(Mode::AllCore));
    mode_->addItem(tr("Per-core"), int(Mode::PerCore));
    mode_->setCurrentIndex(mode_->findData(int(Mode::PerCore)));  // new installs prefer direct isolated searches
    level_ = new QComboBox;
    level_->addItem(tr("Fast"), name(Level::Fast));
    level_->addItem(tr("Medium"), name(Level::Medium));
    level_->addItem(tr("Long"), name(Level::Long));
    level_->setToolTip(tr("Per-core: 24-second probes (12 s steady + all 10 boost/idle patterns), then a "
                          "20-minute verification of the whole set at every level.\n"
                          "Fast: 1 probe cycle. Medium: 2 probe cycles. Long: 3 probe cycles.\n"
                          "All-core trials use full 60-second cycles; its confirmation is 1 / 2 / 3 cycles per core "
                          "and 1 / 2 / 5 all-core cycles."));
    limit_ = new QSpinBox;
    limit_->setRange(-50, -5);
    limit_->setValue(-30);
    limit_->setToolTip(tr("Most negative offset to try. The CPU's own allowed range (ryzen-co-helper) still applies."));
    row1->addWidget(mode_);
    row1->addWidget(new QLabel(tr("Level:")));
    row1->addWidget(level_);
    row1->addWidget(new QLabel(tr("Limit:")));
    row1->addWidget(limit_);
    allStart_ = new QSpinBox;
    allStart_->setRange(-50, -1);
    allStart_->setValue(-5);
    allStart_->setToolTip(tr("All-core: the first offset tried; then steps of 5, and steps of 1 after a failure. "
                             "Per-core uses each core's own Start in the table instead."));
    row1->addWidget(new QLabel(tr("All-core start:")));
    row1->addWidget(allStart_);
    row1->addStretch();
    form->addRow(tr("Search:"), row1);
    auto *tuningRow = new QHBoxLayout;
    preset_ = new QComboBox;
    preset_->setToolTip(tr("Fill every core's Start from a saved Ryzen Curve profile (per-core values; an all-core value "
                           "for cores without one; disabled slots are unticked). You can still edit single cores."));
    step_ = new QSpinBox;
    step_->setRange(1, 10);
    step_->setValue(1);
    step_->setToolTip(tr("CO points to go more negative after each passing probe (and back towards stock after a failed "
                         "Start). Default 1: the result is the exact last passing value. A larger step is faster, but "
                         "the result can be up to step - 1 points more conservative than the core could hold."));
    applyTuning_ = new QPushButton(tr("Apply to next trials"));
    applyTuning_->setToolTip(tr("You can change the step during an active per-core search. It affects only future candidates, never the test already running or stored reboot exclusions."));
    applyTuning_->setEnabled(false);
    tuningRow->addWidget(new QLabel(tr("Preset:")));
    tuningRow->addWidget(preset_, 1);
    tuningRow->addWidget(new QLabel(tr("Step:")));
    tuningRow->addWidget(step_);
    tuningRow->addWidget(applyTuning_);
    tuningRow->addStretch();
    form->addRow(tr("Per-core controls:"), tuningRow);
    auto *row2 = new QHBoxLayout;
    scene_ = new QComboBox;
    scene_->setToolTip(tr("Scene applied for the search: set it to the power level you will use every day, since the\n"
                          "offsets a CPU holds depend on the clocks and temperatures that level gives. Scenes that apply\n"
                          "a CPU curve profile are not offered. The previous scene is applied again afterwards."));
    row2->addWidget(scene_, 1);
    form->addRow(tr("CO search scene:"), row2);
    v->addLayout(form);
    estimate_ = new QLabel;
    estimate_->setProperty("role", "muted");
    estimate_->setWordWrap(true);
    v->addWidget(estimate_);
    coreProgress_ = new QProgressBar(this);
    coreProgress_->setAccessibleName(tr("Completed search cores"));
    coreProgress_->setFormat(tr("Core search: %v / %m"));
    coreProgress_->hide();
    v->addWidget(coreProgress_);

    auto *row = new QHBoxLayout;
    row->addStretch();
    saveResult_ = new QPushButton(tr("Save result as profile…"));
    saveResult_->setToolTip(tr("Save the table's Result column as a named Ryzen Curve profile. It is listed under Ryzen Curve "
                               "and in Preset here. A search that did not finish its verification is saved as not verified."));
    loadResult_ = new QPushButton(tr("Use result as Start"));
    loadResult_->setToolTip(tr("Copy the Result column into the Start column (per-core), to continue from it in the next search."));
    verifyBtn_ = new QPushButton(tr("Verify a profile…"));
    verifyBtn_->setToolTip(tr("Run only the 20-minute verification (per-core profile) or the confirmation (all-core) on a saved "
                              "Ryzen Curve profile, without searching: after a BIOS update, or when the weather got warmer. "
                              "A failing core backs off as in a search; the result is saved as a new profile."));
    guardsBtn_ = new QPushButton(tr("Crash guards…"));
    guardsBtn_->setToolTip(tr("Values that crashed this machine are never tested again on that core. List them, with the BIOS "
                              "they were recorded under, and remove ones you no longer trust (e.g. after a BIOS update)."));
    go_ = new QPushButton(tr("Start search"));
    go_->setObjectName("btnAccent");
    stop_ = new QPushButton(tr("Stop and reset"));
    stop_->setObjectName("btnDanger");
    stop_->setEnabled(false);
    auto *mapToggle = new QPushButton(tr("CCD overview"));
    mapToggle->setCheckable(true);
    auto *mapHost = new QWidget;
    coreMap_ = new QGridLayout(mapHost);
    coreMap_->setContentsMargins(0, 0, 0, 0);
    mapHost->hide();
    connect(mapToggle, &QPushButton::toggled, mapHost, &QWidget::setVisible);
    auto *history = new QPushButton(tr("Trial history…"));
    connect(history, &QPushButton::clicked, this, &CoSearchDialog::showAttemptHistory);
    row->addWidget(mapToggle);
    row->addWidget(history);
    auto *archive = new QPushButton(tr("Session archive…"));
    connect(archive, &QPushButton::clicked, this, &CoSearchDialog::showSessionArchive);
    row->addWidget(archive);
    row->addWidget(guardsBtn_);
    row->addWidget(verifyBtn_);
    row->addSpacing(12);
    row->addWidget(saveResult_);
    row->addWidget(loadResult_);
    v->addLayout(row);
    auto *actions = new QHBoxLayout;
    actions->addStretch();
    actions->addWidget(go_);
    actions->addWidget(stop_);
    v->addLayout(actions);
    status_ = new QLabel(tr("Not started. The current CPU curve must be stock."));
    status_->setWordWrap(true);
    status_->setObjectName("coStatus");
    status_->setStyleSheet(statusSheet(nullptr));
    v->addWidget(status_);
    v->addWidget(mapHost);
    table_ = new QTableWidget(0, 5);
    table_->setHorizontalHeaderLabels({tr("Core"), tr("Start"), tr("Result"), tr("Status"), tr("Notes")});
    table_->setToolTip(tr("Per-core: Start is the core's first probe (0 = one step below stock). Untick a core to leave "
                          "it at stock (0) for the whole search and in the saved result."));
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->hide();
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setAccessibleName(tr("CO search activity log"));
    auto *split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(table_);
    split->addWidget(log_);
    split->setChildrenCollapsible(false);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    const QString splitterKey = QStringLiteral("ui/co_search_splitter");
    QSettings prefs(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("centurion"), QStringLiteral("gui"));
    split->restoreState(prefs.value(splitterKey).toByteArray());
    connect(split, &QSplitter::splitterMoved, split, [split, splitterKey] {
        QSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("centurion"), QStringLiteral("gui")).setValue(splitterKey, split->saveState());
    });
    v->addWidget(split, 1);

    loadSettings();
    for (QComboBox *c : {mode_, level_}) connect(c, &QComboBox::currentIndexChanged, this, &CoSearchDialog::updateEstimate);
    for (QSpinBox *spin : {limit_, allStart_, step_})
        connect(spin, &QSpinBox::valueChanged, this, &CoSearchDialog::updateEstimate);
    connect(preset_, &QComboBox::activated, this, [this] {
        if (running_ || preset_->currentData().toString().isEmpty()) return;
        applyPreset(preset_->currentData().toString());
    });
    connect(mode_, &QComboBox::currentIndexChanged, this, [this] { if (!running_) refreshTable(); });
    connect(applyTuning_, &QPushButton::clicked, this, &CoSearchDialog::applyTuning);
    connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *it) {
        if (running_ || it->column() != 0 || !(it->flags() & Qt::ItemIsUserCheckable)) return;
        const int key = it->data(Qt::UserRole).toInt();
        if (it->checkState() == Qt::Checked) excluded_.remove(key);
        else excluded_.insert(key);
        saveSettings();
        updateEstimate();
    });
    updateEstimate();
    refreshTable();

    guard_ = new QTimer(this);
    guard_->setInterval(1000);
    connect(guard_, &QTimer::timeout, this, [this] {
        if (running_) updateEstimate();  // elapsed / left
        if (!running_ || resetting_ || sceneApplying_) return;
        if (scenes::onAc() == std::optional<bool>(false)) {
            note(tr("Charger disconnected; stopping the search and resetting the curve."), theme::WARN); cancel(); return;
        }
        if (scenes::gameSessions() > 0 || scenes::activeScene() != startScene_) {
            note(tr("Scene or game tuning ownership changed; cancelling search."), theme::WARN); cancel(); return;
        }
        if (!applying_ && trial_ && CurvePage::currentState("cpu") != expectedCurve_) {
            note(tr("CPU curve was changed externally during this trial; cancelling."), theme::WARN); cancel(); return;
        }
        if (test_ && test_->state() != QProcess::NotRunning && QDateTime::currentSecsSinceEpoch() > testDeadline_) {
            note(tr("CPU test exceeded safety timeout; terminating."), theme::WARN); cancel();
        }
    });
    watch_ = new QTimer(this);
    watch_->setInterval(3000);
    connect(watch_, &QTimer::timeout, this, &CoSearchDialog::watchTick);
    connect(saveResult_, &QPushButton::clicked, this, &CoSearchDialog::saveResultAs);
    connect(loadResult_, &QPushButton::clicked, this, &CoSearchDialog::loadResultIntoStarts);
    updateResultButtons();
    connect(go_, &QPushButton::clicked, this, &CoSearchDialog::start);
    connect(verifyBtn_, &QPushButton::clicked, this, &CoSearchDialog::verifyProfile);
    connect(guardsBtn_, &QPushButton::clicked, this, &CoSearchDialog::manageCrashGuards);
    connect(stop_, &QPushButton::clicked, this, &CoSearchDialog::cancel);
    QTimer::singleShot(0, this, &CoSearchDialog::offerResume);
}

/// Live AMD MCA threshold counters (machinecheckN/<bank>/<block>/error_count, mode 0444): the kernel
/// reads the bank's MISC MSR on every read, so a corrected error shows here at once, while the kernel
/// itself may only log it at the next MCE poll (check_interval, 5 min by default).
static QHash<QString, QString> mceCounters() {
    QHash<QString, QString> out;
    QDirIterator it(QStringLiteral("/sys/devices/system/machinecheck"), {QStringLiteral("error_count")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext() && out.size() < 4096) {
        const QString p = it.next();
        QFile f(p);
        if (f.open(QIODevice::ReadOnly)) out.insert(p, QString::fromLatin1(f.read(32)).trimmed());
    }
    return out;
}

/// Blocking stock reset, for when the dialog is destroyed mid-search (application quit): the
/// asynchronous privileged::run could not report back any more.
static bool blockingReset() {
    QString pkexec;
    for (const char *c : {"/usr/bin/pkexec", "/bin/pkexec"})
        if (QFileInfo(QString::fromLatin1(c)).isFile()) { pkexec = QString::fromLatin1(c); break; }
    if (pkexec.isEmpty()) return false;
    QProcess p;
    p.start(pkexec, {privileged::helperPath("ryzen-co-helper")});
    if (!p.waitForStarted(3000)) return false;
    p.write(QJsonDocument(QJsonObject{{"op", "reset"}}).toJson(QJsonDocument::Compact) + '\n');
    p.closeWriteChannel();
    if (!p.waitForFinished(15000)) return false;
    const QByteArray out = p.readAllStandardOutput().trimmed();
    return QJsonDocument::fromJson(out.mid(out.lastIndexOf('\n') + 1)).object().value("ok").toBool();
}

CoSearchDialog::~CoSearchDialog() {
    if (perfTest_) {  // a throughput run (with the curve, or the stock run after it): its handler must not run now
        perfTest_->disconnect(this);
        perfTest_->kill();
        perfTest_->waitForFinished(2000);
    }
    if (!running_ || resetting_) return;  // idle, or the asynchronous reset is already on its way
    guard_->stop();
    if (test_) {
        test_->disconnect(this);
        test_->kill();
        test_->waitForFinished(2000);
    }
    const bool inFlight = applying_;
    bool ok = blockingReset();
    // An SMU write still in flight may land after that reset (ryzen-co-helper serialises on its lock):
    // reset once more after it has had time to finish.
    if (inFlight) { QThread::msleep(1500); ok = blockingReset() && ok; }
    // A quit is not a crash: with the curve confirmed at stock, the trial in flight is marked as cut short
    // (still pending, so a reboot before the next start cannot hide it) and a resume runs it again.
    // Between two trials (trial_ empty) the in-memory state is already resolved: store it as such.
    if (ok && search_) {
        cleanExit_ = trial_.has_value();
        writeJournal();
    }
    quitting_ = true;
    done(ok, tr("Application closed during search"));
}

void CoSearchDialog::reject() {
    if (running_) { cancel(); return; }
    QDialog::reject();
}

void CoSearchDialog::closeEvent(QCloseEvent *e) {
    if (running_) {
        cancel();
        e->ignore();  // root writes and reset must finish before this controller can be destroyed
    } else QDialog::closeEvent(e);
}

void CoSearchDialog::note(const QString &s, const char *tone) {
    logLine(log_, QDateTime::currentDateTime().toString("HH:mm:ss"), s, tone ? tone : theme::FG_DIM);
    status_->setText(s);
    theme::setSheet(status_, statusSheet(tone));
}

Tuning CoSearchDialog::tuning() const {
    return {allStart_->value(), step_->value()};
}

void CoSearchDialog::updateEstimate() {
    const auto mode = Mode(mode_->currentData().toInt());
    const bool perCore = mode == Mode::PerCore;
    step_->setEnabled(perCore);
    preset_->setEnabled(perCore && !running_);
    allStart_->setEnabled(!perCore && !running_);
    // The previous CPU curve may still be applied between finishTrial() and step().
    // Do not replace its pending journal with a non-pending snapshot in that gap.
    applyTuning_->setEnabled(perCore && running_ && !resetting_ && !abort_ &&
                             search_.has_value() && search_->perCoreSearching() && trial_.has_value());
    coreProgress_->setVisible(running_ && search_ && search_->mode() == Mode::PerCore);
    if (running_ && search_) {
        // Live progress: per-core from the pace of the cores finished since launch(), otherwise the launch estimate.
        int done = 0, total = 0;
        for (int i = 0; i < search_->cores().size(); ++i) {
            if (search_->mode() == Mode::PerCore && search_->coreState(i).excluded) continue;
            ++total;
            done += search_->mode() == Mode::PerCore ? search_->coreState(i).done : search_->finished();
        }
        coreProgress_->setVisible(perCore && total > 0);
        coreProgress_->setRange(0, std::max(1, total));
        coreProgress_->setValue(done);
        const int elapsed = int((QDateTime::currentSecsSinceEpoch() - searchStart_) / 60);
        const int left = search_->mode() == Mode::PerCore && search_->perCoreSearching()
            ? coinsights::etaMinutes(elapsed, done - doneAtLaunch_, total - doneAtLaunch_, estimateAtLaunch_, true)
            : std::max(0, estimateAtLaunch_ - elapsed);
        estimate_->setText(tr("Trial %1 · %2 / %3 cores done · %4 h %5 min elapsed · about %6 h %7 min left (estimate)")
            .arg(trialNo_).arg(done).arg(total).arg(elapsed / 60).arg(elapsed % 60, 2, 10, QLatin1Char('0'))
            .arg(left / 60).arg(left % 60, 2, 10, QLatin1Char('0')));
        return;
    }
    const auto level = levelFrom(level_->currentData().toString()).value_or(Level::Fast);
    int cores = 0;
    QList<int> starts;
    for (const Core &c : coresOf(ryzen::detect())) {
        if (perCore && excluded_.contains(coreKey(c.ccd, c.slot))) continue;
        ++cores;
        starts << starts_.value(coreKey(c.ccd, c.slot), DEFAULT_START);
    }
    const int m = Search::estimateMinutes(mode, level, std::max(cores, 1), limit_->value(), tuning(), starts);
    estimate_->setText(tr("Roughly %1 h %2 min for %3 cores (depends on where the offsets fail).").arg(m / 60).arg(m % 60, 2, 10, QLatin1Char('0')).arg(cores));
}

void CoSearchDialog::applyTuning() {
    if (!running_ || resetting_ || abort_ || !search_ || !search_->perCoreSearching()) return;
    if (!trial_) { note(tr("Changing steps is only available while a candidate is pending.")); return; }
    const Tuning previous = search_->tuning();
    if (!search_->setTuning(tuning())) return;
    if (!writeJournal()) {
        search_->setTuning(previous);
        step_->setValue(previous.step);
        note(tr("Could not save updated CO steps; previous safe tuning retained."), theme::DANGER);
        return;
    }
    saveSettings();
    note(tr("Updated future per-core trials: step %1. Current test unchanged.").arg(tuning().step), theme::INFO);
}

void CoSearchDialog::reloadPresets() {
    const QString keep = preset_->currentData().toString();
    const QSignalBlocker block(preset_);
    preset_->clear();
    preset_->addItem(tr("(manual: edit Start in the table)"), QString());
    for (const QString &f : QDir(ryzen::profilesDir()).entryList({"*.json"}, QDir::Files, QDir::Name)) {
        const QString n = f.chopped(5);
        preset_->addItem(n, n);
    }
    preset_->setCurrentIndex(std::max(0, preset_->findData(keep)));
}

void CoSearchDialog::applyPreset(const QString &name) {
    // Same file format as Ryzen Curve → Save Profile (see RyzenTab::loadProfile).
    QFile f(ryzen::profilesDir() + '/' + name + ".json");
    QJsonParseError pe;
    const QJsonDocument doc = f.open(QIODevice::ReadOnly) && f.size() <= 256 * 1024
        ? QJsonDocument::fromJson(f.readAll(), &pe) : QJsonDocument();
    if (!doc.isObject()) {
        note(tr("Preset '%1' could not be read; the Start values were not changed.").arg(name), theme::DANGER);
        preset_->setCurrentIndex(0);
        return;
    }
    const QJsonObject d = doc.object();
    QHash<int, int> values;
    QSet<int> disabled;
    for (const QJsonValue &v : d.value("cores").toArray()) {
        const QJsonObject c = v.toObject();
        if (c.value("ccx").toInt() != 0) continue;
        const int key = coreKey(c.value("ccd").toInt(), c.contains("slot") ? c.value("slot").toInt() : c.value("core").toInt());
        if (c.value("disabled").toBool()) disabled.insert(key);
        else if (c.value("coper").isDouble()) values.insert(key, c.value("coper").toInt());
    }
    const QJsonValue coall = d.value("coall");
    QStringList fromAll, fromStock, positive, off;
    for (const Core &c : coresOf(ryzen::detect())) {
        const int key = coreKey(c.ccd, c.slot);
        const QString id = QStringLiteral("CCD%1/%2").arg(c.ccd).arg(c.slot);
        if (disabled.contains(key)) { excluded_.insert(key); starts_[key] = 0; off << id; continue; }
        excluded_.remove(key);
        int v = 0;
        if (values.contains(key)) v = values.value(key);
        else if (coall.isDouble()) { v = coall.toInt(); fromAll << id; }
        else fromStock << id;
        if (v > 0) { positive << id; v = 0; }
        starts_[key] = std::max(v, -50);
    }
    note(tr("Preset '%1' loaded into the Start column.").arg(name), theme::OK);
    if (!fromAll.isEmpty()) note(tr("No per-core value, all-core value %1 used: %2").arg(coall.toInt()).arg(fromAll.join(", ")));
    if (!fromStock.isEmpty()) note(tr("No value in the preset, starting one step below stock: %1").arg(fromStock.join(", ")));
    if (!positive.isEmpty()) note(tr("Positive offsets start at 0: %1").arg(positive.join(", ")));
    if (!off.isEmpty()) note(tr("Disabled in the preset, unticked (stay at stock): %1").arg(off.join(", ")));
    saveSettings();
    refreshTable();
    updateEstimate();
}

void CoSearchDialog::loadSettings() {
    reloadPresets();
    scene_->addItem(tr("(keep the current settings)"), QString());
    for (const QString &n : scenes::names()) {
        const auto s = scenes::load(n);
        if (s && s->cpu.kind != scenes::Choice::Profile) scene_->addItem(n, n);  // a CO profile would replace the trials
    }
    QFile f(settingsPath());
    if (!f.open(QIODevice::ReadOnly) || f.size() > 65536) return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    mode_->setCurrentIndex(std::max(0, mode_->findData(o.value("mode").toString() == "per" ? int(Mode::PerCore) : int(Mode::AllCore))));
    level_->setCurrentIndex(std::max(0, level_->findData(o.value("level").toString())));
    limit_->setValue(o.value("limit").toInt(limit_->value()));
    allStart_->setValue(o.value("all_core_start").toInt(allStart_->value()));
    step_->setValue(o.value("step").toInt(step_->value()));
    for (const QJsonValue &v : o.value("excluded").toArray())
        excluded_.insert(coreKey(v.toObject().value("ccd").toInt(), v.toObject().value("slot").toInt()));
    for (const QJsonValue &v : o.value("starts").toArray()) {
        const QJsonObject c = v.toObject();
        starts_.insert(coreKey(c.value("ccd").toInt(), c.value("slot").toInt()), std::clamp(c.value("value").toInt(), -50, 0));
    }
    preset_->setCurrentIndex(std::max(0, preset_->findData(o.value("preset").toString())));
    scene_->setCurrentIndex(std::max(0, scene_->findData(o.value("scene").toString())));
}

void CoSearchDialog::saveSettings() const {
    QJsonArray excluded, starts;
    for (int key : excluded_) excluded << QJsonObject{{"ccd", key / 1000}, {"slot", key % 1000}};
    for (auto it = starts_.cbegin(); it != starts_.cend(); ++it)
        starts << QJsonObject{{"ccd", it.key() / 1000}, {"slot", it.key() % 1000}, {"value", it.value()}};
    QDir().mkpath(QFileInfo(settingsPath()).path());
    QSaveFile f(settingsPath());
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(QJsonObject{{"mode", Mode(mode_->currentData().toInt()) == Mode::PerCore ? "per" : "all"},
                                      {"level", level_->currentData().toString()}, {"limit", limit_->value()},
                                      {"all_core_start", allStart_->value()}, {"step", step_->value()},
                                      {"excluded", excluded}, {"starts", starts}, {"preset", preset_->currentData().toString()},
                                      {"scene", scene_->currentData().toString()}}).toJson());
    f.commit();
}

bool CoSearchDialog::writeJournal() const {
    if (!search_ || bootId_.isEmpty()) return false;
    QDir().mkpath(stateDir());
    QSaveFile f(journalPath());
    if (!f.open(QIODevice::WriteOnly)) return false;
    // v3 stores the ALREADY selected candidate and whether it is still pending. On reboot,
    // report(false) updates exactly that candidate. A resolved journal must NOT replay it.
    QJsonObject peaks;
    for (auto it = peakMhz_.cbegin(); it != peakMhz_.cend(); ++it) peaks.insert(QString::number(it.key()), it.value());
    const QByteArray data = QJsonDocument(QJsonObject{{"version", 3}, {"boot", bootId_},
        {"search", search_->save()}, {"pending", trial_.has_value()},
        {"trial", trial_ ? trialSignature(*trial_) : QJsonObject{}},
        {"scene", scene_->currentData().toString()}, {"prior_scene", priorScene_},
        {"reboots", hardReboots_}, {"attempts", attempts_}, {"evidence", evidence_}, {"active_cpu", activeCpu_},
        {"peaks", peaks}, {"verify_profile", verifiedProfile_}, {"clean_exit", cleanExit_}}).toJson();
    if (f.write(data) != data.size()) return false;
    return f.commit();  // QSaveFile: durable commit before a new privileged SMU write
}

bool CoSearchDialog::toolsPresent() {
    testProgram_ = QStandardPaths::findExecutable("centurion-cpustress");
    if (testProgram_.isEmpty() || !QFileInfo(privileged::helperPath("ryzen-co-helper")).isExecutable()) {
        QMessageBox::warning(this, tr("Missing tools"), tr("Install centurion-cpustress and ryzen-co-helper first."));
        return false;
    }
    // The per-core verification needs the `power` load: an older centurion-cpustress would only fail there,
    // after the whole search. Ask the binary itself.
    QProcess info;
    info.start(testProgram_, {QStringLiteral("info")});
    const bool answered = info.waitForFinished(10000);
    const QJsonObject loads = answered ? QJsonDocument::fromJson(info.readAllStandardOutput()).object().value("loads").toObject() : QJsonObject{};
    if (!loads.contains(QStringLiteral("power"))) {
        QMessageBox::warning(this, tr("Outdated centurion-cpustress"),
            tr("The installed centurion-cpustress has no 'power' load, which the per-core verification uses. "
               "Reinstall Centurion so that it matches this version."));
        return false;
    }
    if (!loads.value(QStringLiteral("power")).toBool()) {
        QMessageBox::warning(this, tr("CPU not supported"), tr("This CPU has no AVX2 / FMA, which the verification's power load needs."));
        return false;
    }
    // On battery the firmware lowers the power limits (the result would not hold on AC), and an empty battery
    // would end a trial like a crash. Desktops report no power supply (nullopt) and pass.
    if (scenes::onAc() == std::optional<bool>(false)) {
        QMessageBox::warning(this, tr("On battery"), tr("Connect the charger first. CO Search runs on AC power only."));
        return false;
    }
    return true;
}

void CoSearchDialog::showAttemptHistory() {
    // Snapshot only: opening this inspector cannot change the active candidate or journal.
    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(tr("CO Search — Trial History"));
    dlg->resize(820, 480);
    auto *layout = new QVBoxLayout(dlg);
    auto *summary = new QLabel(tr("Snapshot: %1 recorded trials, %2 suspected reboots. Passing a trial is not proof of long-term stability.")
        .arg(attempts_.size()).arg(hardReboots_));
    summary->setTextFormat(Qt::PlainText);
    summary->setWordWrap(true);
    layout->addWidget(summary);
    auto *filter = new QLineEdit;
    filter->setPlaceholderText(tr("Filter by core, candidate, result or evidence…"));
    layout->addWidget(filter);
    auto *history = new QTableWidget(attempts_.size(), 5);
    history->setHorizontalHeaderLabels({tr("Time"), tr("Core"), tr("Candidate"), tr("Result"), tr("Evidence")});
    history->setEditTriggers(QAbstractItemView::NoEditTriggers);
    history->setSelectionBehavior(QAbstractItemView::SelectRows);
    history->verticalHeader()->hide();
    history->horizontalHeader()->setStretchLastSection(true);
    const auto cores = search_ ? search_->cores() : QList<Core>{};
    for (int i = 0; i < attempts_.size(); ++i) {
        const auto a = attempts_.at(i).toObject();
        const int idx = a.value("core").toInt(-1);
        QString core = tr("Whole curve / unknown");
        if (idx >= 0 && idx < cores.size()) core = QStringLiteral("CCD %1 / %2").arg(cores[idx].ccd).arg(cores[idx].slot);
        const QString candidate = a.value("what").toString();
        const QString result = a.value("pass").toBool() ? tr("Passed") : tr("Failed / interrupted");
        const QString evidence = a.value("note").toString() + QStringLiteral(" ") + a.value("logs").toString();
        const QStringList cells{QDateTime::fromSecsSinceEpoch(a.value("t").toInteger()).toLocalTime().toString(Qt::ISODate), core, candidate, result, evidence.trimmed()};
        for (int c = 0; c < cells.size(); ++c) {
            auto *item = new QTableWidgetItem(cells[c]);
            item->setToolTip(cells[c]);
            history->setItem(i, c, item);
        }
        history->item(i, 3)->setForeground(QColor(a.value("pass").toBool() ? theme::OK : theme::DANGER));
    }
    for (int c = 0; c < 4; ++c) history->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    connect(filter, &QLineEdit::textChanged, history, [history](const QString &q) {
        for (int r = 0; r < history->rowCount(); ++r) {
            bool match = q.trimmed().isEmpty();
            for (int c = 0; c < history->columnCount(); ++c) match |= history->item(r, c)->text().contains(q.trimmed(), Qt::CaseInsensitive);
            history->setRowHidden(r, !match);
        }
    });
    layout->addWidget(history, 1);
    auto *close = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(close, &QDialogButtonBox::rejected, dlg, &QDialog::close);
    layout->addWidget(close);
    dlg->show();
}

void CoSearchDialog::showSessionArchive() {
    const auto entries = coarchive::read(stateDir() + QStringLiteral("/co-search-runs.json"));
    if (!entries) { QMessageBox::warning(this, tr("Session archive"), tr("The archive is unreadable, oversized or incompatible.")); return; }
    QDialog dlg(this); dlg.setWindowTitle(tr("Session archive")); dlg.resize(850, 540);
    auto *layout = new QVBoxLayout(&dlg);
    auto *hint = new QLabel(tr("Historical evidence only. A completed search is not proof of long-term stability. Legacy sessions may lack physical topology."));
    hint->setWordWrap(true); layout->addWidget(hint);
    auto *pick = new QComboBox; layout->addWidget(pick);
    auto *table = new QTableWidget; table->setColumnCount(6);
    table->setHorizontalHeaderLabels({tr("CCD"), tr("Core"), tr("Logical CPUs"), tr("CO"), tr("Walk state"), tr("Last failed")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers); table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    layout->addWidget(table, 1);
    auto *details = new QPlainTextEdit; details->setReadOnly(true); layout->addWidget(details, 1);
    for (qsizetype i = entries->size(); i > 0; --i) {
        const auto r = entries->at(i - 1).toObject();
        pick->addItem(QDateTime::fromSecsSinceEpoch(r.value("t").toInteger()).toLocalTime().toString(Qt::ISODate)
            + QStringLiteral(" · ") + (r.value("complete").toBool() ? tr("Completed") : tr("Incomplete")), int(i - 1));
    }
    auto render = [&] {
        table->setRowCount(0);
        if (pick->currentIndex() < 0) { details->setPlainText(tr("No saved sessions.")); return; }
        const auto r = entries->at(pick->currentData().toInt()).toObject();
        auto unknown = tr("Unknown");
        for (const auto &v : r.value("topology").toArray()) {
            const auto core = v.toObject();
            QStringList cpus; for (const auto &cpu : core.value("cpus").toArray()) cpus << QString::number(cpu.toInt(-1));
            auto number = [&](const char *key) { return core.value(QLatin1String(key)).isDouble() ? QString::number(core.value(QLatin1String(key)).toInt()) : unknown; };
            const auto state = core.value("excluded").toBool() ? tr("Excluded") : !core.value("walk_done").isBool() ? unknown : core.value("walk_done").toBool() ? tr("Walk completed") : tr("Incomplete");
            const QStringList cells{number("ccd"), number("slot"), cpus.join(' '), number("value"), state, number("failed")};
            const int row = table->rowCount(); table->insertRow(row);
            for (int c = 0; c < cells.size(); ++c) table->setItem(row, c, new QTableWidgetItem(cells.at(c)));
        }
        const auto verified = r.value("verification_passed");
        details->setPlainText(tr("Boot: %1\nBIOS: %2\nReset confirmed: %3\nTrials retained: %4 · omitted: %5\n%6")
            .arg(r.value("boot").toString(), r.value("bios").toString(), r.value("reset_ok").toBool() ? tr("Yes") : tr("No / unknown"))
            .arg(r.value("attempts").toArray().size()).arg(r.value("omitted_attempts").toInt()).arg(r.value("notes").toString())
            + QStringLiteral("\n") + tr("Whole-curve verification passed: %1").arg(!verified.isBool() ? unknown : verified.toBool() ? tr("Yes") : tr("No / unknown")));
    };
    connect(pick, &QComboBox::currentIndexChanged, &dlg, render); render();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject); dlg.exec();
}

void CoSearchDialog::refreshTable() {
    // Before any search: the detected cores, so they can be ticked off; afterwards the search's own.
    const QList<Core> cores = search_ ? search_->cores() : coresOf(ryzen::detect());
    const QList<int> v = search_ ? search_->values() : QList<int>(cores.size(), 0);
    const bool perCore = search_ ? search_->mode() == Mode::PerCore : Mode(mode_->currentData().toInt()) == Mode::PerCore;
    // The ticks are the selection for the NEXT search, editable whenever no search is running.
    const bool pickable = !running_ && Mode(mode_->currentData().toInt()) == Mode::PerCore;
    const QHash<int, int> guards = search_ ? QHash<int, int>{} : crashGuardMap();
    QList<int> ranks;
    for (const Core &c : cores) ranks << c.rank;
    const QList<int> anomalies = search_ && perCore && search_->finished() ? coinsights::rankAnomalies(ranks, v) : QList<int>{};
    const QSignalBlocker block(table_);
    table_->setRowCount(cores.size());
    while (auto *item = coreMap_->takeAt(0)) { delete item->widget(); delete item; }
    QMap<int, int> ccdRows, ccdColumns;
    for (int i = 0; i < cores.size(); ++i) {
        const Core &c = cores[i];
        const int key = coreKey(c.ccd, c.slot);
        QStringList cpus;
        for (int x : c.cpus) cpus << QString::number(x);
        QString status;
        const char *tone = nullptr;  // colour of the Status cell (and the Result cell once a core is done)
        bool testing = false, excluded = false, done = false;
        if (search_ && perCore) {
            const Search::CoreState st = search_->coreState(i);
            const bool isolated = trial_ && search_->perCoreSearching();
            if (st.excluded) { status = tr("excluded (stock)"); tone = theme::MUTED; excluded = true; }
            else if (isolated && trial_->perCore.value(i) != 0) { status = tr("testing %1").arg(trial_->perCore[i]); tone = theme::ACCENT; testing = true; }
            else if (trial_ && !isolated && !trial_->perCore.isEmpty()) { status = tr("20-minute verification"); tone = theme::PURPLE; }
            else if (st.done && st.failed) { status = tr("done: %1 passed, %2 failed").arg(st.pass).arg(*st.failed); tone = theme::OK; done = true; }
            else if (st.done) { status = tr("done: %1").arg(st.pass); tone = theme::OK; done = true; }
            else if (st.failed && st.pass == 0) { status = tr("start failed, climbing; never %1 or below").arg(*st.failed); tone = theme::WARN; }
            else if (st.failed) { status = tr("searching, never %1 or below").arg(*st.failed); tone = theme::WARN; }
            else { status = tr("searching"); tone = theme::INFO; }
        } else if (!search_ && guards.contains(key)) {
            status = tr("crash guard: never %1 or below").arg(guards.value(key));
            tone = theme::DANGER;
        }
        // Read-only insights on a core's result (coinsights.h), in the Notes column; a flag turns it yellow.
        QStringList extra;
        bool flagged = false;
        if (search_ && v[i] < 0 && !testing) {
            if (done && peakMhz_.contains(i)) {
                const int rated = ratedMhz(c.cpus.first());
                if (const auto share = coinsights::boostShare(peakMhz_.value(i), rated); share && *share < coinsights::LOW_BOOST) {
                    extra << tr("low boost %1/%2 MHz").arg(peakMhz_.value(i)).arg(rated);
                    flagged = true;
                }
            }
            if (perf_.contains(i)) {
                extra << tr("perf %1 %").arg(perf_.value(i), 0, 'f', 1);
                flagged |= perf_.value(i) < coinsights::STRETCH_PERCENT;
            }
            if (anomalies.contains(i)) {
                extra << tr("CPPC: test longer");
                flagged = true;
            }
        }
        QStringList tried;  // this core's probes, oldest first
        for (const QJsonValue &a : std::as_const(attempts_))
            if (search_ && a.toObject().value("core").toInt(-1) == i)
                tried << QStringLiteral("%1 %2").arg(a.toObject().value("value").toInt()).arg(a.toObject().value("pass").toBool() ? u'✓' : u'✗');
        QStringList tip;
        if (flagged) tip << tr("low boost: never ran near its rated clock while tested alone, so its pass is weaker evidence.\n"
                               "perf below %1 %: slower than at stock (clock stretching or lower boost).\n"
                               "CPPC: deeper than every lower-ranked core, unusual for a top core.").arg(coinsights::STRETCH_PERCENT, 0, 'f', 0);
        if (c.rank >= 0) tip << tr("CPPC rank %1 (higher = better core)").arg(c.rank);
        if (!tried.isEmpty()) tip << tr("Tried: %1").arg(tried.join(QStringLiteral("   ")));
        auto *core = new QTableWidgetItem(QStringLiteral("CCD%1 / %2  (CPU %3)").arg(c.ccd).arg(c.slot).arg(cpus.join(',')));
        core->setData(Qt::UserRole, key);
        core->setFlags(pickable ? Qt::ItemFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable) : Qt::ItemFlags(Qt::ItemIsEnabled));
        // While a per-core search runs, show its own exclusions (a resumed search keeps the ones it started with).
        const bool unticked = running_ && search_ && perCore ? search_->coreState(i).excluded : excluded_.contains(key);
        if (pickable || (perCore && search_)) core->setCheckState(unticked ? Qt::Unchecked : Qt::Checked);
        table_->setItem(i, 0, core);
        // Start: editable for the next search; while one runs (or shows its result), the search's own.
        table_->removeCellWidget(i, 1);
        if (pickable) {
            auto *spin = new QSpinBox;
            spin->setRange(-50, 0);
            spin->setValue(starts_.value(key, DEFAULT_START));
            connect(spin, &QSpinBox::valueChanged, this, [this, key](int value) {
                starts_[key] = value;
                preset_->setCurrentIndex(0);  // edited by hand: no longer the preset as saved
                saveSettings();
                updateEstimate();
            });
            table_->setItem(i, 1, new QTableWidgetItem);
            table_->setCellWidget(i, 1, spin);
        } else {
            table_->setItem(i, 1, new QTableWidgetItem(search_ && perCore ? QString::number(search_->coreState(i).start) : QString()));
        }
        table_->setItem(i, 2, new QTableWidgetItem(search_ ? QString::number(v[i]) : QString()));
        table_->setItem(i, 3, new QTableWidgetItem(status));
        if (!ccdRows.contains(c.ccd)) {
            const int r = int(ccdRows.size()) * 2;
            ccdRows.insert(c.ccd, r); ccdColumns.insert(c.ccd, 0);
            coreMap_->addWidget(new QLabel(tr("CCD %1").arg(c.ccd)), r, 0, 1, 8);
        }
        const int col = ccdColumns[c.ccd]++;
        auto *tile = new QLabel(tr("Core %1: %2").arg(c.slot).arg(search_ ? QString::number(v[i]) : QStringLiteral("—")));
        const QString state = testing ? tr("Testing") : excluded ? tr("Excluded") : done ? tr("Search done") :
            status.isEmpty() ? tr("No per-core result") : tone == theme::DANGER ? tr("Guarded") : tone == theme::WARN ? tr("Backtracking") :
            tone == theme::PURPLE ? tr("Verifying") : tr("Searching");
        tile->setText(tile->text() + QLatin1Char('\n') + state);
        tile->setTextFormat(Qt::PlainText);
        tile->setToolTip(core->text() + QLatin1Char('\n') + status + QLatin1Char('\n') + tip.join(QLatin1Char('\n')));
        tile->setAccessibleName(tile->text() + QStringLiteral(" — ") + status);
        tile->setStyleSheet(QStringLiteral("QLabel { border: 1px solid %1; color: %1; padding: 3px; }").arg(tone ? tone : theme::FG_DIM));
        coreMap_->addWidget(tile, ccdRows.value(c.ccd) + 1, col);
        if (tone) table_->item(i, 3)->setForeground(QColor(tone));
        table_->setItem(i, 4, new QTableWidgetItem(extra.join(QStringLiteral(" · "))));
        table_->item(i, 4)->setForeground(QColor(flagged ? theme::WARN : theme::MUTED));
        if (done || testing) {
            QFont bold = table_->font();
            bold.setBold(true);
            table_->item(i, done ? 2 : 3)->setFont(bold);
            if (done) table_->item(i, 2)->setForeground(QColor(theme::OK));
        }
        if (excluded) for (int col : {0, 2}) table_->item(i, col)->setForeground(QColor(theme::MUTED));
        if (testing) {
            QColor wash(theme::ACCENT);
            wash.setAlphaF(0.14);
            for (int col = 0; col < 5; ++col) table_->item(i, col)->setBackground(wash);
        }
        if (!tip.isEmpty()) for (int col : {0, 2, 3, 4}) table_->item(i, col)->setToolTip(tip.join('\n'));
    }
    table_->resizeColumnsToContents();
}

std::optional<ryzen::Layout> CoSearchDialog::preflight(Mode mode) {
    if (running_ || perfTest_) return std::nullopt;
    if (QFileInfo::exists(journalPath())) {
        // Never overwrite an unresolved crash history with a fresh, unguarded search.
        offerResume();
        return std::nullopt;
    }
    if (!toolsPresent()) return std::nullopt;
    if (CurvePage::currentState("cpu") != "stock" || scenes::gameSessions() > 0) {
        QMessageBox::warning(this, tr("Search locked"), tr("Reset the CPU curve to stock and end game sessions before searching. Existing offsets must not be overwritten.")); return std::nullopt;
    }
    // This helper's retained state is only *recorded* state, never an SMU read-back. Require
    // a known explicit stock write to avoid silently overwriting a saved CO configuration.
    QFile coState(QStringLiteral("/run/centurion/co-state.json"));
    const QJsonObject co = coState.open(QIODevice::ReadOnly) && coState.size() < (64LL << 10)
        ? QJsonDocument::fromJson(coState.readAll()).object() : QJsonObject{};
    const QJsonValue state = co.value("state");
    const QJsonObject recorded = state.toObject();
    const bool knownStock = state.toString() == QStringLiteral("reset") ||
        (recorded.size() == 1 && recorded.value("all").isDouble() && recorded.value("all").toInt(1) == 0);
    if (!knownStock) {
        QMessageBox::warning(this, tr("Cannot verify stock state"),
           tr("Use Ryzen Curve → Reset Curve Optimizer first. The SMU cannot read offsets back; "
              "the last helper-written state must explicitly be reset/0 before this search.")); return std::nullopt;
    }
    const ryzen::Layout layout = ryzen::detect();
    if (mode == Mode::PerCore && !layout.parked.isEmpty()) {
        QMessageBox::warning(this, tr("CCD parked"), tr("A CCD is parked by Optimizations: its cores cannot be tested alone. Unpark it first.")); return std::nullopt;
    }
    return layout;
}

void CoSearchDialog::start() {
    const auto mode = Mode(mode_->currentData().toInt());
    const auto layout = preflight(mode);
    if (!layout) return;
    const QList<Core> cores = coresOf(*layout);
    if (cores.isEmpty()) { QMessageBox::warning(this, tr("Topology"), tr("No CPU cores detected.")); return; }
    QList<Core> skipped;
    if (mode == Mode::PerCore)
        for (const Core &c : cores) if (excluded_.contains(coreKey(c.ccd, c.slot))) skipped << c;
    if (skipped.size() == cores.size()) {
        QMessageBox::warning(this, tr("No cores to test"), tr("Every core is unticked. Tick at least one core to search.")); return;
    }
    const int minutes = Search::estimateMinutes(mode, levelFrom(level_->currentData().toString()).value_or(Level::Fast),
                                                int(cores.size() - skipped.size()), limit_->value(), tuning());
    if (QMessageBox::warning(this, tr("Risk of a system crash"),
            tr("Undervolting may freeze or reboot the system. No applications can be protected against a hard lockup. "
               "The assistant resets the CPU curve when it can, but it cannot restore unsaved offsets. "
               "CO Search has no custom temperature cutoff; the CPU's own firmware thermal management remains active. "
               "This search takes roughly %1 h %2 min. Continue?").arg(minutes / 60).arg(minutes % 60),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    saveSettings();
    search_.emplace(mode, levelFrom(level_->currentData().toString()).value_or(Level::Fast), limit_->value(), cores, tuning());
    if (!applyCrashGuards(*search_)) {
        search_.reset();
        QMessageBox::critical(this, tr("CO crash guard corrupted"),
            tr("The recorded unsafe CO offsets cannot be read. Search is blocked rather than retrying a known crash value."));
        return;
    }
    for (const Core &c : skipped) search_->exclude(c.ccd, c.slot);
    if (mode == Mode::PerCore) {
        QStringList starts;
        for (const Core &c : cores) {
            if (skipped.contains(c)) continue;
            const int v = starts_.value(coreKey(c.ccd, c.slot), DEFAULT_START);
            search_->setStart(c.ccd, c.slot, v);
            starts << QStringLiteral("CCD%1/%2 %3").arg(c.ccd).arg(c.slot).arg(v);
            if (v < limit_->value())
                note(tr("CCD%1 / %2: Start %3 is past the limit; it starts at %4.").arg(c.ccd).arg(c.slot).arg(v).arg(limit_->value()));
        }
        const QString preset = preset_->currentData().toString();
        note(preset.isEmpty() ? tr("Starts (typed): %1").arg(starts.join(", "))
                              : tr("Starts (preset '%1'): %2").arg(preset, starts.join(", ")));
    }
    for (int i = 0; i < search_->cores().size(); ++i) {
        const Core &c = search_->cores()[i];
        const Search::CoreState st = search_->coreState(i);
        if (st.excluded) note(tr("CCD%1 / %2 excluded: stays at stock.").arg(c.ccd).arg(c.slot));
        else if (st.failed) note(tr("Crash guard: CCD%1 / %2 is never tested at %3 or below.").arg(c.ccd).arg(c.slot).arg(*st.failed), theme::WARN);
    }
    if (const auto all = readAllCoreGuard(); mode == Mode::AllCore && all && !all->isEmpty())
        note(tr("Crash guard: all-core is never tested at %1 or below.").arg(all->value("failed").toInt()), theme::WARN);
    QStringList otherBios;
    for (const QJsonValue &v : readCrashGuards().value_or(QJsonArray{})) {
        const QString b = v.toObject().value("bios").toString();
        if (!b.isEmpty() && b != biosVersion())
            otherBios << QStringLiteral("CCD%1/%2 (%3)").arg(v.toObject().value("ccd").toInt()).arg(v.toObject().value("slot").toInt()).arg(b);
    }
    if (!otherBios.isEmpty())
        note(tr("Crash guards recorded under another BIOS (now %1): %2. They still apply; Crash guards… can remove them.")
                 .arg(biosVersion(), otherBios.join(", ")), theme::WARN);
    verifiedProfile_.clear();
    peakMhz_.clear();
    hardReboots_ = 0;
    attempts_ = {};
    trialNo_ = 0;
    launch();
}

void CoSearchDialog::verifyProfile() {
    QStringList profiles;
    for (const QString &f : QDir(ryzen::profilesDir()).entryList({"*.json"}, QDir::Files, QDir::Name)) profiles << f.chopped(5);
    if (profiles.isEmpty()) { QMessageBox::information(this, tr("Verify a profile"), tr("No saved Ryzen Curve profiles.")); return; }
    bool ok = false;
    const QString pick = QInputDialog::getItem(this, tr("Verify a profile"), tr("Profile to verify:"), profiles,
                                               std::max<int>(0, profiles.indexOf(preset_->currentData().toString())), false, &ok);
    if (!ok || pick.isEmpty()) return;
    // Same file format as Ryzen Curve → Save Profile (and applyPreset).
    QFile f(ryzen::profilesDir() + '/' + pick + ".json");
    const QJsonDocument doc = f.open(QIODevice::ReadOnly) && f.size() <= 256 * 1024 ? QJsonDocument::fromJson(f.readAll()) : QJsonDocument();
    if (!doc.isObject()) { QMessageBox::warning(this, tr("Verify a profile"), tr("Profile '%1' could not be read.").arg(pick)); return; }
    const QJsonObject d = doc.object();
    QHash<int, int> values;
    bool anyPer = false;
    QStringList outside;  // verified as stored or not at all: a clamped value would be verified under the profile's name
    for (const QJsonValue &v : d.value("cores").toArray()) {
        const QJsonObject c = v.toObject();
        if (c.value("ccx").toInt() != 0) continue;
        const int key = coreKey(c.value("ccd").toInt(), c.contains("slot") ? c.value("slot").toInt() : c.value("core").toInt());
        if (c.value("disabled").toBool()) { values.insert(key, 0); anyPer = true; }
        else if (c.value("coper").isDouble()) {
            const int x = c.value("coper").toInt();
            if (x < -50 || x > 0) outside << QString::number(x);
            values.insert(key, x);
            anyPer = true;
        }
    }
    const QJsonValue coall = d.value("coall");
    if (!anyPer && !coall.isDouble()) { QMessageBox::warning(this, tr("Verify a profile"), tr("Profile '%1' has no offsets.").arg(pick)); return; }
    if (coall.isDouble() && (coall.toInt() < -50 || coall.toInt() > 0)) outside << QString::number(coall.toInt());
    if (!outside.isEmpty()) {
        QMessageBox::warning(this, tr("Verify a profile"),
            tr("Profile '%1' holds offsets outside -50…0 (%2). Only negative offsets down to -50 can be verified; "
               "the profile is not changed.").arg(pick, outside.join(", ")));
        return;
    }
    const Mode mode = anyPer ? Mode::PerCore : Mode::AllCore;
    const auto layout = preflight(mode);
    if (!layout) return;
    const QList<Core> cores = coresOf(*layout);
    if (cores.isEmpty()) { QMessageBox::warning(this, tr("Topology"), tr("No CPU cores detected.")); return; }
    const auto level = levelFrom(level_->currentData().toString()).value_or(Level::Fast);
    // Fail closed like start(): an unreadable guard file must not read as "no crash recorded".
    const auto allGuard = readAllCoreGuard();
    if (!readCrashGuards() || !allGuard) {
        QMessageBox::critical(this, tr("CO crash guard corrupted"),
            tr("The recorded unsafe CO offsets cannot be read. Verification is blocked rather than retrying a known crash value."));
        return;
    }
    // The test order of the cores is the Search's own; a throwaway one gives it before the limit is known.
    QList<int> v;
    int deepest = 0;
    QStringList clash, shown;
    const QHash<int, int> guards = crashGuardMap();
    const Search order(mode, level, limit_->value(), cores, tuning());
    for (const Core &c : order.cores()) {
        const int key = coreKey(c.ccd, c.slot);
        const int x = mode == Mode::PerCore ? values.value(key, coall.isDouble() ? coall.toInt() : 0) : coall.toInt();
        if (guards.contains(key) && x <= guards.value(key))
            clash << tr("CCD%1/%2 %3 (crashed at %4)").arg(c.ccd).arg(c.slot).arg(x).arg(guards.value(key));
        v << x;
        shown << QStringLiteral("CCD%1/%2 %3").arg(c.ccd).arg(c.slot).arg(x);
        deepest = std::min(deepest, x);
    }
    if (mode == Mode::AllCore && !allGuard->isEmpty() && coall.toInt() <= allGuard->value("failed").toInt())
        clash << tr("all-core %1 (crashed at %2)").arg(coall.toInt()).arg(allGuard->value("failed").toInt());
    if (!clash.isEmpty()) {
        QMessageBox::warning(this, tr("Verify a profile"),
            tr("These values are at or below a recorded crash on that core, so they are not applied: %1.\n\n"
               "Make them less negative, or remove the guard under Crash guards… if you no longer trust it.").arg(clash.join(", ")));
        return;
    }
    if (deepest == 0) { QMessageBox::information(this, tr("Verify a profile"), tr("Every core of '%1' is at stock: nothing to verify.").arg(pick)); return; }
    search_.emplace(mode, level, std::min(limit_->value(), deepest), cores, tuning());
    const auto t = search_->startVerification(v) ? search_->inFlight() : std::nullopt;
    if (!t) { search_.reset(); QMessageBox::warning(this, tr("Verify a profile"), tr("The profile's values cannot be verified.")); return; }
    if (QMessageBox::warning(this, tr("Risk of a system crash"),
            tr("Verifying '%1' applies its offsets under load for about %2 min (longer if a core has to back off). "
               "Undervolting may freeze or reboot the system; the curve is reset to stock afterwards. Continue?")
                .arg(pick).arg((t->seconds() + 59) / 60),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) { search_.reset(); return; }
    saveSettings();
    verifiedProfile_ = pick;
    peakMhz_.clear();
    note(tr("Verifying profile '%1': %2").arg(pick, shown.join(", ")), theme::INFO);
    hardReboots_ = 0;
    attempts_ = {};
    trialNo_ = 0;
    launch();
}

void CoSearchDialog::manageCrashGuards() {
    if (running_) return;
    const auto guards = readCrashGuards();
    const auto all = readAllCoreGuard();
    if (!guards || !all) { QMessageBox::critical(this, tr("CO crash guard corrupted"), tr("The crash guard file cannot be read; it is left unchanged.")); return; }
    if (guards->isEmpty() && all->isEmpty()) { QMessageBox::information(this, tr("Crash Guards"), tr("No crash guards are recorded.")); return; }
    const int allRow = all->isEmpty() ? -1 : int(guards->size());  // the all-core guard, last
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Crash Guards"));
    dlg.resize(560, 360);
    auto *lay = new QVBoxLayout(&dlg);
    auto *info = new QLabel(tr("Each row is a value that crashed this machine while that core was tested alone: it and every "
                               "more negative value are never tried on that core again. BIOS is the one the crash happened "
                               "under (empty: recorded before Centurion kept it); current BIOS: %1.").arg(biosVersion()));
    if (allRow >= 0)
        info->setText(info->text() + ' ' + tr("\"All cores\" is an all-core offset that crashed this machine: all-core searches "
                                              "never go to it or below."));
    info->setWordWrap(true);
    lay->addWidget(info);
    auto *table = new QTableWidget(int(guards->size()) + (allRow >= 0 ? 1 : 0), 3);
    table->setHorizontalHeaderLabels({tr("Core"), tr("Never at or below"), tr("BIOS")});
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (int i = 0; i < guards->size(); ++i) {
        const QJsonObject g = guards->at(i).toObject();
        QStringList cpus;
        for (const QJsonValue &c : g.value("cpus").toArray()) cpus << QString::number(c.toInt());
        const QString bios = g.value("bios").toString();
        table->setItem(i, 0, new QTableWidgetItem(QStringLiteral("CCD%1 / %2  (CPU %3)").arg(g.value("ccd").toInt()).arg(g.value("slot").toInt()).arg(cpus.join(','))));
        table->setItem(i, 1, new QTableWidgetItem(QString::number(g.value("failed").toInt())));
        table->item(i, 1)->setForeground(QColor(theme::DANGER));
        table->setItem(i, 2, new QTableWidgetItem(bios));
        if (!bios.isEmpty() && bios != biosVersion()) table->item(i, 2)->setForeground(QColor(theme::WARN));
    }
    if (allRow >= 0) {
        const QString bios = all->value("bios").toString();
        table->setItem(allRow, 0, new QTableWidgetItem(tr("All cores (all-core search)")));
        table->setItem(allRow, 1, new QTableWidgetItem(QString::number(all->value("failed").toInt())));
        table->item(allRow, 1)->setForeground(QColor(theme::DANGER));
        table->setItem(allRow, 2, new QTableWidgetItem(bios));
        if (!bios.isEmpty() && bios != biosVersion()) table->item(allRow, 2)->setForeground(QColor(theme::WARN));
    }
    table->resizeColumnsToContents();
    lay->addWidget(table, 1);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close);
    QPushButton *remove = box->addButton(tr("Remove selected"), QDialogButtonBox::DestructiveRole);
    remove->setObjectName("btnDanger");
    lay->addWidget(box);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(remove, &QPushButton::clicked, &dlg, [this, &dlg, table, guards, all, allRow] {
        QSet<int> rows;
        for (const QModelIndex &i : table->selectionModel()->selectedRows()) rows.insert(i.row());
        if (rows.isEmpty()) return;
        if (QMessageBox::warning(&dlg, tr("Remove crash guards"),
                tr("Remove %1 guard(s)? A later search may then try values that crashed this machine before.").arg(rows.size()),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
        QJsonArray kept;
        QStringList gone;
        for (int i = 0; i < guards->size(); ++i) {
            if (!rows.contains(i)) { kept << guards->at(i); continue; }
            const QJsonObject g = guards->at(i).toObject();
            gone << QStringLiteral("CCD%1/%2 %3").arg(g.value("ccd").toInt()).arg(g.value("slot").toInt()).arg(g.value("failed").toInt());
        }
        if (kept.size() != guards->size()) {
            QSaveFile f(crashGuardPath());
            const QByteArray data = QJsonDocument(QJsonObject{{"version", 1}, {"guards", kept}}).toJson();
            if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit()) {
                QMessageBox::critical(&dlg, tr("Crash Guards"), tr("Could not write the crash guard file; nothing was removed."));
                return;
            }
        }
        if (rows.contains(allRow)) {
            if (!QFile::remove(allCoreGuardPath())) {
                if (!gone.isEmpty()) note(tr("Crash guards removed: %1").arg(gone.join(", ")), theme::WARN);
                QMessageBox::critical(&dlg, tr("Crash Guards"), tr("Could not remove the all-core crash guard file."));
                return;
            }
            gone << tr("all-core %1").arg(all->value("failed").toInt());
        }
        note(tr("Crash guards removed: %1").arg(gone.join(", ")), theme::WARN);
        dlg.accept();
    });
    dlg.exec();
    refreshTable();
}

void CoSearchDialog::measureThroughput(std::function<void(const QHash<int, int> &, int)> then) {
    static constexpr int PERF_SECONDS = 5;
    QStringList cpus;
    for (int i = 0; search_ && i < search_->cores().size(); ++i)
        if (search_->mode() == Mode::AllCore || !search_->coreState(i).excluded) cpus << QString::number(search_->cores()[i].cpus.first());
    if (cpus.isEmpty() || testProgram_.isEmpty()) { then({}, -1); return; }
    const QStringList args{QStringLiteral("percore"), QStringLiteral("--load"), QStringLiteral("sse"), QStringLiteral("--seconds"),
                           QString::number(PERF_SECONDS), QStringLiteral("--cpus"), cpus.join(',')};
    auto *p = new QProcess(this);
    perfTest_ = p;
    auto chunks = std::make_shared<QHash<int, int>>();
    auto buf = std::make_shared<QByteArray>();
    auto parse = [p, chunks, buf] {
        *buf += p->readAllStandardOutput();
        int nl;
        while ((nl = buf->indexOf('\n')) >= 0) {
            const QJsonObject o = QJsonDocument::fromJson(buf->left(nl)).object();
            buf->remove(0, nl + 1);
            if (o.value("ev").toString() == "core_end" && !o.value("skipped").toBool())
                chunks->insert(o.value("cpu").toInt(-1), o.value("chunks").toInt());
        }
    };
    connect(p, &QProcess::readyReadStandardOutput, this, parse);
    auto finish = std::make_shared<std::function<void(int)>>([this, p, chunks, then, parse](int code) {
        parse();
        p->deleteLater();
        perfTest_ = nullptr;
        then(*chunks, code);
    });
    connect(p, &QProcess::finished, this, [finish](int code, QProcess::ExitStatus st) { (*finish)(st == QProcess::NormalExit ? code : -1); });
    connect(p, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError e) { if (e == QProcess::FailedToStart) (*finish)(-1); });
    QTimer::singleShot((cpus.size() * PERF_SECONDS + 90) * 1000, p, [p] { p->kill(); });
    logLine(log_, {}, QStringLiteral("centurion-cpustress ") + args.join(' '), theme::MUTED);
    p->start(testProgram_, args);
}

void CoSearchDialog::compareThroughput(std::function<void()> then) {
    go_->setEnabled(false);
    note(tr("Measuring single-core throughput at stock, to compare with the found curve…"), theme::INFO);
    measureThroughput([this, then](const QHash<int, int> &stock, int code) {
        go_->setEnabled(!running_);
        const QHash<int, int> curve = std::exchange(curveRun_, {});
        auto after = qScopeGuard([then] { then(); });
        if (code != 0) { note(tr("The stock throughput run did not complete; no clock-stretch comparison."), theme::WARN); return; }
        QStringList flagged;
        double lo = 0, hi = 0;
        bool any = false;
        for (int i = 0; i < search_->cores().size(); ++i) {
            const int cpu = search_->cores()[i].cpus.first();
            if (!curve.contains(cpu) || !stock.contains(cpu)) continue;
            const auto change = coinsights::changePercent(stock.value(cpu), curve.value(cpu));
            if (!change) continue;
            perf_.insert(i, *change);
            lo = any ? std::min(lo, *change) : *change;
            hi = any ? std::max(hi, *change) : *change;
            any = true;
            if (*change < coinsights::STRETCH_PERCENT)
                flagged << QStringLiteral("CCD%1/%2 %3 %").arg(search_->cores()[i].ccd).arg(search_->cores()[i].slot).arg(*change, 0, 'f', 1);
        }
        if (!any) note(tr("No throughput values to compare."), theme::WARN);
        else if (flagged.isEmpty())
            note(tr("Clock-stretch check: no single-core throughput loss against stock (%1 % … %2 %).")
                     .arg(lo, 0, 'f', 1).arg(hi, 0, 'f', 1), theme::OK);
        else
            note(tr("Clock-stretch check: lower throughput than at stock on %1. Clock stretching or a lower boost at that "
                    "offset: 1-2 points less negative there may be faster.").arg(flagged.join(", ")), theme::WARN);
        refreshTable();
    });
}

void CoSearchDialog::offerResume() {
    if (running_ || !QFileInfo::exists(journalPath())) return;
    QFile f(journalPath());
    // An unreadable or oversized journal still goes to the "invalid journal" question below, or Start would
    // return here forever without saying why.
    const bool readable = f.open(QIODevice::ReadOnly) && f.size() <= (1LL << 20);
    const QJsonObject j = readable ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject{};
    f.close();
    // The journal's boot is this one: the machine never restarted, so the trial in flight may still be applied
    // (the application died or was quit while the reset failed). An unreadable boot ID, or a journal whose
    // boot cannot be read, counts as this boot.
    const QString currentBoot = readBootId();
    const bool sameBoot = currentBoot.isEmpty() || !readable || j.value("boot").toString() == currentBoot;
    // A failed removal must be said out loud: otherwise the same question comes back on every open and Start.
    auto removeJournal = [this]() {
        QFile rm(journalPath());
        if (rm.remove() || !rm.exists()) return true;
        const QFileInfo fi(journalPath()), di(fi.path());
        QMessageBox::critical(this, tr("Cannot remove the CO search journal"),
            tr("%1 could not be removed: %2.\nFile owner: %3, folder owner: %4.\n\nA file or folder owned by root (for example "
               "after starting Centurion with sudo) cannot be removed from here. Remove it in a terminal:\n\nsudo rm '%1'")
                .arg(fi.filePath(), rm.errorString(), fi.owner(), di.owner()));
        return false;
    };
    // Discarding removes the journal; in this boot only after a confirmed stock reset, so no candidate is left behind.
    // `unreadable`: the journal holds nothing usable, so after a failed reset it may still be removed on request.
    auto discard = [this, sameBoot, removeJournal](std::function<void()> after, bool unreadable = false) {
        if (!sameBoot) { if (removeJournal()) after(); return; }
        note(tr("Resetting the CPU curve to stock before discarding the interrupted search…"));
        privileged::run(privileged::helperPath("ryzen-co-helper"), QJsonObject{{"op", "reset"}}, this,
            [this, after, unreadable, removeJournal](const privileged::Result &r) {
                if (!r.ok()) {
                    note(tr("Reset failed: %1. The journal is kept; use Ryzen Curve → Reset, or reboot.").arg(r.message()), theme::DANGER);
                    if (!unreadable || QMessageBox::warning(this, tr("Reset failed"),
                            tr("The CPU curve could not be reset to stock: %1\n\nThe journal cannot be read, so keeping it protects "
                               "nothing and only brings this question back. Remove it anyway? The curve state stays unknown until "
                               "Ryzen Curve → Reset succeeds or the machine restarts (a restart clears Curve Optimizer offsets).")
                                .arg(r.message()),
                            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
                        return;
                }
                if (removeJournal()) after();
            }, 120000);
    };
    // A journal that cannot be resumed must still be removable, or every later Start lands here again.
    auto unusable = [this, sameBoot, discard, readable](const QString &why, bool guarded = false) {
        const auto answer = QMessageBox::warning(this, tr("Invalid CO search journal"),
            why + "\n\n" + (guarded ? tr("It cannot be safely resumed. Discard the old journal?")
                                    : tr("It cannot be safely resumed, and its interrupted value cannot be identified for a crash guard. "
                                         "Discard the old journal?")) + ' '
                + (sameBoot ? tr("The CPU curve is reset to stock first: the interrupted value may still be applied in this boot.")
                            : tr("The CPU curve is not changed: the machine has restarted since.")),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer == QMessageBox::Yes) discard([this] { note(tr("Invalid CO search journal discarded.")); }, !readable);
    };
    if (!readable) {
        // Say why: the file, its owner and the open error (a root-owned journal is the usual case).
        const QFileInfo fi(journalPath());
        const QString cause = fi.size() > (1LL << 20) ? tr("it is %1 bytes").arg(fi.size())
                                                       : tr("%1; owner %2").arg(f.errorString(), fi.owner());
        unusable(tr("The interrupted CO Search journal cannot be read (or is larger than 1 MiB).") + "\n" + fi.filePath() + " (" + cause + ")");
        return;
    }
    auto s = Search::load(j.value("search").toObject());
    if (!s || j.value("version").toInt() != 3) {
        unusable(tr("The interrupted CO Search journal is incompatible or damaged."));
        return;
    }
    if (currentBoot.isEmpty()) {
        QMessageBox::warning(this, tr("Boot identity"), tr("Cannot verify boot ID; recovery is blocked."));
        return;
    }
    if (!j.value("pending").isBool()) {
        unusable(tr("The journal has no valid pending-state marker."));
        return;
    }
    const bool pending = j.value("pending").toBool(false);
    std::optional<Trial> interrupted;
    if (pending) {
        interrupted = s->inFlight();
        if (!interrupted || trialSignature(*interrupted) != j.value("trial").toObject()) {
            unusable(tr("Saved candidate and search state disagree (journal: %1).").arg(j.value("trial").toObject().value("what").toString()));
            return;
        }
    }
    // What ended the trial in flight:
    //  - the application quit after a confirmed stock reset (clean_exit): nothing failed, the trial runs again;
    //  - the machine restarted otherwise: a crash, recorded for good (the persistent crash guard);
    //  - the application died while the machine kept running: a failure in this search only (as a wrong
    //    result is), not a crash of the machine, so later searches are not limited by it.
    const bool cleanExit = pending && j.value("clean_exit").toBool(false);
    const bool crashed = pending && !cleanExit && !sameBoot;
    // Quarantine a crash immediately, BEFORE the user can choose Discard. Otherwise discarding the
    // journal and starting a new search would silently repeat the crash-inducing value.
    if (crashed && !saveCrashGuard(*s, *interrupted)) {
        QMessageBox::critical(this, tr("Recovery guard failed"),
            tr("Cannot save the interrupted CO candidate to the persistent crash guard. "
               "Neither resuming nor discarding the journal is safe until this is repaired."));
        return;
    }
    // The journal's cores are the topology the search ran on. After a BIOS / SMT / CCD-parking change a
    // CCD/slot may have other logical CPUs, or be gone: the stress would load the wrong CPUs and a result
    // would be credited to the wrong physical core. (The crash guard above is keyed by CCD/slot, so it holds.)
    auto topology = [](QList<Core> cores) {
        for (Core &c : cores) { c.rank = -1; std::sort(c.cpus.begin(), c.cpus.end()); }
        std::sort(cores.begin(), cores.end(), [](const Core &a, const Core &b) { return std::pair(a.ccd, a.slot) < std::pair(b.ccd, b.slot); });
        return cores;
    };
    if (topology(s->cores()) != topology(coresOf(ryzen::detect()))) {
        unusable(tr("The CPU topology changed since this search started (BIOS, SMT or a parked CCD): its cores and "
                    "logical CPUs no longer match this machine."), true);
        return;
    }
    // Crash watcher: what the system logs say about the interrupted trial (coevidence.h). It is collected
    // before the question and used only inside this search, never for the persistent crash guard.
    auto ask = [this, j, s, interrupted, pending, cleanExit, crashed, currentBoot, discard](const coevidence::Verdict &verdict) mutable {
        // No reboot lockout: every crashed candidate is guarded for good, so a resume can never
        // repeat it and a reboot loop cannot form. Each resume is still the user's decision.
        const int rebootCount = std::max(0, j.value("reboots").toInt()) + int(crashed);
        const QString what = pending ? interrupted->what : tr("the last resolved trial");
        // The final verification has every core undervolted: there the logs may name the culprit. A single-core
        // probe already knows its core (the guard above); evidence there is only reported.
        const bool finalStage = s->mode() == Mode::PerCore && !s->perCoreSearching();
        Outcome failed{false, {}, crashed};
        if (crashed && finalStage && verdict.kind == coevidence::Verdict::Blamed) failed.cpus = verdict.cpus;
        QString text;
        if (!pending)
            text = tr("The search stopped after its previous trial was already resolved. No candidate will be replayed.");
        else if (cleanExit)
            text = tr("The application was closed during %1, after the CPU curve was reset to stock. Nothing failed: "
                      "a resume runs this trial again.").arg(what);
        else if (crashed && finalStage)
            text = tr("The search was interrupted during %1. Undervolted cores back off by 2 points before it runs again: "
                      "only the core the system logs blame, if they blame one; otherwise every undervolted core.").arg(what);
        else if (crashed)
            text = tr("The search was interrupted while trying %1. That value is recorded as a crash: this core keeps its last "
                      "passing value, and it is never tested at that value or below again, in this or any later search.").arg(what);
        else if (finalStage)
            text = tr("The application stopped during %1 while the machine kept running. It counts as a failed verification: "
                      "every undervolted core backs off by 1 point before it runs again.").arg(what);
        else
            text = tr("The application stopped while trying %1, but the machine kept running. That value counts as failed "
                      "in this search only, as a wrong result would; no crash guard is recorded.").arg(what);
        if (pending) {
            QStringList offsets;
            if (interrupted->allCore) offsets << tr("All-core: %1").arg(*interrupted->allCore);
            else for (int i = 0; i < interrupted->perCore.size() && i < s->cores().size(); ++i) {
                if (!interrupted->perCore[i]) continue;
                const auto &c = s->cores()[i];
                offsets << QStringLiteral("CCD %1 / %2: %3").arg(c.ccd).arg(c.slot).arg(interrupted->perCore[i]);
            }
            text += "\n\n" + tr("Interrupted candidate (not the next candidate): %1").arg(offsets.join(QStringLiteral(" · ")));
        }
        // Preview follows exactly the same report/guard/replay order as the real resume.
        Search preview = *s;
        if (pending && !cleanExit) preview.report(failed);
        if (applyCrashGuards(preview)) {
            const auto next = pending && cleanExit ? interrupted : preview.previewNext();
            if (next) {
                QStringList values;
                if (next->allCore) values << tr("All-core: %1").arg(*next->allCore);
                else for (int i = 0; i < next->perCore.size() && i < preview.cores().size(); ++i) {
                    const auto &c = preview.cores()[i];
                    values << QStringLiteral("CCD %1 / %2: %3").arg(c.ccd).arg(c.slot).arg(next->perCore[i]);
                }
                text += "\n\n" + tr("Next candidate preview: %1 (%2 s of tests, excluding overhead). %3")
                    .arg(next->what).arg(next->seconds()).arg(values.join(QStringLiteral(" · ")));
            } else text += "\n\n" + tr("No further candidate in the current search state.");
        } else text += "\n\n" + tr("Next candidate unavailable: crash guard could not be read.");
        if (crashed) text += "\n\n" + tr("System logs: %1").arg(verdict.summary);
        const auto answer = QMessageBox::question(this, tr("Resume CO search"),
            text + "\n\n" + tr("Suspected reboots in this search so far: %1.").arg(rebootCount) + "\n\n"
                 + tr("Resume after resetting the CPU curve to stock? No discards this interrupted search (its values so far "
                      "stay in the table and can be saved)."),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::No);
        if (answer == QMessageBox::No) {
            discard([this, s, j, pending, cleanExit, failed, rebootCount]() mutable {
                if (pending && !cleanExit) s->report(failed);
                attempts_ = j.value("attempts").toArray();
                hardReboots_ = rebootCount;
                search_ = std::move(s);
                trial_.reset();
                finished_ = false;
                refreshTable();
                updateResultButtons();
                note(tr("Interrupted search discarded. Its values so far are in the table; Save result keeps them (not verified)."));
            });
            return;
        }
        if (answer != QMessageBox::Yes || !toolsPresent()) return;
        if (scenes::gameSessions() > 0) {
            QMessageBox::warning(this, tr("Search locked"), tr("End game sessions first."));
            return;
        }
        mode_->setCurrentIndex(mode_->findData(int(s->mode())));
        level_->setCurrentIndex(level_->findData(name(s->level())));
        limit_->setValue(s->limit());
        allStart_->setValue(s->tuning().allCoreStart);
        step_->setValue(s->tuning().step);
        scene_->setCurrentIndex(std::max(0, scene_->findData(j.value("scene").toString())));
        attempts_ = j.value("attempts").toArray();
        peakMhz_.clear();
        const QJsonObject peaks = j.value("peaks").toObject();
        for (auto it = peaks.begin(); it != peaks.end(); ++it) peakMhz_.insert(it.key().toInt(), it.value().toInt());
        verifiedProfile_ = j.value("verify_profile").toString();
        hardReboots_ = rebootCount;
        if (pending && !cleanExit) {
            s->report(failed);
            QJsonObject recovered{{"t", double(QDateTime::currentSecsSinceEpoch())},
                {"what", interrupted->what}, {"pass", false},
                {"note", crashed ? "reboot; unsafe candidate quarantined" : "application stopped; failed in this search only"},
                {"logs", crashed ? verdict.summary : QString()}};
            if (s->mode() == Mode::PerCore && !finalStage) {
                for (int i = 0; i < interrupted->perCore.size(); ++i) {
                    if (interrupted->perCore[i] == 0) continue;
                    recovered["core"] = i;
                    recovered["value"] = interrupted->perCore[i];
                    break;
                }
            }
            attempts_.append(recovered);
        }
        if (!applyCrashGuards(*s)) {
            QMessageBox::critical(this, tr("CO crash guard corrupted"),
                tr("Recorded crash exclusions cannot be read. No CO value was written."));
            return;
        }
        search_ = std::move(s);
        trial_.reset();
        bootId_ = currentBoot;
        priorScene_ = j.value("prior_scene").toString();
        // Commit the RECOVERED state before any further SMU write; another abrupt reboot may now
        // safely resume from a state where the failed candidate is already excluded. A trial to run again
        // needs no new state: the journal on disk already holds it, pending.
        if (!cleanExit && !writeJournal()) {
            QMessageBox::critical(this, tr("Recovery journal failed"),
                tr("Cannot durably commit the recovered state. No CO value was applied."));
            search_.reset();
            return;
        }
        trialNo_ = attempts_.size();
        note(!pending ? tr("Recovery: continuing after last resolved trial. Resetting to stock first.")
             : cleanExit ? tr("Recovery: %1 runs again. Resetting to stock first.").arg(what)
             : crashed ? tr("Recovery: %1 marked unsafe and excluded. Resetting to stock first.").arg(what)
                       : tr("Recovery: %1 counted as failed in this search. Resetting to stock first.").arg(what));
        if (crashed) note(tr("System logs: %1").arg(verdict.summary));
        const std::optional<Trial> again = cleanExit ? interrupted : std::nullopt;
        privileged::run(privileged::helperPath("ryzen-co-helper"), QJsonObject{{"op", "reset"}}, this,
            [this, again](const privileged::Result &r) {
                if (!r.ok()) { note(tr("Reset failed: %1. Not resuming.").arg(r.message()), theme::DANGER); search_.reset(); return; }
                const QString keep = priorScene_;
                replay_ = again;
                launch();
                priorScene_ = keep;
            }, 120000);
    };
    if (crashed) {
        note(tr("Reading the system logs of the crashed boot…"));
        gatherEvidence(j, *s, *interrupted, ask);
    } else {
        ask({});
    }
}

void CoSearchDialog::gatherEvidence(const QJsonObject &journal, const Search &search, const Trial &trial,
                                    std::function<void(const coevidence::Verdict &)> then) {
    using coevidence::Record;
    // 1. What the live watcher saw during the trial and committed to the journal before the crash.
    QList<Record> records;
    for (const QJsonValue &v : journal.value("evidence").toArray())
        if (const auto r = Record::fromJson(v.toObject())) records << *r;
    const QString crashed = journal.value("boot").toString();
    QList<int> offsets = trial.perCore;
    if (trial.allCore) offsets = QList<int>(search.cores().size(), *trial.allCore);
    const QList<Core> cores = search.cores();
    privileged::run(privileged::helperPath("health-reader"), QJsonObject{{"op", "boots"}}, this,
        [this, records, crashed, offsets, cores, then](const privileged::Result &b) mutable {
            // 3. pstore records the boot history attributes to the crashed boot (a kernel panic's last lines).
            bool previousIsCrashed = false;
            if (b.ok()) {
                const QJsonArray boots = b.json.value("boots").toArray();  // newest first
                const QString current = b.json.value("current").toString();
                for (int i = 0; i + 1 < boots.size(); ++i)
                    if (boots[i].toObject().value("boot").toString() == current)
                        previousIsCrashed = boots[i + 1].toObject().value("boot").toString() == crashed;
                for (const QJsonValue &e : b.json.value("pstore").toObject().value("entries").toArray()) {
                    if (e.toObject().value("boot").toString() != crashed) continue;
                    for (const QString &line : e.toObject().value("text").toString().split('\n'))
                        if (const auto r = coevidence::fromKernelText(line, QStringLiteral("pstore"))) records << *r;
                }
            }
            privileged::run(privileged::helperPath("health-reader"), QJsonObject{{"since", 0.0}}, this,
                [this, records, previousIsCrashed, offsets, cores, then](const privileged::Result &k) mutable {
                    // 2. Machine checks the kernel found in the banks early in THIS boot: left there by the reset,
                    // so they belong to the boot before - used only when that is surely the crashed one.
                    if (k.ok() && previousIsCrashed) {
                        for (const QJsonValue &v : k.json.value("events").toArray()) {
                            const QJsonObject e = v.toObject();
                            if (e.value("kind").toString() != "mce" || e.value("ts_us").toDouble() > 120e6) continue;
                            if (const auto r = coevidence::fromKernelText(e.value("text").toString(), QStringLiteral("boot-kernel")))
                                records << *r;
                        }
                    }
                    coevidence::Verdict v = coevidence::judge(records, cores, offsets);
                    if (!previousIsCrashed)
                        v.summary += ' ' + tr("(Machine checks from this boot's start were not used: the boot history cannot confirm "
                                              "that the previous boot is the one that crashed.)");
                    then(v);
                }, 30000);
        }, 30000);
}

void CoSearchDialog::launch() {
    running_ = true;
    abort_ = false;
    finished_ = false;
    bootId_ = readBootId();
    if (bootId_.isEmpty()) { running_ = false; replay_.reset(); QMessageBox::warning(this, tr("Boot identity"), tr("Cannot verify boot ID.")); return; }
    cleanExit_ = false;
    for (QWidget *w : std::initializer_list<QWidget *>{go_, mode_, level_, limit_, scene_, verifyBtn_, guardsBtn_}) w->setEnabled(false);
    stop_->setEnabled(true);
    confirmPassed_ = false;
    curveRun_.clear();
    perf_.clear();
    searchStart_ = QDateTime::currentSecsSinceEpoch();
    doneAtLaunch_ = 0;
    int searched = 0;
    for (int i = 0; i < search_->cores().size(); ++i) {
        if (search_->coreState(i).excluded) continue;
        ++searched;
        doneAtLaunch_ += search_->coreState(i).done;
    }
    {
        QList<int> starts;
        for (const Core &c : search_->cores()) starts << starts_.value(coreKey(c.ccd, c.slot), DEFAULT_START);
        estimateAtLaunch_ = search_->confirming() ? cosearch::VERIFY_SECONDS / 60
            : Search::estimateMinutes(search_->mode(), search_->level(), std::max(searched - doneAtLaunch_, 1), search_->limit(), search_->tuning(), starts);
    }
    updateEstimate();
    refreshTable();
    updateResultButtons();
    priorScene_ = scenes::activeScene();
    guard_->start();
    auto begin = [this] {
        startScene_ = scenes::activeScene();
        note(tr("%1 search, %2 level, limit %3. Every tested candidate will be cleared afterward.")
                 .arg(search_->mode() == Mode::AllCore ? tr("All-core") : tr("Per-core"), name(search_->level())).arg(search_->limit()));
        if (search_->mode() == Mode::PerCore)
            note(tr("Per-core: step %1. A core's first failure after a pass ends it at its last passing value; a failed "
                    "Start climbs back by the step. You can change the step during the run.").arg(search_->tuning().step));
        // Baseline: only kernel events logged after this point count against a trial.
        kmsgSeq_ = 0;
        scanKernel([this](bool ok, const QJsonArray &, quint64 last) {
            if (!running_ || resetting_) return;
            if (!ok) { reset(tr("Cannot read classified kernel events; not starting.")); return; }
            kmsgSeq_ = last;
            if (mceCounters().isEmpty())
                note(tr("MCA threshold counters are not readable here: corrected errors count only once the kernel logs them "
                        "(its MCE poll can take up to 5 minutes), so a pass is weaker evidence."));
            step();
        });
    };
    const QString scene = scene_->currentData().toString();
    auto *win = parentWidget() ? qobject_cast<MainWindow *>(parentWidget()->window()) : nullptr;
    if (scene.isEmpty() || scene == priorScene_) { begin(); return; }
    if (!win || !win->scenes()) { reset(tr("Scenes are not available here; choose \"keep the current settings\".")); return; }
    sceneApplying_ = true;
    note(tr("Applying the CO search scene '%1'…").arg(scene));
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(win->scenes(), &SceneEngine::finished, this, [this, conn, scene, begin](const QString &n, bool ok, const QStringList &log) {
        if (n != scene) return;
        disconnect(*conn);
        sceneApplying_ = false;
        if (!running_ || resetting_) return;
        if (!ok) { reset(tr("The CO search scene failed to apply: %1").arg(log.filter(QStringLiteral("✗")).join("; "))); return; }
        if (abort_) { reset(tr("Cancelled")); return; }
        begin();
    });
    win->scenes()->apply(scene);
}

void CoSearchDialog::step() {
    if (!running_ || resetting_) return;
    if (abort_) { reset(tr("Cancelled")); return; }
    // After a clean exit the trial that was cut short runs again; the snapshot is from after its next().
    trial_ = replay_ ? std::exchange(replay_, std::nullopt) : search_->next();
    applyTuning_->setEnabled(trial_.has_value() && search_->perCoreSearching());
    refreshTable();
    if (!trial_) {
        finished_ = true;
        // The final verification's curve is still applied (its journal stays pending): measure throughput with it now,
        // the stock run follows the reset (compareThroughput), so both run on an equally warm CPU.
        if (!confirmPassed_) { reset(tr("Search finished")); return; }
        note(tr("Measuring single-core throughput with the found curve (clock-stretch check)…"), theme::INFO);
        measureThroughput([this](const QHash<int, int> &chunks, int code) {
            if (abort_) { reset(tr("Cancelled")); return; }
            if (code == 1) {
                finished_ = false;  // a wrong result with the found curve: not verified
                note(tr("Wrong result during the throughput run with the found curve: it is NOT stable and is not saved as verified."), theme::DANGER);
            }
            curveRun_ = code == 0 ? chunks : QHash<int, int>{};
            reset(tr("Search finished"));
        });
        return;
    }
    ++trialNo_;
    evidence_ = {};
    activeCpu_ = -1;
    if (!writeJournal()) { reset(tr("Cannot durably save the recovery journal; no CO offset was written.")); return; }
    const bool all = trial_->allCore.has_value();
    QJsonObject req;
    if (all) {
        req = {{"op", "set_coall"}, {"params", QJsonObject{{"value", *trial_->allCore}}}};
        expectedCurve_ = *trial_->allCore == 0 ? QStringLiteral("stock") : QStringLiteral("all-core %1").arg(*trial_->allCore);
    } else {
        QJsonArray entries;
        for (int i = 0; i < search_->cores().size(); ++i) {
            const Core &c = search_->cores()[i];
            entries.append(QJsonObject{{"ccd", c.ccd}, {"ccx", 0}, {"core", c.slot}, {"coper", trial_->perCore[i]}});
        }
        req = {{"op", "set_coper_batch"}, {"params", QJsonObject{{"entries", entries}}}};
        expectedCurve_ = QStringLiteral("per-core");
    }
    note(tr("Trial %1: %2 (about %3 s plus setup). %4").arg(trialNo_).arg(trial_->what).arg(trial_->seconds()).arg(search_->stageText()), theme::INFO);
    mceBefore_ = mceCounters();
    applying_ = true;
    privileged::run(privileged::helperPath("ryzen-co-helper"), req, this, [this](const privileged::Result &r) {
        applying_ = false;
        if (!running_) return;
        if (!r.ok()) { reset(tr("SMU write rejected: %1").arg(r.message())); return; }
        if (abort_) { reset(tr("Cancelled after in-flight write")); return; }
        watchSeq_ = kmsgSeq_;
        mceWatch_ = mceBefore_;
        watch_->start();
        testIndex_ = 0;
        outcome_ = Outcome{true, {}};
        runTest();
    }, 120000);
}

void CoSearchDialog::runTest() {
    const Test &t = trial_->tests[testIndex_];
    test_ = new QProcess(this);
    testBuf_.clear();
    testEnd_ = {};
    testStart_ = QDateTime::currentSecsSinceEpoch();
    testDeadline_ = testStart_ + t.seconds + 120;  // allow CPU reference calculation and process startup
    logLine(log_, {}, QStringLiteral("centurion-cpustress ") + t.args.join(' '), theme::MUTED);
    connect(test_, &QProcess::readyReadStandardOutput, this, &CoSearchDialog::testOutput);
    connect(test_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) { testFinished(code, status != QProcess::NormalExit); });
    connect(test_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) { note(tr("CPU test failed to start."), theme::DANGER); cancel(); }
    });
    test_->start(testProgram_, t.args);
}

void CoSearchDialog::testOutput() {
    testBuf_ += test_->readAllStandardOutput();
    int nl;
    while ((nl = testBuf_.indexOf('\n')) >= 0) {
        const QJsonObject o = QJsonDocument::fromJson(testBuf_.left(nl)).object();
        testBuf_.remove(0, nl + 1);
        const QString ev = o.value("ev").toString();
        if (ev == "end") testEnd_ = o;
        else if (ev == "core" && o.value("cpu").toInt(-1) != activeCpu_) {
            // Single-core parts: which core carries the load now, kept in the journal for a crash.
            activeCpu_ = o.value("cpu").toInt(-1);
            writeJournal();
        }
        else if (ev == "error") logLine(log_, {}, tr("          wrong result on CPU %1").arg(o.value("cpu").toInt()).trimmed(), theme::DANGER);
        else if (ev == "phase") {
            const QJsonValue mhz = o.value("mhz_max");
            if (mhz.isDouble() && search_->perCoreSearching())  // a single core under test: its peak boost
                for (int i = 0; i < trial_->perCore.size(); ++i)
                    if (trial_->perCore[i] != 0) peakMhz_[i] = std::max(peakMhz_.value(i), mhz.toInt());
            status_->setText(tr("Trial %1: %2 · test %3/%4 · cycle %5 · %6 · %7%8")
                .arg(trialNo_).arg(trial_->what).arg(testIndex_ + 1).arg(trial_->tests.size()).arg(o.value("cycle").toInt() + 1)
                .arg(o.value("kernel").toString(), o.value("phase").toString(),
                     mhz.isDouble() ? QStringLiteral(" · up to %1 MHz").arg(mhz.toInt()) : QString()));
        }
    }
}

void CoSearchDialog::testFinished(int code, bool crashed) {
    if (!test_) return;
    testOutput();                                            // what is left in the pipe
    if (!testBuf_.isEmpty()) { testBuf_ += '\n'; testOutput(); }  // a last line without its newline
    test_->deleteLater();
    test_ = nullptr;
    if (abort_) { reset(tr("Cancelled while testing")); return; }
    const QString status = testEnd_.value("status").toString();
    if (crashed) {
        // A stress process that dies by a signal on an undervolted core is a failure of that offset.
        outcome_ = Outcome{false, {}, true};
        note(tr("CPU test crashed (exit status %1). Applying additional CO safety margin.").arg(code), theme::DANGER);
    } else if (status == "fail" && code == 1) {
        QList<int> cpus;
        for (const auto &v : testEnd_.value("failed").toArray()) cpus << v.toInt();
        outcome_ = Outcome{false, cpus};
    } else if (status != "pass" || code != 0) {
        reset(tr("CPU test did not complete (%1): %2").arg(status.isEmpty() ? QString::number(code) : status, testEnd_.value("msg").toString()));
        return;
    } else if (++testIndex_ < trial_->tests.size()) {
        runTest();
        return;
    }
    // Allow the Health kernel-event debounce to flush before interpreting this result.
    QTimer::singleShot(2500, this, [this] {
        if (abort_) { reset(tr("Cancelled after CPU test")); return; }
        validateFaultLog(outcome_);
    });
}

void CoSearchDialog::scanKernel(std::function<void(bool, const QJsonArray &, quint64)> then) {
    // health-reader (read-only, silent polkit) reads /dev/kmsg directly. health.log is not used: with
    // kernel.dmesg_restrict=1 the Health tab persists events only every 5 minutes, and the file does not
    // exist until the first event, so it can neither prove a clean trial nor be relied on to exist.
    privileged::run(privileged::helperPath("health-reader"), QJsonObject{{"since", double(kmsgSeq_)}}, this,
        [then](const privileged::Result &r) {
            then(r.ok(), r.json.value("events").toArray(), quint64(r.json.value("last_seq").toDouble()));
        }, 30000);
}

void CoSearchDialog::validateFaultLog(Outcome o) {
    scanKernel([this, o](bool ok, const QJsonArray &events, quint64 last) mutable {
        if (!running_ || resetting_) return;
        if (abort_) { reset(tr("Cancelled after CPU test")); return; }
        // An unreadable kernel log is NOT evidence of a clean trial.
        if (!ok) { reset(tr("Cannot read classified kernel events; stopping without claiming a clean test.")); return; }
        kmsgSeq_ = std::max(kmsgSeq_, last);
        for (const QJsonValue &v : events) {
            const QJsonObject e = v.toObject();
            const QString kind = e.value("kind").toString();
            if (kind == "thermal") {
                // CPU temperature is not a CO Search stop criterion. Firmware still manages
                // its own thermal throttling/limits. A kernel emergency shutdown is different:
                // continuing to stress after a critical platform event is not defensible.
                if (e.value("level").toString() == "critical") {
                    reset(tr("Critical platform emergency event: %1").arg(e.value("title").toString()));
                    return;
                }
                note(tr("CPU throttling event observed (continuing; no CO temperature limit): %1")
                     .arg(e.value("title").toString()), theme::WARN);
                continue;
            }
            if (kind == "mce" || kind == "lockup" || kind == "oops") {
                note(tr("Hardware fault reported during trial: %1").arg(e.value("title").toString()), theme::DANGER);
                o.pass = false;
            }
        }
        // Corrected errors the kernel has not polled yet: any threshold counter that moved, traced to its
        // CPU (machinecheckN belongs to CPU N).
        const QHash<QString, QString> now = mceCounters();
        for (auto i = mceBefore_.cbegin(); i != mceBefore_.cend(); ++i) {
            const auto n = now.constFind(i.key());
            if (n == now.cend() || n.value() == i.value()) continue;
            const QString bank = QFileInfo(i.key()).dir().path().section('/', -3, -2);
            note(tr("Corrected machine-check errors counted during trial (%1: %2 → %3)").arg(bank, i.value(), n.value()), theme::DANGER);
            o.pass = false;
            bool num = false;
            const int cpu = bank.section('/', 0, 0).mid(QStringLiteral("machinecheck").size()).toInt(&num);
            if (num && !o.cpus.contains(cpu)) o.cpus << cpu;
        }
        // A wrong result or a machine-check from a different stock core cannot be attributed
        // to the candidate under test. Stop instead of skewing that core's search boundary.
        if (search_->mode() == Mode::PerCore && trial_ && !trial_->perCore.isEmpty() && !o.pass) {
            QList<int> activeCpus;
            int activeCores = 0;
            for (int i = 0; i < trial_->perCore.size(); ++i) {
                if (trial_->perCore[i] == 0 || i >= search_->cores().size()) continue;
                ++activeCores;
                activeCpus.append(search_->cores()[i].cpus);
            }
            // Multiple nonzero cores means final full-set confirmation, not an isolated probe.
            if (activeCores == 1) {
                for (int cpu : o.cpus) {
                    if (!activeCpus.contains(cpu)) {
                        reset(tr("Hardware error on CPU %1 outside the isolated core; cannot attribute failure safely.").arg(cpu));
                        return;
                    }
                }
            }
        }
        if (o.interrupted && trial_ && !saveCrashGuard(*search_, *trial_)) {
            reset(tr("Cannot persist a crash guard after a stress worker crash; stopping."));
            return;
        }
        finishTrial(o);
    });
}

void CoSearchDialog::finishTrial(const Outcome &o) {
    watch_->stop();
    QJsonArray cpus;
    for (int c : o.cpus) cpus << c;
    int probed = -1;  // per-core: the core this isolated probe was for
    if (search_->perCoreSearching())
        for (int i = 0; i < trial_->perCore.size(); ++i) if (trial_->perCore[i] != 0) probed = i;
    QJsonObject attempt{{"t", double(testStart_)}, {"what", trial_->what}, {"pass", o.pass}, {"cpus", cpus}};
    if (probed >= 0) { attempt["core"] = probed; attempt["value"] = trial_->perCore[probed]; }  // the row's history tooltip
    attempts_.append(attempt);
    confirmPassed_ = search_->confirming() && o.pass;
    QStringList named;
    for (int c : o.cpus) named << QString::number(c);
    note(o.pass ? tr("Passed: %1").arg(trial_->what)
                : tr("FAILED: %1%2").arg(trial_->what, named.isEmpty() ? QString() : tr(" (CPU %1)").arg(named.join(','))),
         o.pass ? theme::OK : theme::DANGER);
    search_->report(o);
    if (probed >= 0 && search_->coreState(probed).done) {
        const Core &c = search_->cores()[probed];
        note(tr("CCD%1 / %2 finished at %3: it is not tested more negative than that again.")
             .arg(c.ccd).arg(c.slot).arg(search_->coreState(probed).pass), theme::OK);
    }
    trial_.reset();
    applyTuning_->setEnabled(false);
    updateEstimate();
    // Keep the previous trial marked PENDING on disk until a subsequent candidate has
    // its own durable journal, or until stock reset completes. Its CO voltage is still
    // applied in this gap: a late reboot must invalidate THAT trial, not claim "idle".
    // A failed write for the next trial triggers reset; the old candidate stays quarantinable.
    refreshTable();
    QTimer::singleShot(0, this, &CoSearchDialog::step);
}

void CoSearchDialog::cancel() {
    if (!running_ || resetting_) return;
    abort_ = true;
    stop_->setEnabled(false);
    applyTuning_->setEnabled(false);
    if (test_ && test_->state() != QProcess::NotRunning) {
        QPointer<QProcess> pending(test_);
        test_->terminate();
        QTimer::singleShot(3000, this, [pending] {
            if (pending && pending->state() != QProcess::NotRunning) pending->kill();
        });
        return;
    }
    if (perfTest_ && perfTest_->state() != QProcess::NotRunning) { perfTest_->terminate(); return; }  // its handler resets
    if (applying_ || sceneApplying_) { note(tr("Waiting for in-flight write before resetting.")); return; }
    reset(tr("Cancelled by user / guard"));
}

void CoSearchDialog::reset(const QString &reason) {
    if (!running_ || resetting_) return;
    resetting_ = true;
    watch_->stop();
    applyTuning_->setEnabled(false);
    note(reason + tr(" — requesting stock reset."), theme::WARN);
    privileged::run(privileged::helperPath("ryzen-co-helper"), QJsonObject{{"op", "reset"}}, this,
        [this, reason](const privileged::Result &r) { done(r.ok(), reason + (r.ok() ? QString() : QStringLiteral("; reset FAILED: ") + r.message())); }, 120000);
}

bool CoSearchDialog::saveProfile(const QString &name, int margin) const {
    // Same file format as Ryzen Curve → Save Profile, so the result loads there like any other profile.
    // `margin`: points towards stock on every undervolted core (never past 0). "bios": what it was found under.
    QList<int> v = search_->values();
    for (int &x : v) x = std::min(0, x + margin);
    QJsonArray cores;
    if (search_->mode() == Mode::PerCore)
        for (int i = 0; i < search_->cores().size(); ++i)
            cores.append(QJsonObject{{"ccd", search_->cores()[i].ccd}, {"ccx", 0}, {"slot", search_->cores()[i].slot},
                                     {"coper", v[i]}, {"disabled", false}});
    int ccds = 0;
    for (const Core &c : search_->cores()) ccds = std::max(ccds, c.ccd + 1);
    const QJsonObject profile{{"version", "2.0.0"}, {"ccd_count", ccds},
                              {"coall", search_->mode() == Mode::AllCore ? QJsonValue(v.value(0)) : QJsonValue()}, {"cores", cores},
                              {"bios", biosVersion()}};
    QDir().mkpath(ryzen::profilesDir());
    QSaveFile f(ryzen::profilesDir() + '/' + name + ".json");
    const QByteArray data = QJsonDocument(profile).toJson();
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size() && f.commit();
}

void CoSearchDialog::updateResultButtons() {
    bool any = false;
    if (search_) for (int x : search_->values()) any |= x < 0;
    saveResult_->setEnabled(!running_ && any);
    loadResult_->setEnabled(!running_ && any);
}

void CoSearchDialog::saveResultAs() {
    if (running_ || !search_) return;
    const QString date = QDateTime::currentDateTime().toString("yyyy-MM-dd HHmm");
    QString name = finished_ ? tr("CO search %1").arg(date) : tr("CO search %1 (not verified)").arg(date);
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Save Result as Profile"));
    auto *form = new QFormLayout(&dlg);
    if (!finished_) {
        auto *warn = new QLabel(tr("This search did not finish its verification: these values passed only the tests run so far."));
        warn->setWordWrap(true);
        warn->setStyleSheet(QStringLiteral("color: %1;").arg(theme::WARN));
        form->addRow(warn);
    }
    auto *nameEdit = new QLineEdit(name);
    nameEdit->setMinimumWidth(320);
    form->addRow(tr("Profile name:"), nameEdit);
    auto *margin = new QSpinBox;
    margin->setRange(0, 5);
    margin->setSuffix(tr(" point(s) towards stock"));
    margin->setToolTip(tr("Saved value = found value + margin on every undervolted core (never past 0). Many users keep 1-2 "
                          "points of margin for daily use."));
    form->addRow(tr("Margin:"), margin);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    form->addRow(box);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    name = nameEdit->text().trimmed();
    if (name.isEmpty()) return;
    if (name.contains('/') || name.contains('\\') || name.startsWith('.') || name.size() > 100) {
        QMessageBox::warning(this, tr("Invalid name"), tr("Use a name without '/' or '\\', not starting with '.', up to 100 characters."));
        return;
    }
    if (QFileInfo::exists(ryzen::profilesDir() + '/' + name + ".json") &&
        QMessageBox::question(this, tr("Overwrite profile"), tr("A profile named '%1' exists. Replace it?").arg(name),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    if (!saveProfile(name, margin->value())) { QMessageBox::critical(this, tr("Save failed"), tr("Could not write the profile.")); return; }
    reloadPresets();
    note((finished_ ? tr("Result saved as Ryzen Curve profile '%1'.").arg(name)
                    : tr("Result saved as Ryzen Curve profile '%1' (NOT verified: the search did not finish).").arg(name))
             + (margin->value() ? tr(" Margin: +%1 on every undervolted core.").arg(margin->value()) : QString()),
         finished_ ? theme::OK : theme::WARN);
}

void CoSearchDialog::loadResultIntoStarts() {
    if (running_ || !search_) return;
    const QList<int> v = search_->values();
    QStringList set;
    for (int i = 0; i < search_->cores().size(); ++i) {
        const Core &c = search_->cores()[i];
        const int key = coreKey(c.ccd, c.slot);
        if (search_->mode() == Mode::PerCore && search_->coreState(i).excluded) continue;  // stays unticked
        starts_[key] = std::clamp(v[i], -50, 0);
        set << QStringLiteral("CCD%1/%2 %3").arg(c.ccd).arg(c.slot).arg(starts_[key]);
    }
    preset_->setCurrentIndex(0);
    mode_->setCurrentIndex(mode_->findData(int(Mode::PerCore)));
    saveSettings();
    refreshTable();
    updateEstimate();
    note(tr("Result copied into the Start column: %1").arg(set.join(", ")));
}

void CoSearchDialog::watchTick() {
    if (!running_ || !trial_ || resetting_) { watch_->stop(); return; }
    QList<coevidence::Record> fresh;
    // MCA threshold counters move the moment a corrected error happens; the kernel itself may log it
    // only at its next poll (5 minutes), too late before a crash.
    const QHash<QString, QString> now = mceCounters();
    for (auto i = now.cbegin(); i != now.cend(); ++i)
        if (mceWatch_.contains(i.key()) && mceWatch_.value(i.key()) != i.value())
            if (const auto r = coevidence::fromCounterPath(i.key())) fresh << *r;
    mceWatch_ = now;
    auto keep = [this](const QList<coevidence::Record> &records) {
        if (records.isEmpty() || !running_ || !trial_) return;
        for (const auto &r : records) {
            if (evidence_.size() >= 64) break;
            evidence_.append(r.toJson());
            note(tr("Watcher: %1").arg(r.text), theme::DANGER);
        }
        writeJournal();  // durable now: a crash a moment later must not lose it
    };
    keep(fresh);
    if (watchBusy_) return;
    watchBusy_ = true;
    privileged::run(privileged::helperPath("health-reader"), QJsonObject{{"since", double(watchSeq_)}}, this,
        [this, keep](const privileged::Result &r) {
            watchBusy_ = false;
            if (!r.ok()) return;
            watchSeq_ = std::max(watchSeq_, quint64(r.json.value("last_seq").toDouble()));
            QList<coevidence::Record> records;
            for (const QJsonValue &v : r.json.value("events").toArray()) {
                const QJsonObject e = v.toObject();
                if (e.value("kind").toString() != "mce" && e.value("kind").toString() != "lockup") continue;
                if (const auto rec = coevidence::fromKernelText(e.value("text").toString(), QStringLiteral("watch-kernel"))) records << *rec;
            }
            keep(records);
        }, 30000);
}

void CoSearchDialog::done(bool resetOk, const QString &reason) {
    guard_->stop();
    watch_->stop();
    // Persist a bounded evidence record. "passed" means only the tests run here, not curve stability.
    QDir().mkpath(stateDir());
    const QString path = stateDir() + "/co-search-runs.json";
    QJsonArray values, topology;
    if (search_) {
        for (int x : search_->values()) values << x;
        for (int i = 0; i < search_->cores().size(); ++i) {
            const auto &c = search_->cores().at(i);
            QJsonArray cpus; for (int cpu : c.cpus) cpus << cpu;
            QJsonObject row{{"ccd", c.ccd}, {"slot", c.slot}, {"cpus", cpus}, {"value", values.at(i)}};
            if (search_->mode() == Mode::PerCore) {
                const auto st = search_->coreState(i);
                row["walk_done"] = st.done; row["excluded"] = st.excluded;
                row["last_pass"] = st.pass;
                if (st.failed) row["failed"] = *st.failed;
            }
            topology << row;
        }
    }
    const bool archived = coarchive::append(path, QJsonObject{
        {"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"t", QDateTime::currentSecsSinceEpoch()}, {"boot", bootId_},
        {"mode", search_ && search_->mode() == Mode::PerCore ? "per" : "all"},
        {"level", search_ ? name(search_->level()) : QString()}, {"values", values},
        {"topology", topology}, {"reset_ok", resetOk}, {"complete", finished_ && resetOk},
        {"verification_passed", finished_ && confirmPassed_},
        {"bios", biosVersion()}, {"verify_profile", verifiedProfile_}, {"notes", reason},
        {"attempts", attempts_}, {"reboots", hardReboots_}});
    if (!archived && !quitting_) note(tr("Could not save CO session history; existing archive was preserved."), theme::WARN);
    // A freeze or an application quit leaves the journal behind for a resume; a search that ended here does not.
    // Never erase evidence merely because the algorithm finished: an unsuccessful stock
    // reset can leave the last CO value applied and the recovery journal is still needed.
    if (!quitting_ && resetOk) QFile::remove(journalPath());
    running_ = false;
    resetting_ = false;
    sceneApplying_ = false;
    replay_.reset();
    updateEstimate();
    trial_.reset();
    if (quitting_) return;
    go_->setEnabled(resetOk);
    for (QWidget *w : std::initializer_list<QWidget *>{mode_, level_, limit_, scene_, verifyBtn_, guardsBtn_}) w->setEnabled(true);
    stop_->setEnabled(false);
    refreshTable();
    updateResultButtons();
    note(reason, resetOk ? nullptr : theme::DANGER);
    // Searches run for hours, mostly unattended: say how it ended, outside the window too.
    {
        QStringList v;
        if (search_) for (int x : search_->values()) v << QString::number(x);
        const QString title = !resetOk ? tr("CO Search: reset FAILED")
                            : finished_ ? tr("CO Search finished") : tr("CO Search stopped");
        const QString body = !resetOk ? tr("The CPU curve could not be reset. Use Ryzen Curve → Reset, or reboot.")
                           : reason + (v.isEmpty() ? QString() : QStringLiteral("\n") + tr("Values: %1").arg(v.join(' ')));
        notifier_->notify(QStringLiteral("co-search"), title, body,
                          {NotifyAction{tr("Show"), [this] { show(); raise(); activateWindow(); }}}, 0, [] {});
    }
    if (!resetOk) {
        QMessageBox::critical(this, tr("Reset failed"), tr("CPU curve reset could not be confirmed. Use Ryzen Curve → Reset, or reboot before continuing."));
        return;
    }
    if (finished_ && search_) {
        QStringList v;
        for (int x : search_->values()) v << QString::number(x);
        const QString date = QDateTime::currentDateTime().toString("yyyy-MM-dd HHmm");
        const QString profile = verifiedProfile_.isEmpty() ? QStringLiteral("CO search %1").arg(date)
                                                           : QStringLiteral("%1 (verified %2)").arg(verifiedProfile_.left(60), date);
        const bool saved = saveProfile(profile);
        reloadPresets();  // the result can be the next search's preset
        note(tr("SMU reset confirmed. Result: %1. %2").arg(v.join(' '),
             !saved ? tr("Saving the profile FAILED.")
                               : tr("Saved as Ryzen Curve profile '%1': load and apply it there. Many users keep 1-2 points of margin "
                                    "above a searched value for daily use; run long tests before relying on it.").arg(profile)),
             saved ? theme::OK : theme::DANGER);
    } else {
        note(tr("SMU reset confirmed. The search did not finish; nothing was saved automatically. Save result keeps the "
                "values found so far, marked as not verified."), theme::WARN);
    }
    // The scene that was active before the search, if the search switched to its own.
    auto restoreScene = [this] {
        auto *win = parentWidget() ? qobject_cast<MainWindow *>(parentWidget()->window()) : nullptr;
        if (win && win->scenes() && scenes::activeScene() != priorScene_) {
            if (priorScene_.isEmpty()) { note(tr("No scene was active before the search: the CO search scene stays applied.")); return; }
            note(tr("Applying the previous scene '%1' again.").arg(priorScene_));
            win->scenes()->apply(priorScene_);
        }
    };
    // Stock run now, under the same scene and on a CPU as warm as the curve's run; the scene changes after it.
    if (!curveRun_.isEmpty()) compareThroughput(restoreScene);
    else restoreScene();
}
