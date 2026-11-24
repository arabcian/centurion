#include "navigationtargets.h"
#include "inteltab.h"
#include "systemdiagnostics.h"
#include "diagreportview.h"
#include "profilediff.h"
#include "trialbar.h"
#include "privileged.h"
#include "theme.h"
#include "experimentalui.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QScrollArea>
#include <QTabWidget>
#include <QTimer>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QCoreApplication>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QHeaderView>
#include <QDateTime>
#include "charts.h"
#include <QTableWidget>
#include <cmath>
#include <algorithm>
#include <utility>
#include <QStandardPaths>
#include <QTime>
#include <QVBoxLayout>
#include <QVariant>

// Limits mirror intel_uv.rs; the helper re-checks everything.
static constexpr double UV_MIN = -300.0;  // GUI floor (helper accepts down to -1000)
static constexpr double ICC_MAX = 511.75;  // 11-bit field (not throttled's 10-bit 255.75)
static constexpr double UV_MAX_POSITIVE = 50.0;  // helper cap for allow_positive
static constexpr int MONITOR_MS = 2000;
static const char *BOOT_FILE = "/etc/centurion/intel-uv-boot.json";
static constexpr int PKEXEC_TIMEOUT_MS = 300000;

static QString helperPath() { return privileged::helperPath(QStringLiteral("intel-uv-helper")); }

QString inteluv::profilesDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
           QStringLiteral("/centurion/intel-uv-profiles");
}

static QGroupBox *box(const QString &title, const char *objName) {
    auto *b = new QGroupBox(title);
    b->setObjectName(QString::fromLatin1(objName));
    return b;
}
static QLabel *muted(const QString &t) {
    auto *l = new QLabel(t);
    l->setProperty("role", "muted");
    l->setWordWrap(true);
    return l;
}
static QDoubleSpinBox *dspin(double lo, double hi, double step, int dec, const QString &suffix) {
    auto *s = new QDoubleSpinBox;
    s->setRange(lo, hi);
    s->setSingleStep(step);
    s->setDecimals(dec);
    s->setSuffix(suffix);
    s->setMinimumWidth(std::max(104, s->sizeHint().width()));
    s->setAlignment(Qt::AlignRight);
    s->setEnabled(false);
    return s;
}

IntelTab::IntelTab(QWidget *parent) : QWidget(parent) {
    QDir().mkpath(inteluv::profilesDir());
    auto *root = new QVBoxLayout(this);
    trial_ = new TrialBar(QStringLiteral("intel/trial"));
    root->addWidget(trial_);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(6);

    // ── top: profile + global ──
    auto *top = new QHBoxLayout;
    auto *profBox = box("Profile", "box_purple");
    auto *pl = new QHBoxLayout(profBox);
    profileCombo_ = new QComboBox;
    profileCombo_->setMinimumWidth(160);
    auto *bLoad = new QPushButton("Load"), *bSave = new QPushButton("Save"), *bDel = new QPushButton("Delete");
    bDel->setObjectName("btnDanger");
    connect(bLoad, &QPushButton::clicked, this, [this] { loadProfile(profileCombo_->currentText()); });
    connect(bSave, &QPushButton::clicked, this, &IntelTab::saveProfile);
    connect(bDel, &QPushButton::clicked, this, &IntelTab::deleteProfile);
    pl->addWidget(profileCombo_, 1);
    pl->addWidget(bLoad);
    pl->addWidget(bSave);
    pl->addWidget(bDel);
    auto *compare = new QPushButton(tr("Compare…"));
    connect(compare, &QPushButton::clicked, this, [this] {
        profilediff::show(this, savedProfileNames(), [](const QString &name) { return profilediff::read(inteluv::profilesDir() + '/' + name + ".json"); });
    });
    pl->addWidget(compare);
    top->addWidget(profBox, 1);

    auto *gBox = box("Global", "box_grey");
    auto *gl = new QHBoxLayout(gBox);
    auto *bRead = new QPushButton("Read current");
    bRead->setToolTip("Read every value back from the CPU (MSR 0x150 mailbox, 0x1A2, 0x610, MCHBAR).");
    connect(bRead, &QPushButton::clicked, this, &IntelTab::readStatus);
    auto *bReset = new QPushButton("Reset voltages");
    bReset->setObjectName("btnDanger");
    bReset->setToolTip("Write 0 mV to all five voltage planes. IccMax, TCC offset and power limits are left alone\n"
                       "(their stock values are set by firmware and cannot be known).");
    connect(bReset, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, "Reset voltages", "Set every voltage offset back to 0 mV?",
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
            applyReset();
    });
    auto *bProbe = new QPushButton("Test UV lock");
    bProbe->setToolTip("Detect the BIOS undervolt lock the way it really behaves: move the CPU Core offset one\n"
                       "step (~1 mV, towards 0) through the OC mailbox, check the readback, restore it.\n"
                       "OC Lock (MSR 0x194 bit 20) is shown by Read current without writing anything.");
    connect(bProbe, &QPushButton::clicked, this, [this] {
        if (busy_) return;
        runOp({{"op", "probe_uv_lock"}}, [this](const QJsonObject &j) {
            uvLock_ = j.value("uv_locked").isBool() ? std::optional<bool>(j.value("uv_locked").toBool()) : std::nullopt;
            log(j.value("message").toString() + (j.value("restored").toBool(true) ? QString() : QStringLiteral(" — WARNING: original offset not restored")),
                uvLock_.value_or(false) ? "err" : "ok");
            readStatus();
        });
    });
    gl->addWidget(bRead);
    gl->addWidget(bProbe);
    gl->addWidget(bReset);
    top->addWidget(gBox);
    root->addLayout(top);

    info_ = new QLabel("Press <b>Read current</b> to query the CPU.");
    info_->setTextFormat(Qt::RichText);
    info_->setWordWrap(true);
    root->addWidget(info_);

    auto *monRow = new QHBoxLayout;
    monOn_ = new QCheckBox("Live monitor");
    monOn_->setToolTip("Every 2 s read the throttle reasons (IA32_THERM_STATUS), VCore\n"
                       "(IA32_PERF_STATUS) and RAPL energy counters. One pkexec call per sample, only while this tab is visible.");
    monLbl_ = new QLabel;
    monLbl_->setTextFormat(Qt::RichText);
    monRow->addWidget(monOn_);
    monRow->addWidget(monLbl_, 1);
    root->addLayout(monRow);
    monTimer_ = new QTimer(this);
    monTimer_->setInterval(MONITOR_MS);
    connect(monTimer_, &QTimer::timeout, this, &IntelTab::pollMonitor);
    connect(monOn_, &QCheckBox::toggled, this, [this](bool on) {
        monPrev_ = {};
        if (limRun_) limRun_->setText(on ? "Stop counting" : "Start counting");
        if (on && isVisible()) startMonitor(); else { stopMonitor(); monLbl_->clear(); }
    });

    auto *mid = new QHBoxLayout;
    mid->setSpacing(5);
    auto *left = new QVBoxLayout, *right = new QVBoxLayout;
    left->setSpacing(6);
    right->setSpacing(6);

    // ── voltage ──
    auto *vBox = box("Voltage offsets  (OC mailbox, MSR 0x150)", "box_yellow");
    auto *vg = new QGridLayout(vBox);
    vg->setVerticalSpacing(3);
    const char *heads[] = {"Plane", "Offset", "Current"};
    for (int c = 0; c < 3; ++c) {
        planeInfo_ << muted(QString::fromLatin1(heads[c]));
        vg->addWidget(planeInfo_.last(), 0, c);
    }
    const QList<QPair<QString, QString>> planes{{"core", "CPU Core"}, {"cache", "CPU Cache"}, {"gpu", "Integrated GPU"},
                                                {"uncore", "System Agent"}, {"analogio", "Analog I/O"},
                                                {"memss", "Memory Subsystem (MemSS)"}, {"ngu", "NGU fabric"}};
    for (int i = 0; i < planes.size(); ++i) {
        auto *on = new QCheckBox(planes[i].second);
        auto *sp = dspin(UV_MIN, 0.0, 1.0, 1, " mV");
        navigationtargets::mark(sp, QStringLiteral("intel.voltage.") + planes[i].first, planes[i].second, QStringLiteral("OC mailbox undervolt voltaj ofseti") + QLatin1Char(' ') + planes[i].first);
        auto *cur = new QLabel("–");
        cur->setMinimumWidth(80);
        connect(on, &QCheckBox::toggled, sp, &QWidget::setEnabled);
        on->setToolTip("Unchecked = this plane is not written.");
        vg->addWidget(on, i + 1, 0);
        vg->addWidget(sp, i + 1, 1);
        vg->addWidget(cur, i + 1, 2);
        volt_.append({planes[i].first, on, sp, cur});
        // Arrow Lake domains stay hidden until the CPU confirms them.
        if (i >= 5) for (QWidget *w : {static_cast<QWidget *>(on), static_cast<QWidget *>(sp), static_cast<QWidget *>(cur)}) w->hide();
    }
    link_ = new QCheckBox("Link Core && Cache");
    link_->setChecked(true);
    link_->setToolTip("Skylake and later: core and cache are one voltage plane. The SMALLER of the two offsets is\n"
                      "applied to both, so they should always be written with the same value.");
    auto sync = [this](int from) {
        if (!link_->isChecked()) return;
        const int to = from == 0 ? 1 : 0;
        QSignalBlocker b1(volt_[to].on), b2(volt_[to].val);
        volt_[to].on->setChecked(volt_[from].on->isChecked());
        volt_[to].val->setEnabled(volt_[from].on->isChecked());
        volt_[to].val->setValue(volt_[from].val->value());
    };
    for (int i : {0, 1}) {
        connect(volt_[i].on, &QCheckBox::toggled, this, [sync, i] { sync(i); });
        connect(volt_[i].val, &QDoubleSpinBox::valueChanged, this, [sync, i] { sync(i); });
    }
    vg->addWidget(link_, planes.size() + 1, 0, 1, 3);
    planeInfo_ << muted("Negative = undervolt. Start around −50 mV, test (Health → Curve stability → CPU test, idle, "
                        "suspend/resume), then step "
                        "down 10 mV at a time. Too far = freezes / MCEs; the offset is lost on reboot and S3, so a bad "
                        "value is recovered by a power cycle.");
    vg->addWidget(planeInfo_.last(), planes.size() + 2, 0, 1, 3);
    noPlanes_ = muted("No voltage plane has answered yet. Press Read current; on a CPU without Intel's OC mailbox "
                      "(any AMD CPU, or a BIOS that hides it) nothing can be offered here.");
    vg->addWidget(noPlanes_, planes.size() + 3, 0, 1, 3);
    vg->setColumnStretch(2, 1);
    auto *domain = new QPushButton(tr("Voltage Domains and CPU Topology…"));
    navigationtargets::mark(domain, QStringLiteral("intel.topology"), QStringLiteral("Voltage Domains and CPU Topology"), QStringLiteral("P E SMT voltaj alanları işlemci topolojisi"));
    connect(domain, &QPushButton::clicked, this, &IntelTab::domainOverview);
    vg->addWidget(domain, planes.size() + 4, 0, 1, 3);
    vg->setRowStretch(planes.size() + 5, 1);
    left->addWidget(vBox);

    // ── extras ──
    auto *xBox = box("Extras", "box_purple");
    auto *xg = new QGridLayout(xBox);
    xg->setVerticalSpacing(3);
    positive_ = new QCheckBox("Allow positive offsets (overvolt, max +50 mV)");
    positive_->setToolTip("Force mode: only for stabilising a marginal chip. Positive offsets raise voltage, heat and wear.");
    connect(positive_, &QCheckBox::toggled, this, &IntelTab::setPositive);
    xg->addWidget(positive_, 0, 0, 1, 2);
    xg->addWidget(new QLabel("BD PROCHOT"), 1, 0);
    bdprochot_ = new QComboBox;
    bdprochot_->addItems({"untouched", "disable", "enable"});
    bdprochot_->setToolTip("Disable_BDPROCHOT (MSR_POWER_CTL bit 0). Bi-directional PROCHOT lets the EC/charger\n"
                           "throttle the CPU to minimum clocks. Disabling it removes that safety net: only for machines that\n"
                           "throttle wrongly (bad charger/sensor). The EC may turn it back on; the daemon re-applies it.");
    xg->addWidget(bdprochot_, 1, 1);
    xg->addWidget(new QLabel("cTDP level"), 2, 0);
    ctdp_ = new QComboBox;
    ctdp_->addItems({"untouched", "0 · nominal", "1 · down", "2 · up"});
    ctdp_->setToolTip("cTDP (MSR_CONFIG_TDP_CONTROL). Only levels the CPU advertises in PLATFORM_INFO are accepted.");
    xg->addWidget(ctdp_, 2, 1);
    xg->addWidget(new QLabel("C1E"), 3, 0);
    c1e_ = new QComboBox;
    c1e_->addItems({"untouched", "disable", "enable"});
    c1e_->setToolTip("MSR_POWER_CTL bit 1 (ThrottleStop C1E). Disabled keeps idle clocks higher at more idle power;\n"
                     "keep it enabled on battery.");
    xg->addWidget(c1e_, 3, 1);
    cstOn_ = new QCheckBox("Package C-state limit");
    cst_ = new QSpinBox;
    cst_->setRange(0, 15);
    cst_->setEnabled(false);
    cstCur_ = new QLabel("–");
    connect(cstOn_, &QCheckBox::toggled, cst_, &QWidget::setEnabled);
    cstOn_->setToolTip("MSR_PKG_CST_CONFIG_CONTROL bits 3:0 on every CPU. Lower only (a shallower deepest package\n"
                       "C-state: less wake-up latency, more idle power); refused when the BIOS locked it (bit 15).\n"
                       "Only on CPUs whose encoding is verified (Haswell .. Arrow Lake).");
    auto *cstRow = new QHBoxLayout;
    cstRow->addWidget(cst_);
    cstRow->addWidget(cstCur_, 1);
    xg->addWidget(cstOn_, 4, 0);
    xg->addLayout(cstRow, 4, 1);
    lock_ = new QCheckBox("Lock PL1/PL2 until reboot");
    lock_->setToolTip("Lock power limit: sets bit 63 of MSR_PKG_POWER_LIMIT after writing, so neither\n"
                      "firmware nor software (including this tool) can change it until the next reset.");
    xg->addWidget(lock_, 5, 0, 1, 2);
    auto *bTs = new QPushButton("Import ThrottleStop.ini…");
    bTs->setToolTip("Read FIVRVoltage offsets from a ThrottleStop profile.");
    connect(bTs, &QPushButton::clicked, this, &IntelTab::importThrottleStop);
    xg->addWidget(bTs, 6, 0, 1, 2);
    xg->setColumnStretch(1, 1);
    xg->setRowStretch(7, 1);  // rows stay together at the top when the box is as tall as its neighbour
    left->addWidget(xBox);

    // ── limit reasons ──
    auto *rBox = box("Limit reasons  (MSR 0x64F / 0x6B0 / 0x6B1)", "box_blue");
    navigationtargets::mark(rBox, QStringLiteral("intel.reasons"), QStringLiteral("Limit reasons"), QStringLiteral("throttle kısıtlama nedenleri"));
    auto *rg = new QGridLayout(rBox);
    rg->setVerticalSpacing(2);
    rg->setHorizontalSpacing(14);
    rg->addWidget(muted("Reason"), 0, 0);
    const char *doms[] = {"core", "gpu", "ring"}, *domLabels[] = {"Core", "iGPU", "Ring"};
    for (int d = 0; d < 3; ++d) rg->addWidget(muted(QString::fromLatin1(domLabels[d])), 0, d + 1, Qt::AlignRight);
    // (bit, label, domains mask core|gpu|ring, tooltip) — SDM client definitions; a
    // domain without that bit shows "–".
    struct Reason { int bit; const char *label; int mask; const char *tip; };
    static const Reason reasons[] = {
        {0, "PROCHOT", 7, "External PROCHOT# (EC, charger, VRM) forced the minimum frequency."},
        {1, "Thermal", 7, "Die temperature reached the TCC target (TjMax − TCC offset)."},
        {5, "Avg thermal (RATL)", 7, "Running-average thermal limit."},
        {6, "VR thermal alert", 7, "Voltage regulator over-temperature."},
        {7, "VR TDC", 7, "VR thermal design current limit."},
        {8, "EDP / IccMax", 7, "Electrical design point: the IccMax current limit (or other electrical limit) was hit."},
        {10, "PL1", 7, "Package long-term power limit."},
        {11, "PL2", 7, "Package short-term power limit."},
        {12, "Max turbo / inefficient", 3, "Core: multi-core turbo ratio limit. iGPU: inefficient-operation limit."},
        {13, "Turbo attenuation", 1, "Turbo transition attenuation (frequent turbo changes damped)."},
        {4, "Residency regulation", 1, "Residency state regulation (C-state based)."},
    };
    int rr = 1;
    for (const Reason &r : reasons) {
        auto *name = new QLabel(QString::fromLatin1(r.label));
        name->setToolTip(QString::fromLatin1(r.tip));
        rg->addWidget(name, rr, 0);
        for (int d = 0; d < 3; ++d) {
            auto *c = new QLabel(r.mask & (1 << d) ? QStringLiteral("·") : QStringLiteral("–"));
            c->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            c->setMinimumWidth(52);
            c->setTextFormat(Qt::RichText);
            rg->addWidget(c, rr, d + 1);
            if (r.mask & (1 << d)) limCells_.insert(QStringLiteral("%1:%2").arg(QString::fromLatin1(doms[d])).arg(r.bit), c);
        }
        ++rr;
    }
    limInfo_ = muted("Counts samples (every 2 s) in which each reason limited the frequency. "
                     "Red = limiting now, amber = happened (sticky log bit or counted).");
    rg->addWidget(limInfo_, rr++, 0, 1, 4);
    auto *lr2 = new QHBoxLayout;
    limRun_ = new QPushButton("Start counting");
    connect(limRun_, &QPushButton::clicked, this, [this] { monOn_->setChecked(!monOn_->isChecked()); });
    auto *bLimReset = new QPushButton("Reset");
    bLimReset->setToolTip("Zero the counters and clear the CPU's sticky log bits.");
    connect(bLimReset, &QPushButton::clicked, this, &IntelTab::resetLimits);
    lr2->addWidget(limRun_);
    lr2->addWidget(bLimReset);
    lr2->addStretch();
    rg->addLayout(lr2, rr++, 0, 1, 4);
    cpuTable_ = new QTableWidget(0, 5);
    cpuTable_->setHorizontalHeaderLabels({"CPU", "MHz", "C0 %", "°C", "Max °C"});
    cpuTable_->verticalHeader()->hide();
    cpuTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    cpuTable_->setSelectionMode(QAbstractItemView::NoSelection);
    cpuTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    cpuTable_->setMinimumHeight(140);
    cpuTable_->setToolTip("Live monitor, per CPU: MHz = base × ΔAPERF/ΔMPERF, C0 % = ΔMPERF/ΔTSC (busy share),\n"
                          "°C = TjMax − DTS reading of that core (shared by its sibling threads).");
    rg->addWidget(cpuTable_, rr++, 0, 1, 4);
    auto *bMax = new QPushButton("Clear max");
    connect(bMax, &QPushButton::clicked, this, [this] { maxTemp_.clear(); });
    rg->addWidget(bMax, rr, 0);
    trendSel_ = new QComboBox;
    trendSel_->addItems({"Package W", "Package °C", "VCore mV"});
    connect(trendSel_, &QComboBox::currentIndexChanged, this, &IntelTab::drawTrend);
    rg->addWidget(trendSel_, rr++, 1, 1, 3);
    trend_ = new LineChart;
    trend_->setMinimumHeight(110);
    trend_->setToolTip("Last 5 minutes of the live monitor.");
    rg->addWidget(trend_, rr++, 0, 1, 4);
    csvOn_ = new QCheckBox("Log to CSV…");
    csvOn_->setToolTip("Append every live-monitor sample to a CSV file you choose (time, package/core W, package °C,\n"
                       "VCore, throttle flags). Pauses while this tab is hidden (no samples then); stops when unticked.\n"
                       "An existing file is appended to only if it is such a log.");
    connect(csvOn_, &QCheckBox::toggled, this, [this](bool on) {
        if (csv_) { csv_->close(); delete csv_; csv_ = nullptr; }
        if (!on) return;
        const QString path = QFileDialog::getSaveFileName(this, "CSV log", QDir::homePath() + "/centurion-intel-monitor.csv", "CSV (*.csv)");
        auto *f = new QFile(path);
        if (path.isEmpty() || !f->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            delete f;
            QSignalBlocker b(csvOn_);
            csvOn_->setChecked(false);
            if (!path.isEmpty()) log("Cannot open " + path, "err");
            return;
        }
        static const QByteArray head = "time,package_w,core_w,package_c,vcore_mv,thermal,power,current\n";
        if (f->size() > 0) {  // appending under another header would give a file with mixed columns
            QFile r(path);
            if (!r.open(QIODevice::ReadOnly) || r.readLine() != head) {
                delete f;
                QSignalBlocker b(csvOn_);
                csvOn_->setChecked(false);
                log(path + " is not a log of this format (different first line) — choose a new file", "err");
                return;
            }
        }
        csv_ = f;
        if (csv_->size() == 0) csv_->write(head);
        log("Logging live-monitor samples to " + path + (monOn_->isChecked() ? QString() : " (tick Live monitor to start sampling)"), "ok");
    });
    rg->addWidget(csvOn_, rr, 0, 1, 4);
    rg->setColumnStretch(0, 1);
    left->addWidget(rBox);
    left->addStretch();

    // ── limits ──
    auto *lBox = box("Limits", "box_blue");
    navigationtargets::mark(lBox, QStringLiteral("intel.limits"), QStringLiteral("Limits"), QStringLiteral("PL1 PL2 ICCmax RAPL güç akım limitleri"));
    auto *lg = new QGridLayout(lBox);
    lg->setVerticalSpacing(3);
    int r = 0;
    lg->addWidget(muted("IccMax (A)"), r++, 0, 1, 4);
    for (const auto &[k, label] : QList<QPair<QString, QString>>{{"core", "Core"}, {"cache", "Cache"}, {"gpu", "iGPU"}}) {
        auto *on = new QCheckBox(label);
        auto *sp = dspin(1.0, ICC_MAX, 1.0, 2, " A");
        navigationtargets::mark(sp, QStringLiteral("intel.iccmax.") + k, QStringLiteral("IccMax / ") + label, QStringLiteral("VR current akım limiti"));
        sp->setValue(100.0);
        auto *cur = new QLabel("–");
        connect(on, &QCheckBox::toggled, sp, &QWidget::setEnabled);
        on->setToolTip("Maximum current of this VR domain (0.25 A steps, floored, up to 511.75 A). Raising it can stop\n"
                       "current-limit throttling; the stock value on the right gives it back.\n"
                       "Supported by fewer CPUs than voltage offsets.");
        lg->addWidget(on, r, 0);
        lg->addWidget(sp, r, 1);
        lg->addWidget(cur, r++, 2, 1, 2);
        icc_.append({k, on, sp, cur});
    }
    lg->addWidget(muted("Thermal"), r++, 0, 1, 4);
    tjOn_ = new QCheckBox("TCC offset");
    tj_ = new QSpinBox;
    navigationtargets::mark(tj_, QStringLiteral("intel.tcc"), QStringLiteral("TCC offset"), QStringLiteral("TjMax thermal sıcaklık ofseti"));
    tj_->setRange(0, 63);
    tj_->setSuffix(" °C");
    tj_->setMinimumWidth(std::max(104, tj_->sizeHint().width()));
    tj_->setAlignment(Qt::AlignRight);
    tj_->setEnabled(false);
    tjCur_ = new QLabel("–");
    connect(tjOn_, &QCheckBox::toggled, tj_, &QWidget::setEnabled);
    tjOn_->setToolTip("MSR 0x1A2 bits 29:24: throttle at TjMax − offset (read-modify-write, target kept ≥ 40 °C).\n"
                      "Same register as Optimizations → CPU → TCC offset (intel_tcc_cooling).");
    lg->addWidget(tjOn_, r, 0);
    lg->addWidget(tj_, r, 1);
    lg->addWidget(tjCur_, r++, 2, 1, 2);
    turboOn_ = new QCheckBox("Max turbo ratio");
    turbo_ = new QSpinBox;
    turbo_->setRange(1, 255);  // base..stock once the status is read
    turbo_->setPrefix("× ");
    turbo_->setMinimumWidth(std::max(104, turbo_->sizeHint().width()));
    turbo_->setAlignment(Qt::AlignRight);
    turbo_->setEnabled(false);
    turboCur_ = new QLabel("–");
    connect(turboOn_, &QCheckBox::toggled, turbo_, &QWidget::setEnabled);
    turboOn_->setToolTip("MSR 0x1AD: caps every turbo ratio (× 100 MHz) — less heat and fan noise, lower peak clocks.\n"
                         "Lower only: never above the stock ratios; the maximum gives stock back. P-cores only on hybrid\n"
                         "CPUs. Needs an unlocked multiplier (PLATFORM_INFO bit 28); lost on reboot.");
    lg->addWidget(turboOn_, r, 0);
    lg->addWidget(turbo_, r, 1);
    lg->addWidget(turboCur_, r++, 2, 1, 2);
    groupsOn_ = new QCheckBox("Turbo groups");
    groupsOn_->setToolTip("MSR 0x1AD per group (ThrottleStop turbo ratio table): one ratio per active-core group, group 1\n"
                          "first. Each stays between the base ratio and stock, and the table must not rise from one group to\n"
                          "the next. Replaces 'Max turbo ratio' (both write the same register).");
    auto *grow = new QHBoxLayout;
    for (int i = 0; i < 8; ++i) {
        auto *sp = new QSpinBox;
        sp->setRange(1, 255);
        sp->setEnabled(false);
        sp->hide();  // shown for the groups the CPU has, once read
        sp->setToolTip(QStringLiteral("group %1").arg(i + 1));
        connect(groupsOn_, &QCheckBox::toggled, sp, &QWidget::setEnabled);
        grow->addWidget(sp);
        groups_ << sp;
    }
    grow->addStretch();
    groupsCur_ = muted("Read current to show the CPU's groups.");
    connect(groupsOn_, &QCheckBox::toggled, this, [this](bool on) { if (on) turboOn_->setChecked(false); });
    connect(turboOn_, &QCheckBox::toggled, this, [this](bool on) { if (on) groupsOn_->setChecked(false); });
    lg->addWidget(groupsOn_, r, 0);
    lg->addLayout(grow, r++, 1, 1, 3);
    lg->addWidget(groupsCur_, r++, 1, 1, 3);

    lg->addWidget(muted("Package power (MSR 0x610)"), r++, 0, 1, 4);
    auto mkPl = [&](const QString &label, double w, double s, const QString &tip) {
        PlRow p{new QCheckBox(label), dspin(1, 250, 1, 0, " W"), dspin(0.001, 1000, 0.5, 3, " s"), new QLabel("–")};
        navigationtargets::mark(p.w, QStringLiteral("intel.power.") + label, label, QStringLiteral("RAPL MSR 0x610 watt güç limiti"));
        navigationtargets::mark(p.s, QStringLiteral("intel.time.") + label, label + QStringLiteral(" time window"), QStringLiteral("tau süre zaman penceresi"));
        p.w->setValue(w);
        p.s->setValue(s);
        p.s->setMinimumWidth(std::max(96, p.s->sizeHint().width()));
        p.on->setToolTip(tip);
        connect(p.on, &QCheckBox::toggled, p.w, &QWidget::setEnabled);
        connect(p.on, &QCheckBox::toggled, p.s, &QWidget::setEnabled);
        lg->addWidget(p.on, r, 0);
        lg->addWidget(p.w, r, 1);
        lg->addWidget(p.s, r, 2);
        lg->addWidget(p.cur, r++, 3);
        return p;
    };
    pl1_ = mkPl("PL1", 55, 28, "Long-term package power limit and its time window (nearest encodable window is used).");
    pl2_ = mkPl("PL2", 90, 0.002, "Short-term package power limit and its time window.");
    mchbar_ = new QCheckBox("Mirror to MCHBAR (MMIO)");
    mchbar_->setChecked(true);
    mchbar_->setToolTip("Write the same value to MCHBAR+0x59A0 (via /dev/mem) as well:\n"
                        "the effective limit is the lower of the MSR and MMIO copies. Skipped automatically if the\n"
                        "host bridge is unknown or /dev/mem is blocked.");
    lg->addWidget(mchbar_, r++, 0, 1, 4);
    auto *clampRow = new QHBoxLayout;
    clamp1_ = new QCheckBox("PL1 clamp");
    clamp2_ = new QCheckBox("PL2 clamp");
    for (QCheckBox *c : {clamp1_, clamp2_}) {
        c->setTristate(true);
        c->setCheckState(Qt::PartiallyChecked);
        c->setToolTip("MSR 0x610 clamp bit (ThrottleStop TPL 'Clamp'): allow throttling below the OS-requested\n"
                      "P-state to hold the limit. Partially checked = untouched. Written together with that PL row.");
        clampRow->addWidget(c);
    }
    clampRow->addStretch();
    lg->addLayout(clampRow, r++, 0, 1, 4);
    auto mkW = [&](QCheckBox *&on, QDoubleSpinBox *&sp, QLabel *&cur, const QString &label, const QString &tip) {
        on = new QCheckBox(label);
        sp = dspin(1, 250, 1, 0, " W");
        sp->setEnabled(false);
        cur = new QLabel("–");
        on->setToolTip(tip);
        connect(on, &QCheckBox::toggled, sp, &QWidget::setEnabled);
        lg->addWidget(on, r, 0);
        lg->addWidget(sp, r, 1);
        lg->addWidget(cur, r++, 2, 1, 2);
    };
    mkW(pl4On_, pl4_, pl4Cur_, "PL4", "Peak power limit through the kernel's powercap 'peak_power' constraint (MSR 0x601).\n"
        "Lower only: it protects the adapter and battery from current spikes, so a value above this boot's stock\n"
        "PL4 is refused.");
    mkW(pp0On_, pp0_, pp0Cur_, "PP0 (cores)", "Core-domain long-term limit through powercap intel-rapl:0:0 (MSR 0x638),\n"
        "enabled when written. A cap: it can only lower power. Some newer CPUs ignore it.");
    mmioOff_ = new QCheckBox("Disable MCHBAR (MMIO) limits");
    mmioOff_->setToolTip("Clears the enable bits of the MCHBAR copy of PL1/PL2, so only the MSR limits apply (ThrottleStop\n"
                         "'MMIO Lock' clears them by writing 0; only the two enable bits are touched here). Refused when the\n"
                         "copy is locked. Not together with mirroring.");
    connect(mmioOff_, &QCheckBox::toggled, this, [this](bool on) { if (on) mchbar_->setChecked(false); });
    connect(mchbar_, &QCheckBox::toggled, this, [this](bool on) { if (on) mmioOff_->setChecked(false); });
    lg->addWidget(mmioOff_, r++, 0, 1, 4);
    plLock_ = muted("On a Legion the EC re-programs PL1/PL2 on every platform-profile change — prefer the Firmware "
                    "Attributes tab (Custom) for lasting limits.");
    lg->addWidget(plLock_, r++, 0, 1, 4);

    lg->addWidget(muted("Frequency (kernel drivers)"), r++, 0, 1, 4);
    auto mkRange = [&](QCheckBox *&on, QSpinBox *&lo, QSpinBox *&hi, QLabel *&cur, const QString &label,
                       int min, int max, const QString &suffix, const QString &tip) {
        on = new QCheckBox(label);
        on->setToolTip(tip);
        lo = new QSpinBox; hi = new QSpinBox;
        for (QSpinBox *s : {lo, hi}) { s->setRange(min, max); s->setSuffix(suffix); s->setEnabled(false); connect(on, &QCheckBox::toggled, s, &QWidget::setEnabled); }
        hi->setValue(max);
        cur = new QLabel("–");
        lg->addWidget(on, r, 0);
        lg->addWidget(lo, r, 1);
        lg->addWidget(hi, r, 2);
        lg->addWidget(cur, r++, 3);
    };
    mkRange(perfOn_, perfMin_, perfMax_, perfCur_, "Speed Shift min–max", 1, 100, " %",
            "intel_pstate min_perf_pct / max_perf_pct (the kernel turns them into the HWP min/max ratio —\n"
            "ThrottleStop's Speed Shift Min/Max), as % of the highest turbo P-state.");
    mkRange(uncOn_, uncMin_, uncMax_, uncCur_, "Ring / uncore", 100, 10000, " MHz",
            "intel_uncore_frequency min/max for every uncore domain (MSR 0x620), kept inside each domain's stock range.\n"
            "100 MHz steps (the driver stores a ratio).");
    uncMin_->setSingleStep(100);
    uncMax_->setSingleStep(100);
    QStringList eppList{"performance", "balance_performance", "default", "balance_power", "power"};
    if (auto t = QFile("/sys/devices/system/cpu/cpufreq/policy0/energy_performance_available_preferences"); t.open(QIODevice::ReadOnly)) {
        const QStringList a = QString::fromLatin1(t.readAll()).split(' ', Qt::SkipEmptyParts);
        if (!a.isEmpty()) { eppList = a; for (QString &x : eppList) x = x.trimmed(); }
    }
    epp_ = new QComboBox;
    epp_->addItem("untouched");
    epp_->addItems(eppList);
    epp_->setToolTip("energy_performance_preference on every policy (ThrottleStop Speed Shift EPP). Not together with\n"
                     "EPP switching (hwphint), which writes the same files.");
    lg->addWidget(new QLabel("EPP"), r, 0);
    lg->addWidget(epp_, r++, 1, 1, 3);
    lg->setColumnStretch(3, 1);
    lg->setRowStretch(r, 1);
    right->addWidget(lBox);

    // ── boot / daemon ──
    auto *dBox = box("Boot, resume && daemon", "box_green");
    navigationtargets::mark(dBox, QStringLiteral("intel.boot"), QStringLiteral("Boot resume daemon"), QStringLiteral("açılış devam servis"));
    auto *dg = new QGridLayout(dBox);
    dg->setVerticalSpacing(3);
    dg->addWidget(new QLabel("Store for"), 0, 0);
    bootTarget_ = new QComboBox;
    bootTarget_->addItems({"AC and battery", "AC only", "Battery only"});
    bootTarget_->setToolTip("[AC]/[BATTERY]: separate profiles per power source. The other slot in the\n"
                            "stored file is kept; the daemon switches when the charger is plugged or unplugged.");
    dg->addWidget(bootTarget_, 0, 1, 1, 3);
    reapply_ = new QCheckBox("Re-apply every");
    reapply_->setChecked(true);
    reapply_->setToolTip("Daemon only (Update_Rate_s): re-write power limits (PL1/PL2,\n"
                         "PL4, PP0, MMIO disable), TCC, BD PROCHOT, cTDP, C1E, Speed Shift %, EPP and uncore each interval —\n"
                         "the EC restores its own PL1/PL2 on profile changes. Values set here therefore win over the same\n"
                         "settings in Optimizations (EPP, perf %, uncore) within one interval.\n"
                         "Voltages, turbo ratios and the C-state limit are written on start, power-source change and resume.");
    interval_ = new QSpinBox;
    interval_->setRange(500, 600000);
    interval_->setSingleStep(500);
    interval_->setValue(5000);
    interval_->setSuffix(" ms");
    dg->addWidget(reapply_, 1, 0);
    dg->addWidget(interval_, 1, 1);
    hwpOn_ = new QCheckBox("EPP switching (hwphint)");
    hwpOn_->setToolTip("hwphint, run by the daemon: switch energy_performance_preference between two\n"
                       "hints by CPU load or RAPL power. 'switch' only touches CPUs whose EPP is already one of the two\n"
                       "(so Optimizations settings are respected); 'force' always writes.");
    dg->addWidget(hwpOn_, 2, 0, 1, 4);
    hwpMode_ = new QComboBox; hwpMode_->addItems({"switch", "force"});
    hwpAlgo_ = new QComboBox; hwpAlgo_->addItems({"load", "power"});
    hwpMulti_ = new QCheckBox("all cores");
    hwpMulti_->setToolTip("load: unchecked = busiest single CPU, checked = average over all CPUs.");
    hwpThreshold_ = dspin(0.05, 1.0, 0.05, 2, "");
    hwpThreshold_->setValue(0.8);
    hwpDomain_ = new QComboBox; hwpDomain_->addItems({"package", "core", "uncore", "dram", "psys"});
    hwpCmp_ = new QComboBox; hwpCmp_->addItems({">", "<"});
    hwpWatts_ = dspin(0, 1000, 1, 1, " W");
    hwpWatts_->setValue(8);
    const QStringList &epp = eppList;
    hwpLoadHint_ = new QComboBox; hwpLoadHint_->addItems(epp);
    hwpNormalHint_ = new QComboBox; hwpNormalHint_->addItems(epp);
    hwpLoadHint_->setCurrentText("performance");
    hwpNormalHint_->setCurrentText("balance_performance");
    dg->addWidget(new QLabel("mode"), 3, 0); dg->addWidget(hwpMode_, 3, 1);
    dg->addWidget(new QLabel("by"), 3, 2); dg->addWidget(hwpAlgo_, 3, 3);
    auto *loadRow = new QWidget; auto *lr = new QHBoxLayout(loadRow); lr->setContentsMargins(0, 0, 0, 0);
    lr->addWidget(new QLabel("load ≥")); lr->addWidget(hwpThreshold_); lr->addWidget(hwpMulti_); lr->addStretch();
    auto *powRow = new QWidget; auto *pr = new QHBoxLayout(powRow); pr->setContentsMargins(0, 0, 0, 0);
    pr->addWidget(hwpDomain_); pr->addWidget(hwpCmp_); pr->addWidget(hwpWatts_); pr->addStretch();
    dg->addWidget(loadRow, 4, 0, 1, 4);
    dg->addWidget(powRow, 5, 0, 1, 4);
    dg->addWidget(new QLabel("busy →"), 6, 0); dg->addWidget(hwpLoadHint_, 6, 1, 1, 3);
    dg->addWidget(new QLabel("idle →"), 7, 0); dg->addWidget(hwpNormalHint_, 7, 1, 1, 3);
    auto hwpSync = [this, loadRow, powRow] {
        const bool on = hwpOn_->isChecked(), load = hwpAlgo_->currentIndex() == 0;
        for (QWidget *w : std::initializer_list<QWidget *>{hwpMode_, hwpAlgo_, hwpLoadHint_, hwpNormalHint_}) w->setEnabled(on);
        loadRow->setVisible(load); powRow->setVisible(!load);
        loadRow->setEnabled(on); powRow->setEnabled(on);
        hwpThreshold_->setEnabled(on); hwpWatts_->setEnabled(on);
    };
    connect(hwpOn_, &QCheckBox::toggled, this, hwpSync);
    connect(hwpAlgo_, &QComboBox::currentIndexChanged, this, hwpSync);
    hwpSync();
    // A fixed EPP and EPP switching write the same files (the helper refuses both).
    connect(hwpOn_, &QCheckBox::toggled, this, [this](bool on) { if (on) epp_->setCurrentIndex(0); });
    connect(epp_, &QComboBox::currentIndexChanged, this, [this](int i) { if (i > 0) hwpOn_->setChecked(false); });
    alarmOn_ = new QCheckBox("Temperature alarm at");
    alarmOn_->setToolTip("Daemon only (ThrottleStop failsafe alarm): when the package reaches this temperature, PL1/PL2 are\n"
                         "lowered to the values on the right — never raised: if the live limit is already lower it stays.\n"
                         "The limits from before the alarm are written back once the package is 10 °C cooler.");
    alarmTemp_ = new QSpinBox;
    alarmTemp_->setRange(60, 105);
    alarmTemp_->setValue(95);
    alarmTemp_->setSuffix(" °C");
    alarmPl1_ = dspin(1, 250, 1, 0, " W");
    alarmPl1_->setValue(25);
    alarmPl2_ = dspin(1, 250, 1, 0, " W");
    alarmPl2_->setValue(35);
    for (QWidget *w : std::initializer_list<QWidget *>{alarmTemp_, alarmPl1_, alarmPl2_}) {
        w->setEnabled(false);
        connect(alarmOn_, &QCheckBox::toggled, w, &QWidget::setEnabled);
    }
    auto *alarmRow = new QHBoxLayout;
    alarmRow->addWidget(alarmTemp_);
    alarmRow->addWidget(new QLabel("→ PL1"));
    alarmRow->addWidget(alarmPl1_);
    alarmRow->addWidget(new QLabel("PL2"));
    alarmRow->addWidget(alarmPl2_);
    alarmRow->addStretch();
    dg->addWidget(alarmOn_, 8, 0);
    dg->addLayout(alarmRow, 8, 1, 1, 3);
    dg->addWidget(muted("Periodic re-apply, EPP switching and the alarm need the daemon: rc-update add centurion-intel-uv-daemon default "
                        "(systemd: centurion-intel-uv-daemon.service). Without it, centurion-intel-uv applies once at boot and after resume."), 9, 0, 1, 4);
    dg->setColumnStretch(3, 1);
    dg->setRowStretch(10, 1);
    right->addWidget(dBox);
    right->addStretch();

    if (theme::experimentalEnabled()) {
        // Each task gets its own complete workspace. All controls are the
        // original widgets; only their parent layout changes in Studio mode.
        for (QWidget *w : {static_cast<QWidget *>(vBox), static_cast<QWidget *>(xBox),
                           static_cast<QWidget *>(rBox), static_cast<QWidget *>(lBox),
                           static_cast<QWidget *>(dBox)}) {
            left->removeWidget(w);
            right->removeWidget(w);
        }
        delete left;
        delete right;
        delete mid;
        auto *work = new QTabWidget;
        work->setObjectName(QStringLiteral("intelWorkflows"));
        work->setDocumentMode(true);
        // Two equal columns; boxes on one row share its height (a grid cell stretches its widget), so a box
        // that gains rows with the hardware (Arrow Lake planes, turbo groups) grows its neighbour with it.
        // A row taller than the window scrolls with the page.
        auto makePage = [work](const QString &label, const QList<QList<QWidget *>> &rows) {
            auto *scroll = new QScrollArea;
            scroll->setFrameShape(QFrame::NoFrame);
            scroll->setWidgetResizable(true);
            auto *page = new QWidget;
            auto *grid = new QGridLayout(page);
            grid->setContentsMargins(5, 7, 5, 7);
            grid->setHorizontalSpacing(8);
            grid->setVerticalSpacing(8);
            grid->setColumnStretch(0, 1);
            grid->setColumnStretch(1, 1);
            int r = 0;
            for (const QList<QWidget *> &row : rows) {
                for (QWidget *w : row) w->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);  // widths from the grid only
                if (row.size() == 1) grid->addWidget(row[0], r, 0, 1, 2);
                else { grid->addWidget(row[0], r, 0); grid->addWidget(row[1], r, 1); }
                grid->setRowStretch(r, 1);  // the rows fill the page: every tab's boxes end on the same line
                ++r;
            }
            scroll->setWidget(page);
            work->addTab(scroll, label);
        };
        makePage(tr("Voltage, safety && startup"), {{vBox, xBox}, {dBox}});
        makePage(tr("Limits && throttling"), {{lBox, rBox}});
        root->addWidget(work, 1);
    } else {
        mid->addLayout(left, 1);
        mid->addLayout(right, 1);
        auto *midHost = new QWidget;
        midHost->setObjectName("intelMid");
        midHost->setStyleSheet("#intelMid { background: transparent; }");
        midHost->setLayout(mid);
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(midHost);
        root->addWidget(scroll, 1);
    }

    // ── actions ──
    auto *aBox = box("Apply", "box_green");
    auto *al = new QHBoxLayout(aBox);
    auto *bApply = new QPushButton("Apply");
    bApply->setObjectName("btnAccent");
    bApply->setFixedWidth(132);
    connect(bApply, &QPushButton::clicked, this, [this] {
        const QJsonObject p = currentProfile();
        if (validate(p)) runOp({{"op", "apply"}, {"profile", p}}, [this](const QJsonObject &) { readStatus(); });
    });
    auto *bBoot = new QPushButton("⏻ Apply at boot && resume");
    bBoot->setToolTip("Store these values as /etc/centurion/intel-uv-boot.json. The centurion-intel-uv service\n"
                      "applies it at boot, and the elogind / systemd sleep hook re-applies it after resume\n"
                      "(offsets are lost on S3 suspend). Test the values with Apply first.");
    connect(bBoot, &QPushButton::clicked, this, &IntelTab::saveBoot);
    auto *bClear = new QPushButton("Clear boot");
    connect(bClear, &QPushButton::clicked, this, [this] { runOp({{"op", "clear_boot"}}); });
    bootLbl_ = muted("");
    al->addWidget(bootLbl_, 1);
    al->addWidget(bClear);
    al->addWidget(bBoot);
    al->addWidget(bApply);
    root->addWidget(aBox);
    buttons_ = {bRead, bReset, bApply, bBoot, bClear, bTs};

    auto *logBox = box("Output / Log", "box_grey");
    auto *ll = new QVBoxLayout(logBox);
    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setObjectName("terminal");
    log_->setMaximumBlockCount(2000);
    log_->setMinimumHeight(54);
    log_->setMaximumHeight(90);
    ll->addWidget(log_);
    logBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    aBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    if (theme::experimentalEnabled()) {
        root->addWidget(experimentalui::disclosure(tr("ACTIVITY LOG  ·  MSR operations"), logBox));
    }
    root->addWidget(logBox, 0);

    // Their ranges and starting values come from the CPU (PL4/PP0 would otherwise start at 1 W, the
    // C-state limit at 0): tickable once Read current has filled them (showStatus enables them).
    for (QCheckBox *c : {pl4On_, pp0On_, perfOn_, uncOn_, cstOn_}) c->setEnabled(false);
    // Daemon settings of the stored boot file (world-readable): saveBoot writes them back from these
    // widgets, so they must start from the stored values, not the defaults.
    if (QFile f(QString::fromLatin1(BOOT_FILE)); f.open(QIODevice::ReadOnly)) {
        const QJsonObject d = QJsonDocument::fromJson(f.readAll()).object().value("daemon").toObject();
        if (d.value("interval_ms").isDouble()) interval_->setValue(d.value("interval_ms").toInt());
        if (d.value("reapply").isBool()) reapply_->setChecked(d.value("reapply").toBool());
        const QJsonObject a = d.value("alarm").toObject();
        alarmOn_->setChecked(!a.isEmpty());
        if (!a.isEmpty()) {
            alarmTemp_->setValue(a.value("temp_c").toInt());
            alarmPl1_->setValue(a.value("pl1_w").toDouble());
            alarmPl2_->setValue(a.value("pl2_w").toDouble());
        }
    }
    reloadProfiles();
    log("Intel CPU detected. Nothing is written until you press Apply.");
}

void IntelTab::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    // First visit reads the CPU once (one pkexec round trip; silent for wheel via the polkit rule).
    if (!readOnce_ && !qEnvironmentVariableIsSet("CENTURION_PGO_TRAIN")) { readOnce_ = true; readStatus(); }
    if (monOn_->isChecked()) startMonitor();
}

void IntelTab::hideEvent(QHideEvent *e) {
    QWidget::hideEvent(e);
    stopMonitor();  // no root sampling while the tab (or window) is hidden
    monPrev_ = {};
}

void IntelTab::log(const QString &msg, const QString &level) {
    static const QHash<QString, QPair<QString, QString>> style{
        {"info", {theme::FG_DIM, "  "}}, {"ok", {theme::OK, "OK"}}, {"err", {theme::DANGER, "!!"}}, {"cmd", {theme::WARN, ">>"}}};
    const auto [color, prefix] = style.value(level, style.value("info"));
    log_->appendHtml(QStringLiteral("<span style=\"color:%1;\">[%2]</span> <span style=\"color:%3;\">%4 %5</span>")
        .arg(theme::MUTED, QTime::currentTime().toString("HH:mm:ss"), color, prefix,
             msg.toHtmlEscaped().replace('\n', "<br>")));
}

// ── profile <-> widgets ─────────────────────────────────────────────────────

QJsonObject IntelTab::currentProfile() const {
    QJsonObject v, i;
    for (const Row &r : volt_)
        if (!r.on->isHidden()) v[r.key] = r.on->isChecked() ? QJsonValue(r.val->value()) : QJsonValue();
    for (const Row &r : icc_) i[r.key] = r.on->isChecked() ? QJsonValue(r.val->value()) : QJsonValue();
    auto pl = [](const PlRow &p, const QCheckBox *clamp) {
        if (!p.on->isChecked()) return QJsonValue();
        QJsonObject o{{"watts", p.w->value()}, {"seconds", p.s->value()}};
        if (clamp->checkState() != Qt::PartiallyChecked) o["clamp"] = clamp->isChecked();
        return QJsonValue(o);
    };
    const int bd = bdprochot_->currentIndex(), ct = ctdp_->currentIndex(), c1 = c1e_->currentIndex();
    const bool anyPl = pl1_.on->isChecked() || pl2_.on->isChecked();
    QJsonArray groups;
    if (groupsOn_->isChecked())
        for (const QSpinBox *g : groups_) if (!g->isHidden()) groups << g->value();
    QJsonObject hwp;
    if (perfOn_->isChecked()) { hwp["min_pct"] = perfMin_->value(); hwp["max_pct"] = perfMax_->value(); }
    if (epp_->currentIndex() > 0) hwp["epp"] = epp_->currentText();
    return {{"voltage", v}, {"iccmax", i}, {"tjoffset", tjOn_->isChecked() ? QJsonValue(tj_->value()) : QJsonValue()},
            {"power", QJsonObject{{"pl1", pl(pl1_, clamp1_)}, {"pl2", pl(pl2_, clamp2_)}, {"mchbar", mchbar_->isChecked()},
                                  {"pl4", pl4On_->isChecked() ? QJsonValue(pl4_->value()) : QJsonValue()},
                                  {"pp0", pp0On_->isChecked() ? QJsonValue(pp0_->value()) : QJsonValue()},
                                  {"mchbar_disable", mmioOff_->isChecked()}}},
            {"allow_positive", positive_->isChecked()}, {"lock_power", lock_->isChecked() && anyPl},
            {"disable_bdprochot", bd == 0 ? QJsonValue() : QJsonValue(bd == 1)},
            {"ctdp", ct == 0 ? QJsonValue() : QJsonValue(ct - 1)},
            {"hwphint", hwpRules()},
            {"turbo_ratio_max", turboOn_->isChecked() ? QJsonValue(turbo_->value()) : QJsonValue()},
            {"turbo_ratios", groups.isEmpty() ? QJsonValue() : QJsonValue(groups)},
            {"c1e", c1 == 0 ? QJsonValue() : QJsonValue(c1 == 2)},
            {"pkg_cstate_limit", cstOn_->isChecked() ? QJsonValue(cst_->value()) : QJsonValue()},
            {"hwp", hwp.isEmpty() ? QJsonValue() : QJsonValue(hwp)},
            {"uncore", uncOn_->isChecked() ? QJsonValue(QJsonObject{{"min_khz", uncMin_->value() * 1000}, {"max_khz", uncMax_->value() * 1000}})
                                           : QJsonValue()}};
}

QJsonArray IntelTab::hwpRules() const {
    if (!hwpOn_->isChecked()) return {};
    QJsonObject r{{"mode", hwpMode_->currentText()}, {"load_hint", hwpLoadHint_->currentText()},
                  {"normal_hint", hwpNormalHint_->currentText()}};
    if (hwpAlgo_->currentIndex() == 0) r["load"] = QJsonObject{{"multi", hwpMulti_->isChecked()}, {"threshold", hwpThreshold_->value()}};
    else r["power"] = QJsonArray{QJsonObject{{"domain", hwpDomain_->currentText()}, {"gt", hwpCmp_->currentIndex() == 0},
                                             {"watts", hwpWatts_->value()}}};
    return {r};
}

void IntelTab::setProfile(const QJsonObject &p) {
    QSignalBlocker lb(link_);
    const bool wasLinked = link_->isChecked();
    link_->setChecked(false);  // load both planes verbatim, then re-link
    positive_->setChecked(p.value("allow_positive").toBool());  // before the offsets: it sets their maximum
    auto fill = [](QList<Row> &rows, const QJsonObject &o) {
        for (Row &r : rows) {
            const QJsonValue v = o.value(r.key);
            r.on->setChecked(v.isDouble());
            if (v.isDouble()) r.val->setValue(v.toDouble());
        }
    };
    fill(volt_, p.value("voltage").toObject());
    fill(icc_, p.value("iccmax").toObject());
    tjOn_->setChecked(p.value("tjoffset").isDouble());
    if (tjOn_->isChecked()) tj_->setValue(qAbs(p.value("tjoffset").toInt()));
    turboOn_->setChecked(p.value("turbo_ratio_max").isDouble());
    if (turboOn_->isChecked()) turbo_->setValue(p.value("turbo_ratio_max").toInt());
    const QJsonObject pw = p.value("power").toObject();
    for (auto [row, key] : {std::pair{&pl1_, "pl1"}, std::pair{&pl2_, "pl2"}}) {
        const QJsonObject t = pw.value(key).toObject();
        row->on->setChecked(!t.isEmpty());
        if (!t.isEmpty()) { row->w->setValue(t.value("watts").toDouble()); row->s->setValue(t.value("seconds").toDouble(row->s->value())); }
    }
    mchbar_->setChecked(pw.value("mchbar").toBool(true));
    for (auto [box, key] : {std::pair{clamp1_, "pl1"}, std::pair{clamp2_, "pl2"}}) {
        const QJsonValue c = pw.value(key).toObject().value("clamp");
        box->setCheckState(c.isBool() ? (c.toBool() ? Qt::Checked : Qt::Unchecked) : Qt::PartiallyChecked);
    }
    for (auto [on, sp, key] : {std::tuple{pl4On_, pl4_, "pl4"}, std::tuple{pp0On_, pp0_, "pp0"}}) {
        on->setChecked(pw.value(key).isDouble());
        if (on->isChecked()) sp->setValue(pw.value(key).toDouble());
    }
    mmioOff_->setChecked(pw.value("mchbar_disable").toBool());
    const QJsonArray grp = p.value("turbo_ratios").toArray();
    groupsOn_->setChecked(!grp.isEmpty());
    for (int g = 0; g < grp.size() && g < groups_.size(); ++g) { groups_[g]->setValue(grp[g].toInt()); groups_[g]->show(); }
    const QJsonValue c1 = p.value("c1e");
    c1e_->setCurrentIndex(c1.isBool() ? (c1.toBool() ? 2 : 1) : 0);
    cstOn_->setChecked(p.value("pkg_cstate_limit").isDouble());
    if (cstOn_->isChecked()) cst_->setValue(p.value("pkg_cstate_limit").toInt());
    const QJsonObject hw2 = p.value("hwp").toObject();
    perfOn_->setChecked(hw2.contains("min_pct") || hw2.contains("max_pct"));
    // A side the profile leaves out keeps the spin box's value (the live one once read), never a range end.
    if (hw2.contains("min_pct")) perfMin_->setValue(hw2.value("min_pct").toInt());
    if (hw2.contains("max_pct")) perfMax_->setValue(hw2.value("max_pct").toInt());
    epp_->setCurrentIndex(hw2.contains("epp") ? qMax(0, epp_->findText(hw2.value("epp").toString())) : 0);
    const QJsonObject un = p.value("uncore").toObject();
    uncOn_->setChecked(!un.isEmpty());
    if (un.contains("min_khz")) uncMin_->setValue(un.value("min_khz").toInt() / 1000);
    if (un.contains("max_khz")) uncMax_->setValue(un.value("max_khz").toInt() / 1000);
    lock_->setChecked(p.value("lock_power").toBool());
    const QJsonValue bd = p.value("disable_bdprochot");
    bdprochot_->setCurrentIndex(bd.isBool() ? (bd.toBool() ? 1 : 2) : 0);
    const QJsonValue ct = p.value("ctdp");
    ctdp_->setCurrentIndex(ct.isDouble() ? ct.toInt() + 1 : 0);
    const QJsonArray hw = p.value("hwphint").toArray();
    hwpOn_->setChecked(!hw.isEmpty());
    if (!hw.isEmpty()) {
        const QJsonObject r = hw.first().toObject();
        hwpMode_->setCurrentText(r.value("mode").toString("switch"));
        hwpLoadHint_->setCurrentText(r.value("load_hint").toString());
        hwpNormalHint_->setCurrentText(r.value("normal_hint").toString());
        if (r.contains("load")) {
            hwpAlgo_->setCurrentIndex(0);
            hwpMulti_->setChecked(r.value("load").toObject().value("multi").toBool());
            hwpThreshold_->setValue(r.value("load").toObject().value("threshold").toDouble(0.8));
        } else {
            hwpAlgo_->setCurrentIndex(1);
            const QJsonArray pw = r.value("power").toArray();
            const QJsonObject t = pw.isEmpty() ? QJsonObject() : pw.first().toObject();
            hwpDomain_->setCurrentText(t.value("domain").toString("package"));
            hwpCmp_->setCurrentIndex(t.value("gt").toBool(true) ? 0 : 1);
            hwpWatts_->setValue(t.value("watts").toDouble());
        }
        if (hw.size() > 1) log(QStringLiteral("Profile has %1 hwphint rules; only the first is editable here (all are kept in the file).").arg(hw.size()), "cmd");
    }
    link_->setChecked(wasLinked && volt_[0].on->isChecked() == volt_[1].on->isChecked() && volt_[0].val->value() == volt_[1].val->value());
}

bool IntelTab::validate(const QJsonObject &p) {
    bool any = false;
    for (const char *k : {"voltage", "iccmax"})
        for (const auto &v : p.value(k).toObject()) any |= v.isDouble();
    any |= p.value("tjoffset").isDouble() || p.value("turbo_ratio_max").isDouble();
    const QJsonObject pw = p.value("power").toObject();
    any |= pw.value("pl1").isObject() || pw.value("pl2").isObject();
    any |= p.value("disable_bdprochot").isBool() || p.value("ctdp").isDouble() || !p.value("hwphint").toArray().isEmpty();
    any |= pw.value("pl4").isDouble() || pw.value("pp0").isDouble() || pw.value("mchbar_disable").toBool()
        || p.value("turbo_ratios").isArray() || p.value("c1e").isBool() || p.value("pkg_cstate_limit").isDouble()
        || p.value("hwp").isObject() || p.value("uncore").isObject();
    if (!any) { QMessageBox::information(this, "Nothing to apply", "Tick at least one value to write."); return false; }
    const QJsonObject v = p.value("voltage").toObject();
    if (v.value("core").isDouble() != v.value("cache").isDouble() || v.value("core").toDouble() != v.value("cache").toDouble())
        if (QMessageBox::warning(this, "Core / Cache differ",
                "Core and Cache are set differently. On Skylake and later they share one plane and the smaller offset "
                "is applied to both.\n\nApply anyway?", QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return false;
    QStringList lower;
    for (const Row &r : icc_) {
        const QVariant cur = r.cur->property("amps");
        const QJsonValue want = p.value("iccmax").toObject().value(r.key);
        if (want.isDouble() && cur.isValid() && want.toDouble() < cur.toDouble())
            lower << QStringLiteral("%1: %2 A → %3 A").arg(r.on->text()).arg(cur.toDouble(), 0, 'f', 2).arg(want.toDouble(), 0, 'f', 2);
    }
    if (!lower.isEmpty() &&
        QMessageBox::warning(this, "IccMax below stock", "These IccMax values are LOWER than the limits read from the CPU and "
            "will cause current-limit throttling:\n\n" + lower.join('\n') + "\n\nContinue?",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return false;
    if (p.value("lock_power").toBool() &&
        QMessageBox::warning(this, "Lock power limits", "PL1/PL2 will be LOCKED until the next reboot. Nothing — not the EC, "
            "not this program — can change them before that.\n\nContinue?", QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return false;
    if (p.value("disable_bdprochot").toBool() &&
        QMessageBox::warning(this, "Disable BD PROCHOT", "Disabling BD PROCHOT removes the EC's emergency throttle "
            "(charger overload, battery or VRM over-temperature).\n\nContinue?", QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return false;
    return true;
}

void IntelTab::setPositive(bool on) {
    for (Row &r : volt_) r.val->setMaximum(on ? UV_MAX_POSITIVE : 0.0);
}

void IntelTab::importThrottleStop() {
    const QString path = QFileDialog::getOpenFileName(this, "ThrottleStop.ini", QDir::homePath(), "ThrottleStop (*.ini);;All files (*)");
    if (path.isEmpty()) return;
    bool ok = false;
    const int idx = QInputDialog::getInt(this, "ThrottleStop profile", "Profile index (0-3):", 0, 0, 3, 1, &ok);
    if (!ok) return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > 1024 * 1024) { log("Cannot read " + path, "err"); return; }
    // Same parse as intel_uv::parse_throttlestop / undervolt.py: [ThrottleStop] FIVRVoltage{plane}{profile}=hex.
    const QStringList keys{"core", "gpu", "cache", "uncore", "analogio"};  // plane index order
    bool inSec = false;
    int n = 0;
    for (const QString &raw : QString::fromUtf8(f.readAll()).split('\n')) {
        const QString l = raw.trimmed();
        if (l.startsWith('[')) { inSec = l.compare("[ThrottleStop]", Qt::CaseInsensitive) == 0; continue; }
        const int eq = l.indexOf('=');
        if (!inSec || eq < 0) continue;
        const QString k = l.left(eq).trimmed();
        for (int plane = 0; plane < keys.size(); ++plane) {
            if (k.compare(QStringLiteral("FIVRVoltage%1%2").arg(plane).arg(idx), Qt::CaseInsensitive) != 0) continue;
            QString hex = l.mid(eq + 1).trimmed();
            if (hex.startsWith("0x", Qt::CaseInsensitive)) hex = hex.mid(2);
            bool okh = false;
            const quint64 v = hex.toULongLong(&okh, 16);
            if (!okh || v == 0) continue;
            int t = int(((v & 0xFFFFFFFFull) >> 21) & 0x7FF);
            if (t >= 0x400) t -= 0x800;
            const double mv = qRound(t / 1.024 * 10.0) / 10.0;
            for (Row &r : volt_) if (r.key == keys[plane]) { r.on->setChecked(true); r.val->setValue(mv); }
            log(QStringLiteral("ThrottleStop %1 → %2 mV").arg(keys[plane]).arg(mv, 0, 'f', 1));
            ++n;
        }
    }
    if (!n) log(QStringLiteral("No non-zero FIVRVoltage entries for profile %1 in %2").arg(idx).arg(path), "err");
    else log("Imported — review the values, then Apply.", "ok");
}

void IntelTab::saveBoot() {
    const QJsonObject p = currentProfile();
    if (!validate(p)) return;
    // Keep the other power-source slot of the stored file (world-readable, no root needed to read).
    QJsonObject cfg;
    if (QFile f(QString::fromLatin1(BOOT_FILE)); f.open(QIODevice::ReadOnly)) {
        const QJsonObject old = QJsonDocument::fromJson(f.readAll()).object();
        if (old.contains("ac") || old.contains("battery")) cfg = old;
        else if (!old.isEmpty()) cfg = {{"ac", old}, {"battery", old}};  // legacy single profile
    }
    const int t = bootTarget_->currentIndex();
    if (t == 0 || t == 1) cfg["ac"] = p;
    if (t == 0 || t == 2) cfg["battery"] = p;
    if (!cfg.contains("ac")) cfg["ac"] = QJsonValue();
    if (!cfg.contains("battery")) cfg["battery"] = QJsonValue();
    cfg["daemon"] = QJsonObject{{"interval_ms", interval_->value()}, {"reapply", reapply_->isChecked()},
                                {"alarm", alarmOn_->isChecked() ? QJsonValue(QJsonObject{{"temp_c", alarmTemp_->value()},
                                    {"pl1_w", alarmPl1_->value()}, {"pl2_w", alarmPl2_->value()}}) : QJsonValue()}};
    if (QMessageBox::question(this, "Apply at boot", QStringLiteral("Store these values for %1 and apply them at every boot and resume?\n\n"
            "Only do this with values you have already tested.").arg(bootTarget_->currentText()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
        runOp({{"op", "set_boot"}, {"config", cfg}});
}

IntelTab::~IntelTab() { stopMonitorStream(); delete csv_; }

void IntelTab::startMonitor() {
    // PGO training run: no root helper at all (the per-sample path fails fast).
    if (!monStreamFailed_ && !qEnvironmentVariableIsSet("CENTURION_PGO_TRAIN")) { startMonitorStream(); return; }
    monTimer_->start();
    pollMonitor();
}

void IntelTab::stopMonitor() {
    monTimer_->stop();
    stopMonitorStream();
}

void IntelTab::startMonitorStream() {
    if (monStream_) return;
    QString pkexec;
    for (const char *c : {"/usr/bin/pkexec", "/bin/pkexec"})
        if (QFileInfo(QString::fromLatin1(c)).isFile()) { pkexec = QString::fromLatin1(c); break; }
    if (pkexec.isEmpty() || !QFileInfo(helperPath()).isFile()) { monStreamFailed_ = true; startMonitor(); return; }
    // Parented to the app, not the tab: once pkexec has exec'd the root helper
    // we cannot kill it (EPERM), and a QProcess destructor would block the GUI
    // waiting for it. Closing stdin makes it exit within one interval.
    auto *p = new QProcess(QCoreApplication::instance());
    monStream_ = p;
    monStreamGotSample_ = false;
    p->setProgram(pkexec);
    p->setArguments({helperPath()});
    p->setProcessChannelMode(QProcess::SeparateChannels);
    connect(p, &QProcess::readyReadStandardOutput, this, [this, p] {
        while (p->canReadLine()) {
            const QByteArray line = p->readLine(1 << 20);
            const QJsonObject o = QJsonDocument::fromJson(line).object();
            if (o.isEmpty() || o.value("stream_end").toBool()) continue;
            if (!o.value("ok").toBool()) {
                const QString err = o.value("error").toString();
                if (err.startsWith(QLatin1String("unknown op"))) {  // older helper: per-sample polling instead
                    monStreamFailed_ = true;
                    if (monOn_->isChecked() && isVisible()) { monTimer_->start(); pollMonitor(); }
                } else {
                    monLbl_->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::DANGER, err.toHtmlEscaped()));
                }
                continue;
            }
            monStreamGotSample_ = true;
            if (clearLogsNext_) {  // requested before the stream was up
                clearLogsNext_ = false;
                p->write("clear\n");
            }
            showMonitor(o);
        }
        if (p->bytesAvailable() > (1 << 20)) p->readAll();  // runaway without newlines: drop
    });
    // Context is `p` (it outlives the tab); `self` guards every touch of the tab.
    QPointer<IntelTab> self(this);
    connect(p, &QProcess::finished, p, [self, this, p](int code, QProcess::ExitStatus) {
        p->deleteLater();
        if (!self || monStream_ != p) return;  // tab gone, or stopped on purpose
        monStream_ = nullptr;
        if (!monStreamGotSample_ && (code == 126 || code == 127)) {
            monLbl_->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::DANGER,
                code == 126 ? QStringLiteral("authorization dismissed") : QStringLiteral("polkit did not authorize the monitor")));
            return;  // no re-prompt loop; untick and tick again to retry
        }
        // The helper's 4 h ceiling (or a crash after samples): carry on if still wanted.
        if (monStreamGotSample_ && monOn_->isChecked() && isVisible())
            QTimer::singleShot(MONITOR_MS, this, [this] { if (monOn_->isChecked() && isVisible() && !monStream_) startMonitor(); });
    });
    connect(p, &QProcess::errorOccurred, p, [self, this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        if (self && monStream_ == p) { monStream_ = nullptr; monStreamFailed_ = true; }
        p->deleteLater();
    });
    p->start();
    const bool clear = std::exchange(clearLogsNext_, false);
    p->write(QJsonDocument(QJsonObject{{"op", "monitor_stream"}, {"interval_ms", MONITOR_MS}, {"clear_logs", clear}})
                 .toJson(QJsonDocument::Compact) + '\n');
}

void IntelTab::stopMonitorStream() {
    if (!monStream_) return;
    QProcess *p = monStream_;
    monStream_ = nullptr;
    disconnect(p, &QProcess::readyReadStandardOutput, this, nullptr);
    p->closeWriteChannel();  // helper sees EOF and exits; finished() deletes it
    if (p->state() == QProcess::Starting) p->kill();
}

void IntelTab::pollMonitor() {
    if (busy_ || monInFlight_ || !isVisible()) return;
    monInFlight_ = true;
    const bool clear = std::exchange(clearLogsNext_, false);
    privileged::run(helperPath(), QJsonObject{{"op", "monitor"}, {"clear_logs", clear}}, this, [this](const privileged::Result &r) {
        monInFlight_ = false;
        if (!r.ok()) { monLbl_->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::DANGER, r.message().toHtmlEscaped())); return; }
        showMonitor(r.json);
    }, 15000);
}

void IntelTab::showMonitor(const QJsonObject &s) {
    const QJsonObject t = s.value("throttle").toObject();
    auto lim = [&](const char *k, const char *label) {
        const bool on = t.value(k).toBool(), logged = t.value("log").toObject().value(k).toBool();
        return QStringLiteral("%1 <span style='color:%2'>%3</span>").arg(label, on ? theme::DANGER : logged ? theme::WARN : theme::OK,
                                                                           on ? "LIM" : logged ? "was" : "OK");
    };
    QStringList parts{lim("thermal", "Thermal"), lim("power", "Power"), lim("current", "Current"), lim("cross_domain", "Cross-domain")};
    if (s.contains("vcore_mv")) parts << QStringLiteral("VCore %1 mV").arg(s.value("vcore_mv").toDouble(), 0, 'f', 0);
    const QJsonObject e = s.value("energy").toObject(), pe = monPrev_.value("energy").toObject();
    const double dt = s.value("t").toDouble() - monPrev_.value("t").toDouble();
    double pkgW = qQNaN(), coreW = qQNaN();
    if (!pe.isEmpty() && dt > 0) {
        const double wrap = e.value("wrap").toDouble(4294967296.0);
        for (const char *d : {"package", "core", "graphics", "dram"}) {
            const QJsonValue a = e.value("raw").toObject().value(d), b = pe.value("raw").toObject().value(d);
            if (!a.isDouble() || !b.isDouble()) continue;
            const double unit = e.value(QString::fromLatin1(d) == "dram" ? "dram_unit_j" : "unit_j").toDouble();
            const double w = std::fmod(a.toDouble() - b.toDouble() + wrap, wrap) * unit / dt;
            if (QString::fromLatin1(d) == "package") pkgW = w;
            if (QString::fromLatin1(d) == "core") coreW = w;
            parts << QStringLiteral("%1 %2 W").arg(QString::fromLatin1(d)).arg(w, 0, 'f', 1);
        }
    }
    recordSample(s, pkgW, coreW);
    updateCpuTable(s);
    monPrev_ = s;
    monLbl_->setText(parts.join(" · "));
    updateLimits(s.value("limits").toObject());
}

void IntelTab::recordSample(const QJsonObject &s, double pkgW, double coreW) {
    const double pkgC = s.value("pkg_temp").isDouble() ? s.value("pkg_temp").toDouble() : qQNaN();
    const double vcore = s.value("vcore_mv").isDouble() ? s.value("vcore_mv").toDouble() : qQNaN();
    hist_.append({s.value("t").toDouble(), pkgW, coreW, pkgC, vcore});
    const double keep = 300.0;  // seconds
    while (!hist_.isEmpty() && hist_.last().t - hist_.first().t > keep) hist_.removeFirst();
    drawTrend();
    if (!csv_) return;
    const QJsonObject t = s.value("throttle").toObject();
    auto num = [](double v, int prec) { return std::isnan(v) ? QString() : QString::number(v, 'f', prec); };
    csv_->write(QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8\n")
        .arg(QDateTime::fromSecsSinceEpoch(qint64(s.value("t").toDouble())).toString(Qt::ISODate))
        .arg(num(pkgW, 2), num(coreW, 2), num(pkgC, 0), num(vcore, 0))
        .arg(int(t.value("thermal").toBool())).arg(int(t.value("power").toBool())).arg(int(t.value("current").toBool())).toUtf8());
    csv_->flush();
}

void IntelTab::drawTrend() {
    const int sel = trendSel_->currentIndex();
    QVector<LineChart::Point> pts;
    for (const Sample &x : std::as_const(hist_)) {
        const double y = sel == 0 ? x.pkgW : sel == 1 ? x.pkgC : x.vcore;
        if (std::isnan(y)) continue;
        pts.append({x.t, y, QDateTime::fromSecsSinceEpoch(qint64(x.t)).toString("HH:mm:ss") + QStringLiteral(" · %1").arg(y, 0, 'f', sel == 0 ? 1 : 0)});
    }
    trend_->setPoints(pts, sel == 0 ? "W" : sel == 1 ? "°C" : "mV");
}

void IntelTab::updateCpuTable(const QJsonObject &s) {
    QHash<int, QJsonObject> prev;
    for (const auto &v : monPrev_.value("percpu").toArray()) prev.insert(v.toObject().value("cpu").toInt(), v.toObject());
    const QJsonArray cur = s.value("percpu").toArray();
    const double base = s.value("base_mhz").toDouble();
    const int tjmax = s.value("tjmax").toInt();
    cpuTable_->setRowCount(cur.size());
    auto cell = [this](int row, int col, const QString &text) {
        auto *it = cpuTable_->item(row, col);
        if (!it) { it = new QTableWidgetItem; it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter); cpuTable_->setItem(row, col, it); }
        it->setText(text);
    };
    for (int row = 0; row < cur.size(); ++row) {
        const QJsonObject c = cur[row].toObject();
        const int cpu = c.value("cpu").toInt();
        cell(row, 0, QString::number(cpu));
        const QJsonObject p = prev.value(cpu);
        // Counters are 64-bit and only grow between two samples of one boot; a smaller value means a reset.
        const double da = c.value("aperf").toDouble() - p.value("aperf").toDouble(),
                     dm = c.value("mperf").toDouble() - p.value("mperf").toDouble(),
                     dt = c.value("tsc").toDouble() - p.value("tsc").toDouble();
        const bool rates = !p.isEmpty() && da >= 0 && dm > 0 && dt > 0;
        cell(row, 1, rates && base > 0 ? QString::number(base * da / dm, 'f', 0) : QStringLiteral("–"));
        cell(row, 2, rates ? QString::number(qMin(100.0, 100.0 * dm / dt), 'f', 1) : QStringLiteral("–"));
        if (c.value("dts").isDouble() && tjmax > 0) {
            const int t = tjmax - c.value("dts").toInt();
            maxTemp_[cpu] = qMax(maxTemp_.value(cpu, t), t);
            cell(row, 3, QString::number(t));
            cell(row, 4, QString::number(maxTemp_[cpu]));
        } else { cell(row, 3, QStringLiteral("–")); cell(row, 4, QStringLiteral("–")); }
    }
}

void IntelTab::updateLimits(const QJsonObject &limits) {
    if (limits.isEmpty()) {
        limInfo_->setText("This CPU returned none of the limit-reason registers.");
        return;
    }
    ++limSamples_;
    for (auto it = limCells_.cbegin(); it != limCells_.cend(); ++it) {
        const QString dom = it.key().section(':', 0, 0);
        const int bit = it.key().section(':', 1).toInt();
        if (!limits.contains(dom)) { it.value()->setText("n/a"); continue; }
        const quint64 v = quint64(limits.value(dom).toDouble());
        const bool now = (v >> bit) & 1, logged = (v >> (bit + 16)) & 1;
        int &n = limCounts_[it.key()];
        if (now) ++n;
        const QString color = now ? theme::DANGER : (logged || n) ? theme::WARN : theme::MUTED;
        it.value()->setText(QStringLiteral("<span style='color:%1'>● %2</span>").arg(color).arg(n));
        it.value()->setToolTip(now ? "limiting now" : logged ? "happened since the last reset (sticky log bit)" : QString());
    }
    limInfo_->setText(QStringLiteral("%1 samples (~%2 s). Red = limiting now, amber = happened since reset.")
                          .arg(limSamples_).arg(limSamples_ * MONITOR_MS / 1000));
}

void IntelTab::resetLimits() {
    limCounts_.clear();
    limSamples_ = 0;
    if (monStream_ && monStreamGotSample_) monStream_->write("clear\n");  // next streamed sample clears them
    else clearLogsNext_ = true;  // the next monitor sample clears the sticky log bits
    for (QLabel *c : std::as_const(limCells_)) { c->setText(QStringLiteral("·")); c->setToolTip({}); }
    limInfo_->setText(monOn_->isChecked() ? QStringLiteral("Counters reset.") : QStringLiteral("Counters reset; press Start counting."));
}

// ── helper calls ────────────────────────────────────────────────────────────

void IntelTab::setBusy(bool b) {
    busy_ = b;
    for (QPushButton *p : std::as_const(buttons_)) p->setEnabled(!b);
}

void IntelTab::runOp(const QJsonObject &req, std::function<void(const QJsonObject &)> then) {
    if (busy_) { log("Previous operation still running, please wait.", "err"); return; }
    const QString op = req.value("op").toString();
    if (op != "status") log("Sending '" + op + "' via pkexec…", "cmd");
    if (op == QLatin1String("reset")) trial_->cancel();
    setBusy(true);
    privileged::run(helperPath(), req, this, [this, op, then](const privileged::Result &r) {
        setBusy(false);
        if (!r.reached) { log(r.error, "err"); return; }
        const QJsonObject j = r.json;
        if (j.contains("boot_profile"))
            bootLbl_->setText(j.value("boot_profile").toBool()
                ? QStringLiteral("<span style='color:%1'>⏻ boot/resume profile active</span>").arg(theme::OK)
                : QStringLiteral("No boot/resume profile."));
        for (const auto &v : j.value("results").toArray()) {
            const QJsonObject o = v.toObject();
            log(o.value("what").toString() + ": " + o.value("message").toString(), o.value("ok").toBool() ? "ok" : "err");
        }
        if (op == "status") { if (!r.ok()) log(r.message(), "err"); showStatus(j); }
        else if (r.ok()) log(j.value("message").toString(op + " done."), "ok");
        else log(op + " failed" + (r.message().isEmpty() ? QString() : ": " + r.message()), "err");
        if (r.ok() && op == QLatin1String("apply")) trial_->offer([this] { revertTrial(); });
        if (then) then(j);
    }, PKEXEC_TIMEOUT_MS);
}

void IntelTab::readStatus() {
    if (busy_) return;
    runOp({{"op", "status"}});
}

void IntelTab::domainOverview() {
    diag::Report report;
    report.code = QStringLiteral("IntelTopology"); report.title = tr("Intel Voltage Domains and CPU Topology");
    report.about = tr("Voltage plane support is based on the last OC mailbox response. Logical CPU topology comes from sysfs. No voltage probe or write is started here.");
    report.limits << tr("Domain sharing is not inferred from names or frequencies. A raw core type is shown only when the kernel reports it.");
    diag::Table domains; domains.title = tr("Voltage Domain Readback");
    domains.headers = {tr("Plane"), tr("Mailbox status"), tr("Offset"), tr("Draft"), tr("Selected")};
    if (voltageSnapshotTime_.isEmpty()) report.add(diag::Level::Unknown, tr("No voltage readback has been captured."));
    else report.add(diag::Level::Info, tr("Last voltage readback: %1 (snapshot, not live)").arg(voltageSnapshotTime_));
    for (const Row &r : volt_) {
        const auto data = voltageSnapshot_.value(r.key).toObject();
        const bool supported = data.value(QStringLiteral("mv")).isDouble() && data.value(QStringLiteral("status")).toInt(-1) == 0;
        domains.add({r.on->text(), data.value(QStringLiteral("status")).isDouble() ? QString::number(data.value(QStringLiteral("status")).toInt()) : tr("Unknown"),
            supported ? QStringLiteral("%1 mV").arg(data.value(QStringLiteral("mv")).toDouble(), 0, 'f', 1) : tr("Unknown"),
            supported ? QStringLiteral("%1 mV").arg(r.val->value(), 0, 'f', 1) : tr("Unavailable"),
            supported && r.on->isChecked() ? tr("Yes") : tr("No")}, supported ? diag::Level::Info : diag::Level::Unknown);
    }
    report.tables << domains << systemdiagnostics::cpuTopology();
    QDialog dlg(this); dlg.setWindowTitle(report.title); dlg.resize(900, 600);
    auto *layout = new QVBoxLayout(&dlg); auto *view = new DiagReportView; view->setReport(report); layout->addWidget(view, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close); connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject); layout->addWidget(buttons); dlg.exec();
}

void IntelTab::showStatus(const QJsonObject &s) {
    QStringList bits;
    const QJsonObject cpu = s.value("cpu").toObject();
    if (!cpu.isEmpty())
        bits << QStringLiteral("<b>%1</b> · %2 (model %3, stepping %4)").arg(cpu.value("name").toString().toHtmlEscaped(),
                cpu.value("codename").toString()).arg(cpu.value("model").toInt()).arg(cpu.value("stepping").toInt());
    const QString lock = s.value("lockdown").toString();
    if (!lock.isEmpty() && lock != "none")
        bits << QStringLiteral("<span style='color:%1'>kernel lockdown: %2 — MSR writes are blocked</span>").arg(theme::DANGER, lock);
    if (s.contains("allow_writes") && s.value("allow_writes").toString() != "on")
        bits << QStringLiteral("<span style='color:%1'>msr.allow_writes=%2</span>").arg(theme::WARN, s.value("allow_writes").toString());

    const QJsonObject v = s.value("voltage").toObject();
    voltageSnapshot_ = v; voltageSnapshotTime_ = QDateTime::currentDateTime().toString(Qt::ISODate);
    bool anyPlane = false;
    for (Row &r : volt_) {
        const QJsonObject o = v.value(r.key).toObject();
        // Only planes the CPU answers for (status 0) are shown: System Agent on
        // Arrow Lake, MemSS/NGU everywhere else, are not offered at all.
        const bool supported = o.contains("mv") && o.value("status").toInt(-1) == 0;
        for (QWidget *w : {static_cast<QWidget *>(r.on), static_cast<QWidget *>(r.val), static_cast<QWidget *>(r.cur)})
            w->setVisible(supported);
        if (!supported) { r.on->setChecked(false); continue; }
        anyPlane = true;
        r.cur->setText(o.contains("mv") ? QStringLiteral("%1 mV").arg(o.value("mv").toDouble(), 0, 'f', 1) : QStringLiteral("error"));
        r.cur->setToolTip(o.contains("raw") ? "raw " + o.value("raw").toString() : o.value("error").toString());
    }
    noPlanes_->setVisible(!anyPlane);
    link_->setVisible(anyPlane);
    for (QWidget *w : planeInfo_) w->setVisible(anyPlane);
    const QJsonObject ic = s.value("iccmax").toObject();
    for (Row &r : icc_) {
        const QJsonObject o = ic.value(r.key).toObject();
        r.cur->setText(o.contains("amps") ? QStringLiteral("%1 A%2").arg(o.value("amps").toDouble(), 0, 'f', 2)
                                                .arg(o.value("unlimited").toBool() ? " (unlimited)" : "")
                                          : QStringLiteral("n/a"));
        r.cur->setToolTip(o.contains("raw") ? "raw " + o.value("raw").toString() + (o.contains("error") ? "\n" + o.value("error").toString() : QString())
                                            : o.value("error").toString());
        r.cur->setProperty("amps", o.contains("amps") ? QVariant(o.value("amps").toDouble()) : QVariant());
        // Pre-fill the editor with the stock limit so ticking the box never lowers it by accident.
        if (o.contains("amps") && !r.on->isChecked()) r.val->setValue(o.value("amps").toDouble());
    }
    const QJsonObject t = s.value("temp").toObject();
    if (t.contains("tjmax")) {
        tjCur_->setText(QStringLiteral("%1 − %2 = %3 °C").arg(t.value("tjmax").toInt()).arg(t.value("offset").toInt()).arg(t.value("target").toInt()));
        tjOn_->setEnabled(t.value("programmable").toBool(true));
        if (!tjOn_->isEnabled()) tjOn_->setChecked(false);
    } else tjCur_->setText("n/a");
    const QJsonObject turboSt = s.value("turbo_ratio").toObject();
    if (turboSt.contains("current_max")) {
        const int base = turboSt.value("base").toInt(), stock = turboSt.value("stock_max").toInt(), cur = turboSt.value("current_max").toInt();
        turboCur_->setText(QStringLiteral("× %1 (%2 GHz) · stock × %3").arg(cur).arg(cur / 10.0, 0, 'f', 1).arg(stock));
        turboCur_->setToolTip("raw " + turboSt.value("raw").toString() + QStringLiteral("\nbase (non-turbo) ratio × %1").arg(base));
        if (base > 0 && stock >= base) turbo_->setRange(base, stock);
        // Pre-fill with the live cap so ticking the box changes nothing by itself.
        if (!turboOn_->isChecked()) turbo_->setValue(cur);
        turboOn_->setEnabled(turboSt.value("programmable").toBool());
        if (!turboOn_->isEnabled()) turboOn_->setChecked(false);
    } else {
        turboCur_->setText("n/a");
        turboCur_->setToolTip(turboSt.value("error").toString());
    }
    const QJsonArray stockG = turboSt.value("stock").toArray(), curG = turboSt.value("current").toArray();
    QStringList gtext;
    for (int g = 0; g < groups_.size(); ++g) {
        const int st = g < stockG.size() ? stockG[g].toInt() : 0;
        groups_[g]->setVisible(st > 0);
        if (st <= 0) continue;
        groups_[g]->setRange(qMax(1, turboSt.value("base").toInt()), st);
        if (!groupsOn_->isChecked() && g < curG.size()) groups_[g]->setValue(curG[g].toInt());
        gtext << QStringLiteral("%1/%2").arg(g < curG.size() ? curG[g].toInt() : 0).arg(st);
    }
    groupsCur_->setText(gtext.isEmpty() ? QStringLiteral("n/a") : "now/stock: " + gtext.join("  "));
    groupsOn_->setEnabled(turboSt.value("programmable").toBool() && !gtext.isEmpty());
    if (!groupsOn_->isEnabled()) groupsOn_->setChecked(false);
    const QJsonObject hs = s.value("hwp").toObject();
    perfCur_->setText(hs.value("max_pct").isDouble() ? QStringLiteral("now %1–%2 %").arg(hs.value("min_pct").toInt()).arg(hs.value("max_pct").toInt())
                                                     : QStringLiteral("n/a"));
    perfOn_->setEnabled(hs.value("max_pct").isDouble());
    if (perfOn_->isEnabled() && !perfOn_->isChecked()) { perfMin_->setValue(hs.value("min_pct").toInt()); perfMax_->setValue(hs.value("max_pct").toInt()); }
    if (hs.value("epp").isString()) epp_->setItemText(0, "untouched (now " + hs.value("epp").toString() + ")");
    const QJsonObject us = s.value("uncore").toObject();
    if (us.value("max_khz").isDouble()) {
        const int lo = us.value("initial_min_khz").toInt() / 1000, hi = us.value("initial_max_khz").toInt() / 1000;
        uncCur_->setText(QStringLiteral("now %1–%2 · stock %3–%4").arg(us.value("min_khz").toInt() / 1000).arg(us.value("max_khz").toInt() / 1000).arg(lo).arg(hi));
        for (QSpinBox *sp : {uncMin_, uncMax_}) sp->setRange(lo, hi);
        if (!uncOn_->isChecked()) { uncMin_->setValue(us.value("min_khz").toInt() / 1000); uncMax_->setValue(us.value("max_khz").toInt() / 1000); }
        uncOn_->setEnabled(true);
    } else {
        uncCur_->setText("n/a");
        uncCur_->setToolTip(us.value("error").toString());
        uncOn_->setEnabled(false);
        uncOn_->setChecked(false);
    }
    const QJsonObject c1s = s.value("c1e").toObject();
    if (c1s.contains("enabled")) c1e_->setItemText(0, QStringLiteral("untouched (now %1)").arg(c1s.value("enabled").toBool() ? "on" : "off"));
    const QJsonObject cs = s.value("pkg_cstate").toObject();
    if (cs.contains("limit")) {
        const bool locked = cs.value("locked").toBool();
        cstCur_->setText(QStringLiteral("now %1%2").arg(cs.value("limit").toInt()).arg(locked ? " · locked" : ""));
        cst_->setMaximum(cs.value("limit").toInt());
        if (!cstOn_->isChecked()) cst_->setValue(cs.value("limit").toInt());
        cstOn_->setEnabled(!locked);
        if (locked) cstOn_->setChecked(false);
    } else cstCur_->setText("n/a");
    const QJsonObject pw = s.value("power").toObject();
    auto term = [](const QJsonObject &o) {
        return QStringLiteral("%1 W · %2 s%3").arg(o.value("watts").toDouble(), 0, 'f', 1).arg(o.value("seconds").toDouble(), 0, 'g', 4)
            .arg(o.value("enabled").toBool() ? "" : " (off)");
    };
    pl1_.cur->setText(pw.contains("pl1") ? term(pw.value("pl1").toObject()) : "n/a");
    pl2_.cur->setText(pw.contains("pl2") ? term(pw.value("pl2").toObject()) : "n/a");
    const bool plLocked = pw.value("locked").toBool();
    for (PlRow *p : {&pl1_, &pl2_}) { p->on->setEnabled(!plLocked); if (plLocked) p->on->setChecked(false); }
    const QJsonObject mc = pw.value("mchbar").toObject();
    QString mtext = mc.contains("error") ? "MCHBAR: " + mc.value("error").toString()
        : QStringLiteral("MCHBAR %1: PL1 %2 W / PL2 %3 W%4").arg(mc.value("base").toString())
              .arg(mc.value("pl1").toObject().value("watts").toDouble(), 0, 'f', 1)
              .arg(mc.value("pl2").toObject().value("watts").toDouble(), 0, 'f', 1)
              .arg(mc.value("matches_msr").toBool() ? " (= MSR)" : " (≠ MSR, the lower one wins)");
    plLock_->setText((plLocked ? QStringLiteral("MSR power limit is LOCKED by firmware. ") : QString()) + mtext +
                     ". The EC may re-program PL1/PL2 on profile changes.");
    mmioOff_->setEnabled(!mc.contains("error") && !mc.value("locked").toBool());
    if (!mmioOff_->isEnabled()) mmioOff_->setChecked(false);
    for (auto [on, sp, cur, key] : {std::tuple{pl4On_, pl4_, pl4Cur_, "pl4"}, std::tuple{pp0On_, pp0_, pp0Cur_, "pp0"}}) {
        const QJsonObject o = pw.value(key).toObject();
        const bool ok = o.value("watts").isDouble();
        cur->setText(ok ? QStringLiteral("%1 W%2").arg(o.value("watts").toDouble(), 0, 'f', 1)
                              .arg(o.value("enabled").isBool() && !o.value("enabled").toBool() ? " (off)" : "") : QStringLiteral("n/a"));
        // The editor stops at the helper's 250 W: the clamped value would lower a higher stock limit unnoticed.
        if (ok && o.value("watts").toDouble() > sp->maximum())
            cur->setText(cur->text() + QStringLiteral(" — above the %1 W editor maximum: ticking lowers it to %1 W").arg(sp->maximum(), 0, 'f', 0));
        cur->setToolTip(o.value("error").toString());
        on->setEnabled(ok);
        if (!ok) on->setChecked(false);
        else if (!on->isChecked() && o.value("watts").toDouble() >= 1) sp->setValue(o.value("watts").toDouble());
    }

    const QJsonObject bd = s.value("bdprochot").toObject();
    if (bd.contains("enabled")) bdprochot_->setToolTip(bdprochot_->toolTip().section("\n\nNow:", 0, 0) +
                                                     QStringLiteral("\n\nNow: %1").arg(bd.value("enabled").toBool() ? "enabled" : "disabled"));
    const QJsonObject ct = s.value("ctdp").toObject();
    const bool ctOk = ct.value("programmable").toBool() && !ct.value("locked").toBool();
    ctdp_->setEnabled(ctOk);
    if (!ctOk) ctdp_->setCurrentIndex(0);
    else for (int lvl = 0; lvl < 3; ++lvl)
        if (auto *m = qobject_cast<QStandardItemModel *>(ctdp_->model())) m->item(lvl + 1)->setEnabled(lvl <= ct.value("levels").toInt());
    if (ct.value("current").isDouble()) ctdp_->setItemText(0, QStringLiteral("untouched (now %1)").arg(ct.value("current").toInt()));

    // BIOS toggles: OC Lock is a plain MSR bit; undervolt protection needs the write probe.
    const QJsonObject oc = s.value("oc_lock").toObject();
    if (oc.contains("locked"))
        bits << (oc.value("locked").toBool()
            ? QStringLiteral("<span style='color:%1'><b>OC Lock: ON</b> (BIOS \"Overclocking Lock\", MSR 0x194 bit 20) — "
                             "voltage offsets cannot be changed until it is disabled in the BIOS.</span>").arg(theme::DANGER)
            : QStringLiteral("<span style='color:%1'><b>OC Lock: off</b></span> (MSR 0x194 %2)").arg(theme::OK, oc.value("raw").toString()));
    else if (oc.contains("error"))
        bits << "OC Lock: unreadable (" + oc.value("error").toString().toHtmlEscaped() + ")";
    if (uvLock_)
        bits << (*uvLock_ ? QStringLiteral("<span style='color:%1'><b>Undervolt: LOCKED</b> by the BIOS (write test rejected — "
                                           "Undervolt Protection / CVE-2019-11157).</span>").arg(theme::DANGER)
                          : QStringLiteral("<span style='color:%1'><b>Undervolt: unlocked</b></span> (write test accepted).").arg(theme::OK));
    else
        bits << QStringLiteral("Undervolt protection: not tested — press <b>Test UV lock</b> (a 1 mV write, restored).");
    info_->setText(bits.join("<br>"));
    log("Values read from the CPU.", "ok");
}

// ── public (tray / game mode) ───────────────────────────────────────────────

void IntelTab::revertTrial() {
    if (busy_) { QTimer::singleShot(1500, this, &IntelTab::revertTrial); return; }
    log("Trial over: voltage offsets back to 0 mV.", "cmd");
    applyReset();
}

void IntelTab::applyReset() { runOp({{"op", "reset"}}, [this](const QJsonObject &) { readStatus(); }); }

bool IntelTab::applyNamedProfile(const QString &name) {
    if (busy_ || !loadProfile(name)) return false;
    runOp({{"op", "apply"}, {"profile", currentProfile()}}, [this](const QJsonObject &) { readStatus(); });
    return true;
}

// ── profiles ────────────────────────────────────────────────────────────────

QStringList IntelTab::savedProfileNames() const {
    QStringList n = QDir(inteluv::profilesDir()).entryList({"*.json"}, QDir::Files, QDir::Name);
    for (QString &s : n) s.chop(5);
    return n;
}

void IntelTab::reloadProfiles() {
    const QString prev = profileCombo_->currentText();
    profileCombo_->clear();
    profileCombo_->addItems(savedProfileNames());
    if (int i = profileCombo_->findText(prev); i >= 0) profileCombo_->setCurrentIndex(i);
}

void IntelTab::saveProfile() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, "Save Profile", "Profile name:", QLineEdit::Normal,
                                               profileCombo_->currentText(), &ok).trimmed();
    if (!ok) return;
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9 _-]{0,63}\\z"));
    if (!re.match(name).hasMatch()) {
        QMessageBox::warning(this, "Invalid Name", "Use 1-64 characters: letters, digits, space, underscore or hyphen.");
        return;
    }
    const QString path = inteluv::profilesDir() + '/' + name + ".json";
    if (QFile::exists(path) && QMessageBox::question(this, "Overwrite Profile", "Profile '" + name + "' already exists. Overwrite?",
                                                     QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(currentProfile()).toJson()) < 0 || !f.commit()) {
        QMessageBox::critical(this, "Save Error", f.errorString());
        return;
    }
    reloadProfiles();
    profileCombo_->setCurrentText(name);
    log("Profile saved: " + path);
}

bool IntelTab::loadProfile(const QString &name) {
    if (name.isEmpty()) return false;
    QFile f(inteluv::profilesDir() + '/' + name + ".json");
    if (!f.open(QIODevice::ReadOnly) || f.size() > 64 * 1024) { log("Cannot read profile '" + name + "': " + f.errorString(), "err"); return false; }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) { log("Profile '" + name + "' is not a JSON object.", "err"); return false; }
    setProfile(doc.object());
    profileCombo_->setCurrentText(name);
    log("Profile loaded: " + name);
    return true;
}

void IntelTab::deleteProfile() {
    const QString name = profileCombo_->currentText();
    if (name.isEmpty() || QMessageBox::question(this, "Delete Profile", "Delete profile '" + name + "'?",
                                                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    if (!QFile::remove(inteluv::profilesDir() + '/' + name + ".json")) log("Could not delete '" + name + "'.", "err");
    reloadProfiles();
}
