#include "cosearchdialog.h"
#include "curvepage.h"
#include "mainwindow.h"
#include "privileged.h"
#include "ryzentab.h"
#include "scenes.h"
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QSaveFile>
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
static QString settingsPath() { return QFileInfo(scenes::dir()).path() + "/co-search.json"; }
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

// Retain failed candidates across NEW searches too, so "start again" cannot silently
// rediscover the same destructive offset. A record is specific to the physical topology.
static bool saveCrashGuard(const Search &s, const Trial &t) {
    if (t.allCore.has_value() || t.perCore.size() != s.cores().size()) return true;
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
                         {"failed", mostConservative}};
    QDir().mkpath(stateDir());
    QSaveFile f(crashGuardPath());
    if (!f.open(QIODevice::WriteOnly)) return false;
    const QByteArray data = QJsonDocument(QJsonObject{{"version", 1}, {"guards", fresh}}).toJson();
    return f.write(data) == data.size() && f.commit();
}

static bool applyCrashGuards(Search &s) {
    const auto guards = readCrashGuards();
    if (!guards) return false;
    for (const QJsonValue &v : *guards) {
        const QJsonObject o = v.toObject();
        QList<int> cpus;
        for (const QJsonValue &cpu : o.value("cpus").toArray()) cpus << cpu.toInt(-1);
        s.guardAgainstCrash(o.value("ccd").toInt(-1), o.value("slot").toInt(-1), cpus,
                            o.value("failed").toInt(0));
    }
    return true;
}

CoSearchDialog::CoSearchDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Ryzen CO Search Assistant (experimental)"));
    resize(820, 640);
    auto *v = new QVBoxLayout(this);
    auto *explainer = new QLabel(tr(
        "Finds negative Curve Optimizer offsets with FFT / integer workload and boost/idle transitions. "
        "All-core runs 60-second cycles (40 s steady + 20 s transitions) to find one "
        "offset for every core; per-core begins at an editable starting offset with other cores at stock, "
        "then uses your coarse step and adaptive 3/2/1-point refinement after failure. Full-length tests verify the "
        "final individual offsets and their combined stability. "
        "The curve is reset to stock when the search ends; the result is saved as a Ryzen Curve profile, not applied. "
        "Passing is NOT proof of long-term stability. A hard freeze may still require rebooting: the search can resume after it."));
    explainer->setWordWrap(true);
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
    level_->setToolTip(tr("Per-core: 24-second probes (12 s steady + all 10 boost/idle patterns); "
                          "60-second cycles for final verification.\n"
                          "Fast: 1 probe cycle, 1 confirmation cycle.\n"
                          "Medium: 2 probe cycles, 2 confirmation cycles.\n"
                          "Long: 3 probe cycles, 3 per-core and 5 all-core confirmation cycles. "
                          "All-core trials use full 60-second cycles."));
    limit_ = new QSpinBox;
    limit_->setRange(-50, -5);
    limit_->setValue(-30);
    limit_->setToolTip(tr("Most negative offset to try. The CPU's own allowed range (ryzen-co-helper) still applies."));
    row1->addWidget(mode_);
    row1->addWidget(new QLabel(tr("Level:")));
    row1->addWidget(level_);
    row1->addWidget(new QLabel(tr("Limit:")));
    row1->addWidget(limit_);
    row1->addStretch();
    form->addRow(tr("Search:"), row1);
    auto *tuningRow = new QHBoxLayout;
    initial_ = new QSpinBox;
    initial_->setRange(-50, -1);
    initial_->setValue(-15);
    initial_->setToolTip(tr("First per-core test value. Defaults to -15; never exceeds the configured search limit or a stored reboot guard."));
    coarseStep_ = new QSpinBox;
    coarseStep_->setRange(1, 20);
    coarseStep_->setValue(5);
    coarseStep_->setToolTip(tr("Coarse downward step after a successful per-core probe. Default: 5 CO points."));
    refineStep_ = new QSpinBox;
    refineStep_->setRange(1, 10);
    refineStep_->setValue(3);
    refineStep_->setToolTip(tr("First failure switches to this refinement step (default 3). Wide gaps use its multiples; narrow gaps automatically use 2 then 1 point."));
    applyTuning_ = new QPushButton(tr("Apply to next trials"));
    applyTuning_->setToolTip(tr("You can change the three values during an active per-core search. Changes affect only future candidates, never the test already running or stored reboot exclusions."));
    applyTuning_->setEnabled(false);
    tuningRow->addWidget(new QLabel(tr("Start:")));
    tuningRow->addWidget(initial_);
    tuningRow->addWidget(new QLabel(tr("Coarse step:")));
    tuningRow->addWidget(coarseStep_);
    tuningRow->addWidget(new QLabel(tr("After failure:")));
    tuningRow->addWidget(refineStep_);
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
    v->addWidget(estimate_);

    auto *row = new QHBoxLayout;
    row->addStretch();
    go_ = new QPushButton(tr("Start search"));
    stop_ = new QPushButton(tr("Stop and reset"));
    stop_->setEnabled(false);
    row->addWidget(go_);
    row->addWidget(stop_);
    v->addLayout(row);
    status_ = new QLabel(tr("Not started. The current CPU curve must be stock."));
    status_->setWordWrap(true);
    v->addWidget(status_);
    table_ = new QTableWidget(0, 3);
    table_->setHorizontalHeaderLabels({tr("Core"), tr("Offset"), tr("Now")});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->hide();
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    auto *split = new QHBoxLayout;
    split->addWidget(table_, 2);
    split->addWidget(log_, 5);
    v->addLayout(split, 1);

    loadSettings();
    for (QComboBox *c : {mode_, level_}) connect(c, &QComboBox::currentIndexChanged, this, &CoSearchDialog::updateEstimate);
    for (QSpinBox *spin : {limit_, initial_, coarseStep_, refineStep_})
        connect(spin, &QSpinBox::valueChanged, this, &CoSearchDialog::updateEstimate);
    connect(applyTuning_, &QPushButton::clicked, this, &CoSearchDialog::applyTuning);
    updateEstimate();

    guard_ = new QTimer(this);
    guard_->setInterval(1000);
    connect(guard_, &QTimer::timeout, this, [this] {
        if (!running_ || resetting_ || sceneApplying_) return;
        if (scenes::gameSessions() > 0 || scenes::activeScene() != startScene_) {
            note(tr("Scene or game tuning ownership changed; cancelling search.")); cancel(); return;
        }
        if (!applying_ && trial_ && CurvePage::currentState("cpu") != expectedCurve_) {
            note(tr("CPU curve was changed externally during this trial; cancelling.")); cancel(); return;
        }
        if (test_ && test_->state() != QProcess::NotRunning && QDateTime::currentSecsSinceEpoch() > testDeadline_) {
            note(tr("CPU test exceeded safety timeout; terminating.")); cancel();
        }
    });
    connect(go_, &QPushButton::clicked, this, &CoSearchDialog::start);
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
    // Even an intentional GUI close must not replay the same interrupted CO candidate later.
    if (ok && search_ && trial_) {
        const Trial t = *trial_;
        if (saveCrashGuard(*search_, t)) {
            search_->report(Outcome{false, {}, true});
            trial_.reset();
            writeJournal();
        }
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

void CoSearchDialog::note(const QString &s) {
    log_->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss") + "  " + s);
    status_->setText(s);
}

Tuning CoSearchDialog::tuning() const {
    return {initial_->value(), coarseStep_->value(), refineStep_->value()};
}

void CoSearchDialog::updateEstimate() {
    const auto mode = Mode(mode_->currentData().toInt());
    const bool perCore = mode == Mode::PerCore;
    for (QSpinBox *spin : {initial_, coarseStep_, refineStep_}) spin->setEnabled(perCore);
    // The previous CPU curve may still be applied between finishTrial() and step().
    // Do not replace its pending journal with a non-pending snapshot in that gap.
    applyTuning_->setEnabled(perCore && running_ && !resetting_ && !abort_ &&
                             search_.has_value() && search_->perCoreSearching() && trial_.has_value());
    const auto level = levelFrom(level_->currentData().toString()).value_or(Level::Fast);
    int cores = 0;
    for (const auto &l : ryzen::detect().cores) cores += l.size();
    const int m = Search::estimateMinutes(mode, level, std::max(cores, 1), limit_->value(), tuning());
    estimate_->setText(tr("Roughly %1 h %2 min for %3 cores (depends on where the offsets fail).").arg(m / 60).arg(m % 60, 2, 10, QLatin1Char('0')).arg(cores));
}

void CoSearchDialog::applyTuning() {
    if (!running_ || resetting_ || abort_ || !search_ || !search_->perCoreSearching()) return;
    if (!trial_) { note(tr("Changing steps is only available while a candidate is pending.")); return; }
    const Tuning previous = search_->tuning();
    if (!search_->setTuning(tuning())) return;
    if (!writeJournal()) {
        search_->setTuning(previous);
        initial_->setValue(previous.initial);
        coarseStep_->setValue(previous.coarseStep);
        refineStep_->setValue(previous.refineStep);
        note(tr("Could not save updated CO steps; previous safe tuning retained."));
        return;
    }
    saveSettings();
    note(tr("Updated future per-core trials: start %1, coarse step %2, failure step %3 (adaptive to 2/1). Current test unchanged.")
         .arg(tuning().initial).arg(tuning().coarseStep).arg(tuning().refineStep));
}

void CoSearchDialog::loadSettings() {
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
    initial_->setValue(o.value("initial").toInt(initial_->value()));
    coarseStep_->setValue(o.value("coarse_step").toInt(coarseStep_->value()));
    refineStep_->setValue(o.value("refine_step").toInt(refineStep_->value()));
    scene_->setCurrentIndex(std::max(0, scene_->findData(o.value("scene").toString())));
}

void CoSearchDialog::saveSettings() const {
    QDir().mkpath(QFileInfo(settingsPath()).path());
    QSaveFile f(settingsPath());
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(QJsonObject{{"mode", Mode(mode_->currentData().toInt()) == Mode::PerCore ? "per" : "all"},
                                      {"level", level_->currentData().toString()}, {"limit", limit_->value()},
                                      {"initial", initial_->value()}, {"coarse_step", coarseStep_->value()},
                                      {"refine_step", refineStep_->value()},
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
    const QByteArray data = QJsonDocument(QJsonObject{{"version", 3}, {"boot", bootId_},
        {"search", search_->save()}, {"pending", trial_.has_value()},
        {"trial", trial_ ? trialSignature(*trial_) : QJsonObject{}},
        {"scene", scene_->currentData().toString()}, {"prior_scene", priorScene_},
        {"reboots", hardReboots_}, {"attempts", attempts_}}).toJson();
    if (f.write(data) != data.size()) return false;
    return f.commit();  // QSaveFile: durable commit before a new privileged SMU write
}

bool CoSearchDialog::toolsPresent() {
    testProgram_ = QStandardPaths::findExecutable("centurion-cpustress");
    if (testProgram_.isEmpty() || !QFileInfo(privileged::helperPath("ryzen-co-helper")).isExecutable()) {
        QMessageBox::warning(this, tr("Missing tools"), tr("Install centurion-cpustress and ryzen-co-helper first."));
        return false;
    }
    return true;
}

void CoSearchDialog::refreshTable() {
    if (!search_) return;
    const QList<Core> &cores = search_->cores();
    const QList<int> v = search_->values();
    table_->setRowCount(cores.size());
    for (int i = 0; i < cores.size(); ++i) {
        QStringList cpus;
        for (int c : cores[i].cpus) cpus << QString::number(c);
        const bool isolated = trial_ && !trial_->perCore.isEmpty() && search_->mode() == Mode::PerCore;
        const bool testing = isolated && trial_->perCore.value(i) != 0;
        const QStringList cells{QStringLiteral("CCD%1 / %2  (CPU %3)").arg(cores[i].ccd).arg(cores[i].slot).arg(cpus.join(',')),
                                QString::number(v[i]), isolated ? (testing ? tr("testing %1").arg(trial_->perCore[i])
                                                                             : tr("stock (isolated)")) : QString()};
        for (int c = 0; c < 3; ++c) table_->setItem(i, c, new QTableWidgetItem(cells[c]));
    }
    table_->resizeColumnsToContents();
}

void CoSearchDialog::start() {
    if (running_) return;
    if (QFileInfo::exists(journalPath())) {
        // Never overwrite an unresolved crash history with a fresh, unguarded search.
        offerResume();
        return;
    }
    if (!toolsPresent()) return;
    if (CurvePage::currentState("cpu") != "stock" || scenes::gameSessions() > 0) {
        QMessageBox::warning(this, tr("Search locked"), tr("Reset the CPU curve to stock and end game sessions before searching. Existing offsets must not be overwritten.")); return;
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
              "the last helper-written state must explicitly be reset/0 before this search.")); return;
    }
    const ryzen::Layout layout = ryzen::detect();
    const auto mode = Mode(mode_->currentData().toInt());
    if (mode == Mode::PerCore && !layout.parked.isEmpty()) {
        QMessageBox::warning(this, tr("CCD parked"), tr("A CCD is parked by Optimizations: its cores cannot be tested alone. Unpark it first.")); return;
    }
    QList<Core> cores;
    for (auto it = layout.cores.cbegin(); it != layout.cores.cend(); ++it) {
        if (layout.parked.contains(it.key())) continue;
        for (int i = 0; i < it.value().size(); ++i)
            cores << Core{it.key(), i, it.value()[i].cpus, it.value()[i].highestPerf.value_or(-1)};
    }
    if (cores.isEmpty()) { QMessageBox::warning(this, tr("Topology"), tr("No CPU cores detected.")); return; }
    const int minutes = Search::estimateMinutes(mode, levelFrom(level_->currentData().toString()).value_or(Level::Fast), cores.size(), limit_->value(), tuning());
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
    hardReboots_ = 0;
    attempts_ = {};
    trialNo_ = 0;
    launch();
}

void CoSearchDialog::offerResume() {
    if (running_) return;
    QFile f(journalPath());
    if (!f.open(QIODevice::ReadOnly) || f.size() > (1LL << 20)) return;
    const QJsonObject j = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    auto s = Search::load(j.value("search").toObject());
    if (!s) {
        const auto answer = QMessageBox::warning(this, tr("Invalid CO search journal"),
            tr("The interrupted CO Search journal is incompatible or damaged. It cannot be safely resumed. "
               "Discard the old journal? The CPU curve is not changed."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer == QMessageBox::Yes) QFile::remove(journalPath());
        return;
    }
    const int journalVersion = j.value("version").toInt(2);  // legacy: before-next() snapshot
    if (journalVersion != 2 && journalVersion != 3) return;
    const QString currentBoot = readBootId();
    if (currentBoot.isEmpty()) {
        QMessageBox::warning(this, tr("Boot identity"), tr("Cannot verify boot ID; recovery is blocked."));
        return;
    }
    const bool rebooted = j.value("boot").toString() != currentBoot;
    std::optional<Trial> interrupted;
    bool pending = false;
    if (journalVersion == 2) {
        // Upgrade existing journals without ever replaying a known interrupted candidate.
        interrupted = s->next();
        pending = interrupted.has_value();
        if (pending && interrupted->what != j.value("what").toString()) {
            QMessageBox::warning(this, tr("Journal mismatch"), tr("Recovery trial does not match the old journal. No curve was written."));
            return;
        }
    } else {
        if (!j.value("pending").isBool()) {
            QMessageBox::warning(this, tr("Invalid CO search journal"),
                tr("The v3 journal has no valid pending-state marker. Recovery is blocked."));
            return;
        }
        pending = j.value("pending").toBool(false);
        if (pending) {
            interrupted = s->inFlight();
            if (!interrupted || trialSignature(*interrupted) != j.value("trial").toObject()) {
                QMessageBox::warning(this, tr("Journal mismatch"), tr("Saved candidate and search state disagree. Recovery is blocked."));
                return;
            }
        }
    }
    // Quarantine immediately, BEFORE the user can choose Discard. Otherwise discarding the
    // journal and starting a new search would silently repeat the crash-inducing value.
    if (pending && !saveCrashGuard(*s, *interrupted)) {
        QMessageBox::critical(this, tr("Recovery guard failed"),
            tr("Cannot save the interrupted CO candidate to the persistent crash guard. "
               "Neither resuming nor discarding the journal is safe until this is repaired."));
        return;
    }
    int rebootCount = std::max(0, j.value("reboots").toInt());
    if (rebootCount >= 2) {
        const auto answer = QMessageBox::warning(this, tr("CO search reboot lockout"),
            tr("This search has already survived two suspected reboot crashes. It is locked to prevent another reboot loop. "
               "Discard the interrupted search? Recorded core crash guards will remain in effect."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer == QMessageBox::Yes) QFile::remove(journalPath());
        return;
    }
    const QString what = pending ? interrupted->what : tr("the last resolved trial");
    const QString text = pending
        ? tr("The search was interrupted while trying %1. That candidate will be FAILED, excluded with a safety margin and never replayed on resume.").arg(what)
        : tr("The search stopped after its previous trial was already resolved. No candidate will be replayed.");
    const auto answer = QMessageBox::question(this, tr("Resume CO search"),
        text + "\n\n" + tr("Resume after resetting the CPU curve to stock? No discards this interrupted search."),
        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::No);
    if (answer == QMessageBox::No) { QFile::remove(journalPath()); return; }
    if (answer != QMessageBox::Yes || !toolsPresent()) return;
    if (scenes::gameSessions() > 0) {
        QMessageBox::warning(this, tr("Search locked"), tr("End game sessions first."));
        return;
    }
    mode_->setCurrentIndex(mode_->findData(int(s->mode())));
    level_->setCurrentIndex(level_->findData(name(s->level())));
    limit_->setValue(s->limit());
    initial_->setValue(s->tuning().initial);
    coarseStep_->setValue(s->tuning().coarseStep);
    refineStep_->setValue(s->tuning().refineStep);
    scene_->setCurrentIndex(std::max(0, scene_->findData(j.value("scene").toString())));
    attempts_ = j.value("attempts").toArray();
    hardReboots_ = rebootCount + int(rebooted && pending);
    if (pending) {
        s->report(Outcome{false, {}, true});
        attempts_.append(QJsonObject{{"t", double(QDateTime::currentSecsSinceEpoch())},
            {"what", interrupted->what}, {"pass", false},
            {"note", rebooted ? "reboot; unsafe candidate quarantined" : "interrupted; unsafe candidate quarantined"}});
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
    // safely resume from a state where the failed candidate is already excluded.
    if (!writeJournal()) {
        QMessageBox::critical(this, tr("Recovery journal failed"),
            tr("Cannot durably commit the recovered state. No CO value was applied."));
        search_.reset();
        return;
    }
    trialNo_ = attempts_.size();
    if (hardReboots_ >= 2) {
        QMessageBox::critical(this, tr("CO search reboot lockout"),
            tr("Two reboots were detected in this search. The last candidate was marked unsafe, "
               "but automatic search continuation is disabled. Restart CO Search manually after checking system stability."));
        search_.reset();
        return;
    }
    note(pending ? tr("Recovery: %1 marked unsafe and excluded. Resetting to stock first.").arg(what)
                 : tr("Recovery: continuing after last resolved trial. Resetting to stock first."));
    privileged::run(privileged::helperPath("ryzen-co-helper"), QJsonObject{{"op", "reset"}}, this,
        [this](const privileged::Result &r) {
            if (!r.ok()) { note(tr("Reset failed: %1. Not resuming.").arg(r.message())); search_.reset(); return; }
            const QString keep = priorScene_;
            launch();
            priorScene_ = keep;
        }, 120000);
}

void CoSearchDialog::launch() {
    running_ = true;
    abort_ = false;
    finished_ = false;
    bootId_ = readBootId();
    if (bootId_.isEmpty()) { running_ = false; QMessageBox::warning(this, tr("Boot identity"), tr("Cannot verify boot ID.")); return; }
    for (QWidget *w : std::initializer_list<QWidget *>{go_, mode_, level_, limit_, scene_}) w->setEnabled(false);
    stop_->setEnabled(true);
    updateEstimate();
    refreshTable();
    priorScene_ = scenes::activeScene();
    guard_->start();
    auto begin = [this] {
        startScene_ = scenes::activeScene();
        note(tr("%1 search, %2 level, limit %3. Every tested candidate will be cleared afterward.")
                 .arg(search_->mode() == Mode::AllCore ? tr("All-core") : tr("Per-core"), name(search_->level())).arg(search_->limit()));
        if (search_->mode() == Mode::PerCore)
            note(tr("Manual CO tuning: start %1 / coarse step %2 / after failure %3 (then 2 and 1 as needed). You can edit and apply these during the run.")
                 .arg(search_->tuning().initial).arg(search_->tuning().coarseStep).arg(search_->tuning().refineStep));
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
    trial_ = search_->next();
    applyTuning_->setEnabled(trial_.has_value() && search_->perCoreSearching());
    refreshTable();
    if (!trial_) { finished_ = true; reset(tr("Search finished")); return; }
    ++trialNo_;
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
    note(tr("Trial %1: %2 (about %3 s plus setup). %4").arg(trialNo_).arg(trial_->what).arg(trial_->seconds()).arg(search_->stageText()));
    mceBefore_ = mceCounters();
    applying_ = true;
    privileged::run(privileged::helperPath("ryzen-co-helper"), req, this, [this](const privileged::Result &r) {
        applying_ = false;
        if (!running_) return;
        if (!r.ok()) { reset(tr("SMU write rejected: %1").arg(r.message())); return; }
        if (abort_) { reset(tr("Cancelled after in-flight write")); return; }
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
    log_->appendPlainText(QStringLiteral("          centurion-cpustress ") + t.args.join(' '));
    connect(test_, &QProcess::readyReadStandardOutput, this, &CoSearchDialog::testOutput);
    connect(test_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) { testFinished(code, status != QProcess::NormalExit); });
    connect(test_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) { note(tr("CPU test failed to start.")); cancel(); }
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
        else if (ev == "error") log_->appendPlainText(tr("          wrong result on CPU %1").arg(o.value("cpu").toInt()));
        else if (ev == "phase") {
            const QJsonValue mhz = o.value("mhz_max");
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
        note(tr("CPU test crashed (exit status %1). Applying additional CO safety margin.").arg(code));
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
                     .arg(e.value("title").toString()));
                continue;
            }
            if (kind == "mce" || kind == "lockup" || kind == "oops") {
                note(tr("Hardware fault reported during trial: %1").arg(e.value("title").toString()));
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
            note(tr("Corrected machine-check errors counted during trial (%1: %2 → %3)").arg(bank, i.value(), n.value()));
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
    QJsonArray cpus;
    for (int c : o.cpus) cpus << c;
    attempts_.append(QJsonObject{{"t", double(testStart_)}, {"what", trial_->what}, {"pass", o.pass}, {"cpus", cpus}});
    QStringList named;
    for (int c : o.cpus) named << QString::number(c);
    note(o.pass ? tr("Passed: %1").arg(trial_->what)
                : tr("FAILED: %1%2").arg(trial_->what, named.isEmpty() ? QString() : tr(" (CPU %1)").arg(named.join(','))));
    search_->report(o);
    trial_.reset();
    applyTuning_->setEnabled(false);
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
    if (applying_ || sceneApplying_) { note(tr("Waiting for in-flight write before resetting.")); return; }
    reset(tr("Cancelled by user / guard"));
}

void CoSearchDialog::reset(const QString &reason) {
    if (!running_ || resetting_) return;
    resetting_ = true;
    applyTuning_->setEnabled(false);
    note(reason + tr(" — requesting stock reset."));
    privileged::run(privileged::helperPath("ryzen-co-helper"), QJsonObject{{"op", "reset"}}, this,
        [this, reason](const privileged::Result &r) { done(r.ok(), reason + (r.ok() ? QString() : QStringLiteral("; reset FAILED: ") + r.message())); }, 120000);
}

QString CoSearchDialog::saveProfile() const {
    // Same file format as Ryzen Curve → Save Profile, so the result loads there like any other profile.
    const QString name = QStringLiteral("CO search %1").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HHmm"));
    const QList<int> v = search_->values();
    QJsonArray cores;
    if (search_->mode() == Mode::PerCore)
        for (int i = 0; i < search_->cores().size(); ++i)
            cores.append(QJsonObject{{"ccd", search_->cores()[i].ccd}, {"ccx", 0}, {"slot", search_->cores()[i].slot},
                                     {"coper", v[i]}, {"disabled", false}});
    int ccds = 0;
    for (const Core &c : search_->cores()) ccds = std::max(ccds, c.ccd + 1);
    const QJsonObject profile{{"version", "2.0.0"}, {"ccd_count", ccds},
                              {"coall", search_->mode() == Mode::AllCore ? QJsonValue(v.value(0)) : QJsonValue()}, {"cores", cores}};
    QDir().mkpath(ryzen::profilesDir());
    QSaveFile f(ryzen::profilesDir() + '/' + name + ".json");
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(profile).toJson()) < 0 || !f.commit()) return {};
    return name;
}

void CoSearchDialog::done(bool resetOk, const QString &reason) {
    guard_->stop();
    // Persist a bounded evidence record. "passed" means only the tests run here, not curve stability.
    QDir().mkpath(stateDir());
    const QString path = stateDir() + "/co-search-runs.json";
    QFile prev(path);
    QJsonArray history;
    if (prev.open(QIODevice::ReadOnly) && prev.size() < (1LL << 20))
        history = QJsonDocument::fromJson(prev.readAll()).object().value("history").toArray();
    QJsonArray values;
    if (search_) for (int x : search_->values()) values << x;
    history.append(QJsonObject{{"t", double(QDateTime::currentSecsSinceEpoch())}, {"boot", bootId_},
                              {"mode", search_ && search_->mode() == Mode::PerCore ? "per" : "all"},
                              {"level", search_ ? name(search_->level()) : QString()}, {"values", values},
                              {"reset_ok", resetOk}, {"complete", finished_ && resetOk},
                              {"notes", reason}, {"attempts", attempts_}});
    while (history.size() > 40) history.removeAt(0);
    QSaveFile target(path);
    if (target.open(QIODevice::WriteOnly)) {
        target.write(QJsonDocument(QJsonObject{{"version", 2}, {"history", history}}).toJson());
        target.commit();
    }
    // A freeze or an application quit leaves the journal behind for a resume; a search that ended here does not.
    // Never erase evidence merely because the algorithm finished: an unsuccessful stock
    // reset can leave the last CO value applied and the recovery journal is still needed.
    if (!quitting_ && resetOk) QFile::remove(journalPath());
    running_ = false;
    resetting_ = false;
    sceneApplying_ = false;
    updateEstimate();
    trial_.reset();
    if (quitting_) return;
    go_->setEnabled(resetOk);
    for (QWidget *w : std::initializer_list<QWidget *>{mode_, level_, limit_, scene_}) w->setEnabled(true);
    stop_->setEnabled(false);
    refreshTable();
    note(reason);
    if (!resetOk) {
        QMessageBox::critical(this, tr("Reset failed"), tr("CPU curve reset could not be confirmed. Use Ryzen Curve → Reset, or reboot before continuing."));
        return;
    }
    if (finished_ && search_) {
        QStringList v;
        for (int x : search_->values()) v << QString::number(x);
        const QString profile = saveProfile();
        note(tr("SMU reset confirmed. Result: %1. %2").arg(v.join(' '),
             profile.isEmpty() ? tr("Saving the profile FAILED.")
                               : tr("Saved as Ryzen Curve profile '%1': load and apply it there. Many users keep 1-2 points of margin "
                                    "above a searched value for daily use; run long tests before relying on it.").arg(profile)));
    } else {
        note(tr("SMU reset confirmed. The search did not finish; nothing was saved."));
    }
    // The scene that was active before the search, if the search switched to its own.
    auto *win = parentWidget() ? qobject_cast<MainWindow *>(parentWidget()->window()) : nullptr;
    if (win && win->scenes() && scenes::activeScene() != priorScene_) {
        if (priorScene_.isEmpty()) { note(tr("No scene was active before the search: the CO search scene stays applied.")); return; }
        note(tr("Applying the previous scene '%1' again.").arg(priorScene_));
        win->scenes()->apply(priorScene_);
    }
}
