#include "ryzentab.h"
#include "cpustress.h"
#include "trialbar.h"
#include <QShowEvent>
#include "platformprofile.h"
#include "privileged.h"
#include "sysinfo.h"
#include "theme.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QCoreApplication>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QSettings>
#include <QTimer>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QIntValidator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QSet>
#include <QStandardPaths>
#include <QGridLayout>
#include <QVBoxLayout>
#include <climits>
#include <numeric>

static constexpr int SLOTS_PER_CCD = 8, CCD_FALLBACK = 2;
static constexpr int CO_MIN = -50, CO_MAX = 20;
static constexpr int PKEXEC_TIMEOUT_MS = 300000;  // covers reading/typing the polkit prompt
static constexpr int MONITOR_MS = 1000;
static constexpr double LIMIT_PCT = 95.0;  // a limit counts as holding the CPU at this share of it
static constexpr int COLS = 7;  // Slot, Offset, Disable, CPPC, MHz, V, W

static QString helperPath() { return privileged::helperPath(QStringLiteral("ryzen-co-helper")); }

// ── topology ────────────────────────────────────────────────────────────────

static QList<int> expandCpuList(const QString &s) {
    QList<int> out;
    for (const QString &part : s.split(',', Qt::SkipEmptyParts)) {
        const QStringList lh = part.trimmed().split('-');
        bool a = false, b = true;
        int lo = lh.value(0).toInt(&a), hi = lh.size() > 1 ? lh[1].toInt(&b) : lo;
        if (a && b && hi >= lo && hi - lo < 4096) for (int i = lo; i <= hi; ++i) out << i;
    }
    return out;
}

// Stable identity of an L3 domain: cache/index3/id (derived from the APIC ID, i.e. the
// physical die position) plus its size. Independent of which CPUs happen to be online and of
// every Optimizations knob (x3d_mode, EPP, boost... change CPPC/frequency, never the APIC ID).
struct L3Info { int id = -1, kib = 0; };
static L3Info l3Info(const QString &cpuDir) {
    const QDir cache(cpuDir + "/cache");
    for (const QString &idx : cache.entryList({"index*"}, QDir::Dirs, QDir::Name))
        if (pp::readText(cache.filePath(idx) + "/level") == QStringLiteral("3")) {
            bool ok = false;
            L3Info li;
            const int v = pp::readText(cache.filePath(idx) + "/id").value_or(QString()).toInt(&ok);
            if (ok) li.id = v;
            QString sz = pp::readText(cache.filePath(idx) + "/size").value_or(QString());
            const int mul = sz.endsWith('M') ? 1024 : 1;
            if (sz.endsWith('K') || sz.endsWith('M')) sz.chop(1);
            li.kib = sz.toInt() * mul;
            return li;
        }
    return {};
}

// Per-boot record of each CPU's L3 identity. An offline CPU (parked CCD, SMT off, mid hot-plug)
// has no cache/ directory, so without this a GUI started while a CCD is parked could only order
// CCDs by CPU number, which is not the SMU's die order. Kept in the user's runtime dir (tmpfs)
// and tied to boot_id, since APIC IDs are only fixed for one boot.
static QString topoCachePath() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    return dir.isEmpty() ? QString() : dir + QStringLiteral("/centurion-cpu-l3.json");
}
static QString bootId() { return pp::readText(QStringLiteral("/proc/sys/kernel/random/boot_id")).value_or(QString()); }

static QMap<int, L3Info> loadTopoCache() {
    QMap<int, L3Info> out;
    QFile f(topoCachePath());
    if (topoCachePath().isEmpty() || !f.open(QIODevice::ReadOnly) || f.size() > 65536) return out;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (bootId().isEmpty() || o.value("boot_id").toString() != bootId()) return out;
    const QJsonObject cpus = o.value("cpus").toObject();
    for (auto it = cpus.begin(); it != cpus.end(); ++it) {
        bool ok = false;
        const int n = it.key().toInt(&ok);
        const QJsonArray a = it.value().toArray();
        if (ok && a.size() == 2 && a[0].toInt(-1) >= 0) out[n] = {a[0].toInt(), a[1].toInt()};
    }
    return out;
}

static void saveTopoCache(const QMap<int, L3Info> &m) {
    if (topoCachePath().isEmpty() || bootId().isEmpty()) return;
    QJsonObject cpus;
    for (auto it = m.cbegin(); it != m.cend(); ++it) cpus[QString::number(it.key())] = QJsonArray{it->id, it->kib};
    QSaveFile f(topoCachePath());
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(QJsonObject{{"boot_id", bootId()}, {"cpus", cpus}}).toJson(QJsonDocument::Compact));
    f.commit();
}

static std::optional<QString> l3Shared(const QString &cpuDir) {
    const QDir cache(cpuDir + "/cache");
    for (const QString &idx : cache.entryList({"index*"}, QDir::Dirs, QDir::Name))
        if (pp::readText(cache.filePath(idx) + "/level") == QStringLiteral("3"))
            return pp::readText(cache.filePath(idx) + "/shared_cpu_list");
    return std::nullopt;
}

QList<int> ryzen::parkedCpus() {
    // Written by tune-helper (root) when it parks a CCD; valid while all its CPUs are offline.
    QFile f(QStringLiteral("/run/centurion/tune/ccd-park.json"));
    if (!f.open(QIODevice::ReadOnly) || f.size() > 4096) return {};
    QList<int> cpus;
    for (const auto &v : QJsonDocument::fromJson(f.readAll()).object().value("cpus").toArray()) cpus << v.toInt();
    const QList<int> online = expandCpuList(pp::readText(QStringLiteral("/sys/devices/system/cpu/online")).value_or(QString()));
    if (cpus.isEmpty() || std::any_of(cpus.begin(), cpus.end(), [&](int c) { return online.contains(c); })) return {};
    std::sort(cpus.begin(), cpus.end());
    return cpus;
}

ryzen::Layout ryzen::detect() {
    Layout out;
    const QDir sys(QStringLiteral("/sys/devices/system/cpu"));
    // Online CPUs only, and L3 lists that overlap are one CCD: during hot-plug
    // one CPU can still report "0-7,16-23" and its neighbour "0-7" for the
    // same L3 (same merge rule as tune.rs group_l3).
    const QList<int> online = expandCpuList(pp::readText(sys.filePath("online")).value_or(QString()));
    auto isOnline = [&](int c) { return online.isEmpty() || online.contains(c); };
    QList<QList<int>> groups;
    QMap<int, L3Info> info = loadTopoCache();  // cpu -> L3 identity (live values override the record)
    bool dirty = false;
    for (const QString &c : sys.entryList({"cpu[0-9]*"}, QDir::Dirs)) {
        const int n = c.mid(3).toInt();
        if (!isOnline(n)) continue;
        const auto s = l3Shared(sys.filePath(c));
        if (!s) continue;
        if (const L3Info li = l3Info(sys.filePath(c)); li.id >= 0 && (info.value(n).id != li.id || info.value(n).kib != li.kib)) {
            info[n] = li;
            dirty = true;
        }
        QList<int> g;
        for (int x : expandCpuList(*s)) if (isOnline(x) && !g.contains(x)) g << x;
        if (!g.contains(n)) g << n;
        for (int i = groups.size() - 1; i >= 0; --i) {
            if (std::none_of(groups[i].begin(), groups[i].end(), [&](int x) { return g.contains(x); })) continue;
            for (int x : groups[i]) if (!g.contains(x)) g << x;
            groups.removeAt(i);
        }
        std::sort(g.begin(), g.end());
        groups << g;
    }
    // A parked CCD has no online CPU, so sysfs no longer lists it: add it back
    // from the park record so it keeps its slot in the grid (shown greyed out).
    const QList<int> parked = parkedCpus();
    if (!parked.isEmpty() && std::none_of(groups.begin(), groups.end(), [&](const QList<int> &g) {
            return std::any_of(parked.begin(), parked.end(), [&](int c) { return g.contains(c); }); }))
        groups << parked;
    if (dirty) saveTopoCache(info);
    // Order by L3 id (the die order the SMU's CO slots follow). Offline CPUs take their id from
    // the per-boot record, so a parked CCD keeps its place even in a freshly started GUI.
    auto keyOf = [&](const QList<int> &g) {
        for (int c : g) if (info.value(c).id >= 0) return info.value(c).id;
        return -1;
    };
    const bool haveIds = std::all_of(groups.begin(), groups.end(), [&](const QList<int> &g) { return keyOf(g) >= 0; });
    // Remember the L3-id order (by each CCD's lowest CPU, which survives SMT/park) and reuse it
    // whenever ids are unreadable (hot-plug transition), instead of flipping to CPU-number order.
    static QList<int> stableOrder;
    auto known = [&] {
        if (stableOrder.size() != groups.size()) return false;
        return std::all_of(groups.begin(), groups.end(), [&](const QList<int> &g) { return stableOrder.contains(g.first()); });
    };
    std::sort(groups.begin(), groups.end(), [&](const QList<int> &a, const QList<int> &b) {
        if (haveIds) return keyOf(a) < keyOf(b);
        if (known()) return stableOrder.indexOf(a.first()) < stableOrder.indexOf(b.first());
        return a.first() < b.first(); });
    if (haveIds) { stableOrder.clear(); for (const auto &g : groups) stableOrder << g.first(); }
    out.ccdCount = groups.size();
    for (int ccd = 0; ccd < groups.size(); ++ccd) {
        int kib = 0;
        for (int c : groups[ccd]) kib = std::max(kib, info.value(c).kib);
        if (kib > 0) out.l3Kib[ccd] = kib;
    }
    for (int ccd = 0; ccd < groups.size(); ++ccd) {
        if (groups[ccd] != parked) continue;
        // Offline CPUs have no topology/: pair thread i with i + n/2 when the die has SMT.
        out.parked.insert(ccd);
        const int n = parked.size(), cores = n > SLOTS_PER_CCD ? n / 2 : n;
        for (int i = 0; i < cores; ++i)
            out.cores[ccd].append({n > SLOTS_PER_CCD ? QList<int>{parked[i], parked[i + cores]} : QList<int>{parked[i]}, std::nullopt});
    }
    for (int ccd = 0; ccd < groups.size(); ++ccd) {
        if (out.parked.contains(ccd)) continue;
        // Physical core = its online SMT siblings; offline siblings (SMT off)
        // must not become extra "cores" and shift the slot numbering.
        QList<QList<int>> phys;
        for (int cpu : groups[ccd]) {
            if (std::any_of(phys.begin(), phys.end(), [&](const QList<int> &p) { return p.contains(cpu); })) continue;
            const QString t = sys.filePath(QStringLiteral("cpu%1/topology/").arg(cpu));
            QList<int> sib;
            for (int x : expandCpuList(pp::readText(t + "core_cpus_list").value_or(
                         pp::readText(t + "thread_siblings_list").value_or(QString::number(cpu)))))
                if (groups[ccd].contains(x)) sib << x;
            if (!sib.contains(cpu)) sib << cpu;
            std::sort(sib.begin(), sib.end());
            phys << sib;
        }
        for (const QList<int> &cpus : phys) {
            bool ok = false;
            int hp = pp::readText(sys.filePath(QStringLiteral("cpu%1/acpi_cppc/highest_perf").arg(cpus.first())))
                         .value_or(QString()).toInt(&ok);
            out.cores[ccd].append({cpus, ok ? std::optional(hp) : std::nullopt});
        }
    }
    return out;
}

QString ryzen::profilesDir() {
    // Same location as the standalone applet so existing profiles carry over.
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
           QStringLiteral("/ryzen-curve-optimizer/profiles");
}

// ── UI ──────────────────────────────────────────────────────────────────────

static QGroupBox *box(const QString &title, const char *objName) {
    auto *b = new QGroupBox(title);
    b->setObjectName(QString::fromLatin1(objName));
    return b;
}

RyzenTab::RyzenTab(QWidget *parent) : QWidget(parent), layout_(ryzen::detect()) {
    ccdCount_ = layout_.ccdCount ? layout_.ccdCount : CCD_FALLBACK;
    if (qEnvironmentVariableIsSet("CENTURION_RYZEN_CCDS")) ccdCount_ = qEnvironmentVariableIntValue("CENTURION_RYZEN_CCDS");  // dev only
    profilesReady_ = QDir().mkpath(ryzen::profilesDir());

    QList<int> mismatch;
    for (int c = 0; c < ccdCount_; ++c)
        if (layout_.cores.contains(c) && layout_.cores[c].size() != SLOTS_PER_CCD) mismatch << c;

    auto *root = new QVBoxLayout(this);
    trial_ = new TrialBar(QStringLiteral("ryzen/trial"));
    root->addWidget(trial_);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(6);

    if (!mismatch.isEmpty()) {
        QStringList d;
        for (int c : mismatch) d << QStringLiteral("CCD%1: %2/%3 cores").arg(c).arg(layout_.cores[c].size()).arg(SLOTS_PER_CCD);
        auto *b = box("Notice", "box_yellow");
        auto *l = new QVBoxLayout(b);
        auto *t = new QLabel("Detected a partially-populated CCD (" + d.join(", ") + "). On chips like this, the SMU's 8 fixed "
            "core slots per CCD and the OS's core numbering do not necessarily line up 1:1 — some slots may not correspond "
            "to a real core at all, and this cannot be auto-detected. Check Disable for any slot with no real core behind it.");
        t->setWordWrap(true);
        l->addWidget(t);
        root->addWidget(b);
    }

    // Layout (top → bottom):
    //   [ Profile ............ Load Save Del ]            [ Reset Curve Optimizer ]
    //   All-Core:  [ value ]                                         [Apply All-Core]
    //   Per-Core:  mode radios                               [Clear] [Apply Per-Core]
    //              ┌ CCD0 card ┐ ┌ CCD1 card ┐   (one aligned grid per CCD)
    // Reset clears *both* all-core and per-core offsets (--set-coall=0), so it
    // lives in the global row, not inside the all-core box. Both Apply buttons
    // share one width and the right edge, so they line up vertically.
    constexpr int APPLY_W = 132;

    auto *top = new QHBoxLayout;
    top->setSpacing(5);
    auto *profBox = box("Profile", "box_purple");
    auto *pl = new QHBoxLayout(profBox);
    profileCombo_ = new QComboBox;
    profileCombo_->setMinimumWidth(160);
    auto *bLoad = new QPushButton("Load"), *bSave = new QPushButton("Save"), *bDel = new QPushButton("Delete");
    bDel->setObjectName("btnDanger");
    connect(bLoad, &QPushButton::clicked, this, [this] { loadProfile(); });
    connect(bSave, &QPushButton::clicked, this, &RyzenTab::saveProfile);
    connect(bDel, &QPushButton::clicked, this, &RyzenTab::deleteProfile);
    pl->addWidget(profileCombo_, 1);
    verified_ = new QLabel;
    verified_->setVisible(false);
    pl->addWidget(verified_);
    connect(profileCombo_, &QComboBox::currentTextChanged, this, &RyzenTab::updateVerified);
    pl->addWidget(bLoad);
    pl->addWidget(bSave);
    pl->addWidget(bDel);
    top->addWidget(profBox, 1);

    auto *resetBox = box("Global", "box_grey");
    auto *rl = new QHBoxLayout(resetBox);
    auto *bReset = new QPushButton("Reset Curve Optimizer");
    bReset->setObjectName("btnDanger");
    bReset->setToolTip("Send --set-coall=0: clears the all-core offset AND every per-core offset.");
    connect(bReset, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, "Reset Curve Optimizer",
                "Clear every Curve Optimizer offset (all-core and per-core) back to 0?",
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
            applyReset();
    });
    rl->addWidget(bReset);
    top->addWidget(resetBox);
    root->addLayout(top);

    // All-core
    auto *acBox = box("All-Core Offset  (--set-coall)", "box_yellow");
    auto *al = new QHBoxLayout(acBox);
    al->addWidget(new QLabel("Offset:"));
    coall_ = new QLineEdit;
    coall_->setPlaceholderText(QStringLiteral("%1 … %2").arg(CO_MIN).arg(CO_MAX));
    coall_->setFixedWidth(96);
    coall_->setAlignment(Qt::AlignCenter);
    coall_->setValidator(new QIntValidator(CO_MIN, CO_MAX, coall_));
    connect(coall_, &QLineEdit::returnPressed, this, [this] { applyAllCore(); });
    al->addWidget(coall_);
    auto *acHint = new QLabel("Same offset on every core. Negative = undervolt.");
    acHint->setProperty("role", "muted");
    al->addWidget(acHint);
    al->addStretch();
    auto *bAll = new QPushButton("Apply All-Core");
    bAll->setObjectName("btnAccent");
    bAll->setFixedWidth(APPLY_W);
    connect(bAll, &QPushButton::clicked, this, [this] { applyAllCore(); });
    al->addWidget(bAll);
    root->addWidget(acBox);

    // Per-core
    auto *coreBox = box("Per-Core Offsets  (fixed SMU slot layout)", "box_green");
    auto *cl = new QVBoxLayout(coreBox);
    cl->setSpacing(5);
    auto *hdr = new QHBoxLayout;
    auto *modeBox = new QWidget;
    auto *ml = new QHBoxLayout(modeBox);
    ml->setContentsMargins(0, 0, 0, 0);
    auto *rPrimary = new QRadioButton(QStringLiteral("CCD0 only (%1 slots)").arg(SLOTS_PER_CCD));
    auto *rAll = rAll_ = new QRadioButton(QStringLiteral("All CCDs (%1 slots)").arg(ccdCount_ * SLOTS_PER_CCD));
    rAll->setChecked(true);
    auto *mg = new QButtonGroup(this);
    mg->addButton(rPrimary);
    mg->addButton(rAll);
    connect(rPrimary, &QRadioButton::toggled, this, [this](bool c) { if (c) setCoreMode(false); });
    connect(rAll, &QRadioButton::toggled, this, [this](bool c) { if (c) setCoreMode(true); });
    ml->addWidget(rPrimary);
    ml->addWidget(rAll);
    modeBox->setVisible(ccdCount_ >= 2);
    hdr->addWidget(modeBox);
    monLbl_ = new QLabel;
    monLbl_->setTextFormat(Qt::RichText);
    monLbl_->setToolTip("Tctl / boost from sysfs (k10temp, cpufreq). Package power needs 'VID + power'.");
    hdr->addSpacing(12);
    hdr->addWidget(monLbl_, 1);
    msrOn_ = new QCheckBox("VID + power");
    msrOn_->setToolTip("Per-core VID (MSR 0xC0010293) and RAPL core/package energy (MSR 0xC001029A/B), the same\n"
                       "registers CoreFreq reads, through the stock msr driver: one root helper per session\n"
                       "(ryzen-co-helper monitor_stream), only while this tab is visible. No CoreFreq module.");
    msrOn_->setChecked(QSettings(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
                                 QStringLiteral("/centurion/gui.ini"), QSettings::IniFormat).value("ryzen/msr_monitor", false).toBool());
    connect(msrOn_, &QCheckBox::toggled, this, [this](bool on) {
        QSettings(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/centurion/gui.ini"),
                  QSettings::IniFormat).setValue("ryzen/msr_monitor", on);
        if (on && isVisible()) startMsrStream(); else { stopMsrStream(); showMsr({}); }
    });
    hdr->addWidget(msrOn_);
    auto *bClear = new QPushButton("Clear");
    bClear->setToolTip("Empty every per-core field and untick Disable (nothing is sent)");
    auto *bCores = new QPushButton("Apply Per-Core");
    bCores->setObjectName("btnAccent");
    bCores->setFixedWidth(APPLY_W);
    connect(bClear, &QPushButton::clicked, this, &RyzenTab::clearCores);
    connect(bCores, &QPushButton::clicked, this, [this] { applyPerCore(); });
    hdr->addWidget(bClear);
    hdr->addWidget(bCores);
    cl->addLayout(hdr);
    applyButtons_ = {bAll, bReset, bCores};

    const QString accents[] = {theme::OK, theme::ACCENT, theme::PURPLE, theme::INFO};
    auto *cards = new QHBoxLayout;
    cards->setSpacing(8);
    slots_.reserve(ccdCount_ * SLOTS_PER_CCD);
    for (int ccd = 0; ccd < ccdCount_; ++ccd) {
        const QString accent = accents[ccd % 4];
        const auto detected = layout_.cores.value(ccd);

        auto *card = new QFrame;
        card->setObjectName("ccdCard");
        card->setStyleSheet(QStringLiteral("#ccdCard { background: %1; border: 1px solid %2; border-top: 2px solid %3; border-radius: 6px; }")
                                .arg(theme::BG2, theme::BORDER_SOFT, accent));
        auto *g = new QGridLayout(card);
        g->setContentsMargins(8, 6, 8, 6);
        g->setHorizontalSpacing(14);
        g->setVerticalSpacing(1);

        // Row 0: title + quick fill
        auto *title = new QLabel(ccdName(ccd));
        ccdTitles_[ccd] = title;
        title->setStyleSheet(QStringLiteral("color:%1; font-weight:700; font-size:11pt; background:transparent;").arg(accent));
        auto *fillBar = new QHBoxLayout;
        fillBar->setSpacing(6);
        auto *fill = new QLineEdit;
        fill->setPlaceholderText(QStringLiteral("%1 … %2").arg(CO_MIN).arg(CO_MAX));
        fill->setFixedWidth(84);
        fill->setAlignment(Qt::AlignCenter);
        fill->setValidator(new QIntValidator(CO_MIN, CO_MAX, fill));
        fill->setToolTip(QStringLiteral("Fill every enabled slot of CCD%1 with this offset").arg(ccd));
        auto *fb = new QPushButton("Fill");
        connect(fill, &QLineEdit::returnPressed, this, [this, ccd] { fillCcd(ccd); });
        connect(fb, &QPushButton::clicked, this, [this, ccd] { fillCcd(ccd); });
        fillEntries_[ccd] = fill;
        fillBar->addStretch();
        fillBar->addWidget(fill);
        fillBar->addWidget(fb);
        auto *temp = new QLabel;
        temp->setProperty("role", "muted");
        temp->setToolTip(QStringLiteral("k10temp Tccd%1 (CCD die temperature)").arg(ccd + 1));
        ccdTemps_[ccd] = temp;
        g->addWidget(title, 0, 0, 1, 3, Qt::AlignLeft | Qt::AlignVCenter);
        g->addWidget(temp, 0, 3, 1, 2, Qt::AlignCenter);
        g->addLayout(fillBar, 0, 5, 1, 2);

        // Row 1: column headers — same grid, so they line up with the cells.
        const char *heads[] = {"Slot", "Offset", "Disable", "CPPC", "MHz", "V", "W"};
        for (int c = 0; c < COLS; ++c) {
            auto *h = new QLabel(QString::fromLatin1(heads[c]));
            h->setProperty("role", "muted");
            h->setAlignment(Qt::AlignCenter);
            if (c == 3) h->setToolTip(QStringLiteral("Firmware preferred-core rank of each core (CPPC highest_perf).\n"
                "On X3D chips the 3D V-Cache mode (Optimizations) changes these ranks; the column always shows\n"
                "the frequency-mode ranking (recorded in frequency mode, reused in cache mode)."));
            if (c == 4) h->setToolTip("Effective clock (APERF/MPERF, /proc/cpuinfo), highest thread of the core.");
            if (c == 5) h->setToolTip("Requested core voltage from the VID (needs 'VID + power').");
            if (c == 6) h->setToolTip("RAPL core energy per second (needs 'VID + power').");
            g->addWidget(h, 1, c);
        }
        auto *line = new QFrame;
        line->setFrameShape(QFrame::HLine);
        line->setStyleSheet(QStringLiteral("color:%1;").arg(theme::BORDER_SOFT));
        g->addWidget(line, 2, 0, 1, COLS);

        for (int s = 0; s < SLOTS_PER_CCD; ++s) {
            const ryzen::PhysCore *pc = s < detected.size() ? &detected[s] : nullptr;
            QStringList cpus;
            if (pc) for (int c : pc->cpus) cpus << QString::number(c);
            const QString cpuHint = pc ? QStringLiteral("OS CPU%1 %2").arg(cpus.size() == 1 ? "" : "s", cpus.join(", "))
                                       : QStringLiteral("OS CPU mapping unknown (topology mismatch)");
            const int r = 3 + s;

            auto *id = new QLabel(QStringLiteral("S%1").arg(s));
            id->setStyleSheet(QStringLiteral("color:%1; font-weight:600; background:transparent;").arg(accent));
            id->setToolTip(QStringLiteral("Fixed SMU slot %1 of CCD%2 (hardware addressing, not the OS core id)\n").arg(s).arg(ccd) + cpuHint);
            g->addWidget(id, r, 0, Qt::AlignCenter);

            auto *e = new QLineEdit;
            e->setPlaceholderText("0");
            e->setFixedWidth(72);
            e->setFixedHeight(20);
            e->setStyleSheet("padding: 0 4px;");
            e->setAlignment(Qt::AlignCenter);
            e->setValidator(new QIntValidator(CO_MIN, CO_MAX, e));
            e->setToolTip(QStringLiteral("Curve Optimizer offset for this slot (%1..%2)").arg(CO_MIN).arg(CO_MAX));
            g->addWidget(e, r, 1, Qt::AlignCenter);

            auto *d = new QCheckBox;
            d->setToolTip("Disable this SMU slot.\nTick it if your CPU has no physical core here (e.g. a 6-core CCD uses "
                          "only 6 of the 8 SMU slots). Disabled slots are skipped entirely.");
            connect(d, &QCheckBox::toggled, e, [e](bool on) { e->setEnabled(!on); if (on) e->clear(); });
            g->addWidget(d, r, 2, Qt::AlignCenter);

            auto *cppc = new QLabel(QStringLiteral("–"));  // filled by refreshCppc()
            cppc->setMinimumWidth(52);
            cppc->setAlignment(Qt::AlignCenter);
            g->addWidget(cppc, r, 3, Qt::AlignCenter);

            auto liveCell = [&](int col) {
                auto *l = new QLabel(QStringLiteral("–"));
                l->setMinimumWidth(44);
                l->setAlignment(Qt::AlignCenter);
                l->setStyleSheet(QStringLiteral("color:%1; background:transparent;").arg(theme::FG_DIM));
                g->addWidget(l, r, col, Qt::AlignCenter);
                return l;
            };
            QLabel *mhz = liveCell(4), *volt = liveCell(5), *watt = liveCell(6);
            slots_.append({ccd, s, e, d, id, cppc, mhz, volt, watt});
        }
        // Spare width is shared evenly, so the columns spread across the
        // card instead of bunching up on the left.
        for (int c = 0; c < COLS; ++c) g->setColumnStretch(c, 1);
        g->setRowStretch(3 + SLOTS_PER_CCD, 1);
        ccdColumns_[ccd] = card;
        cards->addWidget(card, 1);
    }

    auto *gridHost = new QWidget;
    gridHost->setObjectName("ryzenGrid");
    gridHost->setStyleSheet("#ryzenGrid { background: transparent; }");
    gridHost->setLayout(cards);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(gridHost);
    coreScroll_ = scroll;
    coreGrid_ = gridHost;
    fitCoreScroll();
    cl->addWidget(scroll, 1);
    root->addWidget(coreBox, 1);

    auto *logBox = box("Output / Log", "box_grey");
    auto *ll = new QVBoxLayout(logBox);
    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setObjectName("terminal");
    log_->setMaximumBlockCount(2000);
    ll->addWidget(log_);
    log_->setMinimumHeight(54);
    log_->setMaximumHeight(90);
    root->addWidget(logBox, 0);

    monTimer_ = new QTimer(this);
    monTimer_->setInterval(MONITOR_MS);
    connect(monTimer_, &QTimer::timeout, this, &RyzenTab::monitorTick);

    reloadProfiles();
    for (int c = 0; c < ccdCount_; ++c) activeCcds_.insert(c);
    updateParked();

    log(QStringLiteral("%1 CCD(s), %2 fixed SMU slots each (%3 total).").arg(ccdCount_).arg(SLOTS_PER_CCD).arg(ccdCount_ * SLOTS_PER_CCD));
    if (!layout_.ccdCount) log(QStringLiteral("CCD topology could not be read from sysfs — assuming %1 CCDs. Verify before applying.").arg(CCD_FALLBACK), "err");
    if (!mismatch.isEmpty()) log("Partially-populated CCD detected — see the notice above about SMU slot IDs vs. OS core IDs.", "cmd");
    bool cppc = false;
    for (const auto &l : std::as_const(layout_.cores)) for (const auto &c : l) cppc |= c.highestPerf.has_value();
    for (auto it = layout_.cores.cbegin(); it != layout_.cores.cend(); ++it) {
        QStringList c;
        for (const auto &pc : it.value()) c << QStringLiteral("%1").arg(pc.cpus.first());
        log(QStringLiteral("%1 = CPUs (first thread per core): %2").arg(ccdName(it.key()), c.join(' ')), "info");
    }
    log(cppc ? "CPPC highest_perf read per core from sysfs — shown in the 'cppc' column."
             : "CPPC highest_perf not available from sysfs on this system — cppc column shows '–'.", cppc ? "info" : "cmd");
    if (!profilesReady_) log("Profile directory is not writable: " + ryzen::profilesDir(), "err");
}

void RyzenTab::log(const QString &msg, const QString &level) {
    static const QHash<QString, QPair<QString, QString>> style{
        {"info", {theme::FG_DIM, "  "}}, {"ok", {theme::OK, "OK"}}, {"err", {theme::DANGER, "!!"}}, {"cmd", {theme::WARN, ">>"}}};
    const auto [color, prefix] = style.value(level, style.value("info"));
    log_->appendHtml(QStringLiteral("<span style=\"color:%1;\">[%2]</span> <span style=\"color:%3;\">%4 %5</span>")
        .arg(theme::MUTED, QTime::currentTime().toString("HH:mm:ss"), color, prefix,
             msg.toHtmlEscaped().replace('\n', "<br>")));
}

std::pair<RyzenTab::Parse, int> RyzenTab::parse(const Slot &s) const {
    if (s.disable->isChecked()) return {Parse::Disabled, 0};
    const QString t = s.entry->text().trimmed();
    if (t.isEmpty()) return {Parse::Empty, 0};
    bool ok = false;
    const int v = t.toInt(&ok);
    if (!ok) return {Parse::Invalid, 0};
    if (v < CO_MIN || v > CO_MAX) return {Parse::Range, v};
    return {Parse::Ok, v};
}

QList<RyzenTab::Slot *> RyzenTab::activeSlots() {
    QList<Slot *> out;
    for (Slot &s : slots_) if (activeCcds_.contains(s.ccd)) out << &s;
    return out;
}

void RyzenTab::setCoreMode(bool all) {
    activeCcds_.clear();
    for (int c = 0; c < (all ? ccdCount_ : 1); ++c) activeCcds_.insert(c);
    for (auto it = ccdColumns_.cbegin(); it != ccdColumns_.cend(); ++it) {
        const bool vis = activeCcds_.contains(it.key());
        it.value()->setVisible(vis);
    }
    for (Slot &s : slots_) if (!activeCcds_.contains(s.ccd)) { s.disable->setChecked(false); s.entry->clear(); }
    log(all ? QStringLiteral("Core mode: All CCDs (%1 slots)").arg(ccdCount_ * SLOTS_PER_CCD)
            : QStringLiteral("Core mode: CCD0 Only (%1 slots)").arg(SLOTS_PER_CCD));
}

void RyzenTab::clearCores() {
    for (Slot *s : activeSlots()) { s->disable->setChecked(false); s->entry->clear(); }
    log("Per-core fields cleared.");
}

void RyzenTab::fillCcd(int ccd) {
    const QString t = fillEntries_.value(ccd)->text().trimmed();
    bool ok = false;
    const int v = t.toInt(&ok);
    if (t.isEmpty()) { QMessageBox::warning(this, "Missing Value", QStringLiteral("Enter an offset to fill CCD%1 with.").arg(ccd)); return; }
    if (!ok || v < CO_MIN || v > CO_MAX) {
        QMessageBox::warning(this, "Out of Range", QStringLiteral("Offset must be within %1..%2.").arg(CO_MIN).arg(CO_MAX));
        return;
    }
    int n = 0;
    for (Slot &s : slots_) if (s.ccd == ccd && !s.disable->isChecked()) { s.entry->setText(QString::number(v)); ++n; }
    if (n) log(QStringLiteral("CCD%1: filled %2 slot(s) with offset %3. Click 'Apply Per-Core' to send it.").arg(ccd).arg(n).arg(v));
    else log(QStringLiteral("CCD%1: every slot is disabled, nothing filled.").arg(ccd), "err");
}

// ── apply ───────────────────────────────────────────────────────────────────

void RyzenTab::setBusy(bool b) {
    busy_ = b;
    for (QPushButton *p : std::as_const(applyButtons_)) p->setEnabled(!b);
}

bool RyzenTab::applyAllCore(std::function<void()> then) {
    const QString t = coall_->text().trimmed();
    bool ok = false;
    const int v = t.toInt(&ok);
    if (t.isEmpty()) { QMessageBox::warning(this, "Missing Value", "Please enter an all-core offset value."); return false; }
    if (!ok || v < CO_MIN || v > CO_MAX) {
        QMessageBox::warning(this, "Out of Range", QStringLiteral("Offset must be within %1..%2.").arg(CO_MIN).arg(CO_MAX));
        return false;
    }
    QJsonObject params = namedProfile();
    params["value"] = v;
    runOp("set_coall", params, std::move(then));
    return true;
}

void RyzenTab::applyReset() { runOp("reset", {}); }

// Trial time-out: back to stock (0) — after the operation in flight, if there is one.
void RyzenTab::revertTrial() {
    if (busy_) { QTimer::singleShot(1500, this, &RyzenTab::revertTrial); return; }
    log("Trial over: resetting Curve Optimizer to 0.", "cmd");
    applyReset();
}

bool RyzenTab::applyPerCore(std::function<void()> then) {
    QJsonArray entries;
    QStringList problems;
    int skipped = 0, parkedSkipped = 0;
    updateParked();
    for (Slot *s : activeSlots()) {
        if (parkedNow_.contains(s->ccd)) { parkedSkipped += parse(*s).first == Parse::Ok; continue; }
        const auto [st, v] = parse(*s);
        const QString label = QStringLiteral("CCD%1/S%2").arg(s->ccd).arg(s->slot);
        switch (st) {
        case Parse::Disabled: ++skipped; break;
        case Parse::Empty: break;
        case Parse::Invalid: problems << label + ": not a number ('" + s->entry->text().trimmed() + "')"; break;
        case Parse::Range: problems << QStringLiteral("%1: %2 is outside %3..%4").arg(label).arg(v).arg(CO_MIN).arg(CO_MAX); break;
        case Parse::Ok: entries.append(QJsonObject{{"ccd", s->ccd}, {"ccx", 0}, {"core", s->slot}, {"coper", v}}); break;
        }
    }
    if (!problems.isEmpty()) {
        QMessageBox::warning(this, "Invalid Values", "Nothing was applied. Fix these first:\n\n" + problems.join('\n'));
        for (const QString &p : problems) log(p, "err");
        return false;
    }
    if (entries.isEmpty()) { QMessageBox::information(this, "No Input", "No per-core values have been entered."); return false; }
    if (parkedSkipped) log(QStringLiteral("Skipping %1 slot(s) on the parked CCD (kept in the profile).").arg(parkedSkipped), "cmd");
    if (skipped) log(QStringLiteral("Skipping %1 disabled slot(s).").arg(skipped));
    QJsonObject params = namedProfile();
    params["entries"] = entries;
    runOp("set_coper_batch", params, std::move(then));
    return true;
}

void RyzenTab::runOp(const QString &op, const QJsonObject &params, std::function<void()> then) {
    if (busy_) { log("Previous operation still running, please wait.", "err"); return; }
    log("Sending '" + op + "' via pkexec... (you may be prompted for a password)", "cmd");
    setBusy(true);
    if (op == QLatin1String("reset")) trial_->cancel();
    privileged::run(helperPath(), QJsonObject{{"op", op}, {"params", params}}, this,
        [this, then, op](const privileged::Result &r) {
            setBusy(false);
            if (r.ok() && (op == QLatin1String("set_coall") || op == QLatin1String("set_coper_batch")))
                trial_->offer([this] { revertTrial(); });
            if (!r.reached) { log(r.error, "err"); return; }
            if (r.ok()) log("Operation applied successfully.", "ok");
            else log("Operation failed: " + (r.message().isEmpty() ? QStringLiteral("see per-core results below") : r.message()), "err");
            const QJsonArray results = r.json.value("results").toArray();
            for (const auto &v : results) {
                const QJsonObject o = v.toObject();
                log(QStringLiteral("CCD%1/S%2 -> offset %3 (%4)").arg(o.value("ccd").toInt()).arg(o.value("core").toInt())
                        .arg(o.value("coper").toInt()).arg(o.value("message").toString()),
                    o.value("ok").toBool() ? "ok" : "err");
            }
            if (results.isEmpty() && r.json.contains("message")) log(r.json.value("message").toString());
            if (r.ok() && then) then();
        }, PKEXEC_TIMEOUT_MS);
}

// ── profiles ────────────────────────────────────────────────────────────────

QStringList RyzenTab::savedProfileNames() const {
    QStringList n = QDir(ryzen::profilesDir()).entryList({"*.json"}, QDir::Files, QDir::Name);
    for (QString &s : n) s.chop(5);
    return n;
}

void RyzenTab::reloadProfiles() {
    const QString prev = profileCombo_->currentText();
    profileCombo_->clear();
    profileCombo_->addItems(savedProfileNames());
    if (int i = profileCombo_->findText(prev); i >= 0) profileCombo_->setCurrentIndex(i);
    updateVerified();  // a profile saved again under the same name changed its content
}

// The badge next to the profile: the newest CPU test (Health → Curve stability) run while this profile, as it is
// saved now, was applied. Saving the profile again changes its content, so an old result no longer shows.
void RyzenTab::updateVerified() {
    QFile f(ryzen::profilesDir() + QLatin1Char('/') + profileCombo_->currentText() + QStringLiteral(".json"));
    const QJsonObject r = !profileCombo_->currentText().isEmpty() && f.open(QIODevice::ReadOnly) && f.size() < 256 * 1024
        ? cpustress::verification(cpustress::load(), profileCombo_->currentText(), cpustress::profileHash(f.readAll())) : QJsonObject();
    verified_->setVisible(!r.isEmpty());
    if (r.isEmpty()) return;
    const QString v = r.value(QStringLiteral("verdict")).toString();
    const bool pass = v == QLatin1String("pass");
    verified_->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(pass ? theme::OK : theme::DANGER,
        pass ? QStringLiteral("✓ verified") : v == QLatin1String("crashed") ? QStringLiteral("✗ crashed") : QStringLiteral("✗ failed")));
    const QString test = r.value(QStringLiteral("mode")).toString() == QLatin1String("percore") ? QStringLiteral("per-core") : QStringLiteral("all-core");
    verified_->setToolTip(QStringLiteral("CPU test (Health → Curve stability), %1 %2, %3: %4")
        .arg(test, r.value(QStringLiteral("load")).toString().toUpper(),
             QDateTime::fromSecsSinceEpoch(r.value(QStringLiteral("t")).toInteger()).toString(QStringLiteral("yyyy-MM-dd HH:mm")),
             pass ? QStringLiteral("passed") : v == QLatin1String("crashed") ? QStringLiteral("the machine went down") : QStringLiteral("failed")));
}

QJsonObject RyzenTab::currentState() {
    QJsonArray cores;
    for (Slot *s : activeSlots()) {
        const auto [st, v] = parse(*s);
        cores.append(QJsonObject{{"ccd", s->ccd}, {"ccx", 0}, {"slot", s->slot},
                                 {"coper", st == Parse::Ok ? QJsonValue(v) : QJsonValue()},
                                 {"disabled", s->disable->isChecked()}});
    }
    bool ok = false;
    const int coall = coall_->text().trimmed().toInt(&ok);
    return {{"version", "2.0.0"}, {"ccd_count", ccdCount_}, {"coall", ok ? QJsonValue(coall) : QJsonValue()}, {"cores", cores}};
}

void RyzenTab::saveProfile() {
    if (!profilesReady_) { QMessageBox::critical(this, "Save Error", "Profile directory is not writable:\n" + ryzen::profilesDir()); return; }
    bool ok = false;
    const QString name = QInputDialog::getText(this, "Save Profile", "Profile name:", QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok) return;
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9 _-]{0,63}\\z"));
    if (!re.match(name).hasMatch()) {
        QMessageBox::warning(this, "Invalid Name", "Use 1-64 characters: letters, digits, space, underscore or hyphen, starting with a letter or digit.");
        return;
    }
    const QString path = ryzen::profilesDir() + '/' + name + ".json";
    if (QFile::exists(path) && QMessageBox::question(this, "Overwrite Profile", "Profile '" + name + "' already exists. Overwrite?",
                                                     QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    QJsonObject state = currentState();
    if (QFile old(path); old.open(QIODevice::ReadOnly) && old.size() <= 256 * 1024) {
        // A parked CCD is not in the grid: overwriting must not drop its offsets.
        QJsonArray cores = state.value("cores").toArray();
        for (const auto &v : QJsonDocument::fromJson(old.readAll()).object().value("cores").toArray())
            if (v.toObject().value("ccd").toInt() >= ccdCount_) cores.append(v);
        state["cores"] = cores;
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(state).toJson()) < 0 || !f.commit()) {
        QMessageBox::critical(this, "Save Error", f.errorString());
        return;
    }
    reloadProfiles();
    profileCombo_->setCurrentText(name);
    loadedName_ = name;
    loadedState_ = currentState();
    log("Profile saved: " + path);
}

// The saved profile the grid still shows unchanged (loaded or just saved), named in the apply request so the
// curve journal records "profile:NAME" (Health → Curve stability, the CPU test's verified badge); {} after an edit.
QJsonObject RyzenTab::namedProfile() {
    if (loadedName_.isEmpty() || currentState() != loadedState_) return {};
    return {{"profile", loadedName_}};
}

bool RyzenTab::loadProfile() {
    const QString name = profileCombo_->currentText();
    if (name.isEmpty()) { QMessageBox::information(this, "No Profile", "No profile selected to load."); return false; }
    QFile f(ryzen::profilesDir() + '/' + name + ".json");
    if (!f.open(QIODevice::ReadOnly) || f.size() > 256 * 1024) { QMessageBox::critical(this, "Load Error", f.errorString()); return false; }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (!doc.isObject()) { QMessageBox::critical(this, "Load Error", pe.error ? pe.errorString() : "Profile file is not a JSON object."); return false; }
    const QJsonObject d = doc.object();
    const QJsonValue coall = d.value("coall");
    coall_->setText(coall.isDouble() ? QString::number(coall.toInt()) : QString());
    for (Slot &s : slots_) { s.disable->setChecked(false); s.entry->clear(); }
    int missing = 0;
    for (const auto &v : d.value("cores").toArray()) {
        const QJsonObject c = v.toObject();
        const int ccd = c.value("ccd").toInt(), ccx = c.value("ccx").toInt(),
                  slot = c.contains("slot") ? c.value("slot").toInt() : c.value("core").toInt();
        Slot *hit = nullptr;
        for (Slot &s : slots_) if (ccx == 0 && s.ccd == ccd && s.slot == slot) hit = &s;
        if (!hit) { ++missing; continue; }
        const bool dis = c.value("disabled").toBool();
        hit->disable->setChecked(dis);
        if (!dis && c.value("coper").isDouble()) hit->entry->setText(QString::number(c.value("coper").toInt()));
    }
    bool hidden = false;
    for (const Slot &s : slots_) hidden |= !activeCcds_.contains(s.ccd) && (s.disable->isChecked() || !s.entry->text().isEmpty());
    if (hidden) rAll_->setChecked(true);  // same set Scenes / centurion-gamemode apply
    loadedName_ = name;
    loadedState_ = currentState();
    log("Profile loaded: " + name);
    if (missing) log(QStringLiteral("%1 slot(s) in the profile do not exist on this topology (%2 CCD(s)) and were ignored.")
                         .arg(missing).arg(ccdCount_), "err");
    return true;
}

void RyzenTab::deleteProfile() {
    const QString name = profileCombo_->currentText();
    if (name.isEmpty() || QMessageBox::question(this, "Delete Profile", "Delete profile '" + name + "'?",
                                                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    if (!QFile::remove(ryzen::profilesDir() + '/' + name + ".json")) { QMessageBox::critical(this, "Delete Error", "Could not delete the profile."); return; }
    reloadProfiles();
    log("Profile deleted: " + name);
}

bool RyzenTab::applyNamedProfile(const QString &name) {
    if (busy_) { log("Previous operation still running, please wait.", "err"); return false; }
    const int i = profileCombo_->findText(name);
    if (i < 0) { reloadProfiles(); QMessageBox::warning(this, "No Profile", "Profile '" + name + "' no longer exists."); return false; }
    profileCombo_->setCurrentIndex(i);
    if (!loadProfile()) return false;

    bool anyCore = false;
    for (Slot *s : activeSlots()) anyCore |= parse(*s).first == Parse::Ok;
    const bool hasCoall = !coall_->text().trimmed().isEmpty();
    // Chained, not back-to-back: the Python version fired per-core while the
    // all-core pkexec call was still running, so it was rejected as busy
    // and a profile with both parts only ever got its all-core half applied.
    if (hasCoall) return applyAllCore(anyCore ? std::function<void()>([this] { applyPerCore(); }) : std::function<void()>());
    if (anyCore) return applyPerCore();
    log("Profile '" + name + "' contains no offsets to apply.", "err");
    return false;
}

// ── parked CCD (Optimizations → Park a CCD) ─────────────────────────────────

void RyzenTab::fitCoreScroll() {
    if (!coreScroll_ || !coreGrid_) return;
    // A tab that has not been shown yet keeps the sizes its layouts and layout items cached
    // before the style changed (they are only refreshed when a layout is activated); drop
    // them so the measurement is made in the current style.
    for (QWidget *w : coreGrid_->findChildren<QWidget *>()) w->updateGeometry();
    for (QLayout *l : coreGrid_->findChildren<QLayout *>()) l->invalidate();
    coreScroll_->setMinimumHeight(std::min(coreGrid_->sizeHint().height(), 260));
}

// The height above is measured in the theme's metrics; at start-up the style sheet is applied
// after the window is built (theme.h), so measure again when it arrives. Queued: the children
// get their own StyleChange in no particular order, and only then are the layouts' cached sizes gone.
void RyzenTab::changeEvent(QEvent *e) {
    QWidget::changeEvent(e);
    if (e->type() == QEvent::StyleChange) QMetaObject::invokeMethod(this, &RyzenTab::fitCoreScroll, Qt::QueuedConnection);
}

void RyzenTab::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    updateVerified();
    updateParked();
    monitorTick();
    monTimer_->start();
    if (msrOn_->isChecked()) startMsrStream();
}

void RyzenTab::hideEvent(QHideEvent *e) {
    QWidget::hideEvent(e);
    monTimer_->stop();
    stopMsrStream();  // no root sampling while the tab (or window) is hidden
}

RyzenTab::~RyzenTab() { stopMsrStream(); }

void RyzenTab::updateParked() {
    // The layout was read once at construction; hot-plug (park, SMT) or an early start can leave
    // it stale. Re-read it; the CPU->CCD mapping comes from L3 ids only, never from CPPC.
    {
        const ryzen::Layout fresh = ryzen::detect();
        if (fresh.ccdCount == layout_.ccdCount) layout_ = fresh;
    }
    const QList<int> parked = ryzen::parkedCpus();
    parkedNow_.clear();
    for (int ccd = 0; ccd < ccdCount_; ++ccd) {
        QList<int> cpus;
        for (const auto &pc : layout_.cores.value(ccd)) cpus << pc.cpus;
        const bool isParked = !parked.isEmpty() && !cpus.isEmpty()
                              && std::all_of(cpus.begin(), cpus.end(), [&](int c) { return parked.contains(c); });
        if (isParked) parkedNow_.insert(ccd);
        if (QWidget *card = ccdColumns_.value(ccd)) {
            card->setEnabled(!isParked);
            card->setToolTip(isParked ? QStringLiteral("CCD%1 is parked (offline) by Optimizations. Its offsets are kept in the "
                                                       "profile but not sent until it is back online.").arg(ccd) : QString());
        }
        if (QLabel *t = ccdTitles_.value(ccd))
            t->setText(isParked ? ccdName(ccd) + QStringLiteral("  · parked") : ccdName(ccd));
        for (const Slot &s : std::as_const(slots_)) {
            if (s.ccd != ccd) continue;
            const auto cores = layout_.cores.value(ccd);
            const ryzen::PhysCore *pc = s.slot < cores.size() ? &cores[s.slot] : nullptr;
            QStringList cpus;
            if (pc) for (int c : pc->cpus) cpus << QString::number(c);
            s.row->setToolTip(QStringLiteral("Fixed SMU slot %1 of CCD%2 (hardware addressing, not the OS core id)\n").arg(s.slot).arg(ccd) +
                              (pc ? QStringLiteral("OS CPU%1 %2").arg(cpus.size() == 1 ? "" : "s", cpus.join(", "))
                                  : QStringLiteral("OS CPU mapping unknown (topology mismatch)")));
        }
    }
    refreshCppc();
}

QString RyzenTab::ccdName(int ccd) const {
    // Name the die by hardware identity: on X3D parts the V-Cache CCD has the larger L3.
    const int kib = layout_.l3Kib.value(ccd);
    int lo = INT_MAX, hi = 0;
    for (int v : layout_.l3Kib) { lo = std::min(lo, v); hi = std::max(hi, v); }
    if (!kib) return QStringLiteral("CCD%1").arg(ccd);
    const QString size = QStringLiteral("%1 MB L3").arg(kib / 1024);
    return layout_.l3Kib.size() >= 2 && hi > lo && kib == hi ? QStringLiteral("CCD%1 · V-Cache · %2").arg(ccd).arg(size)
                                                            : QStringLiteral("CCD%1 · %2").arg(ccd).arg(size);
}

// Where the per-core rank snapshots (below) are kept. Keyed by BIOS version + CPU model: a
// firmware update or another CPU can re-rank the cores. Slot keys ("ccd.slot") survive SMT
// on/off and CPU renumbering.
static QString guiIni() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/centurion/gui.ini");
}
static QString cppcBaseKey() {
    static const QString key = [] {
        QString k = sysinfo::dmi(QStringLiteral("bios_version")).value_or(QStringLiteral("unknown")) + '_' +
                    sysinfo::cpuModel().value_or(QStringLiteral("cpu"));
        k.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
        return QStringLiteral("ryzen/cppc_freq/") + k;
    }();
    return key;
}

// True if `snap` and `live` rank the cores of one CCD the same way. A mode switch lifts or lowers a
// CCD's ranks and may clip the best ones to one value (ties), but never swaps two cores: a strict
// order in one with the opposite strict order in the other means the record is from another
// firmware state, and it is dropped.
static bool sameOrder(const QList<std::optional<int>> &live, const QList<std::optional<int>> &snap) {
    if (live.size() != snap.size()) return false;
    for (int i = 0; i < live.size(); ++i)
        for (int j = 0; j < live.size(); ++j)
            if (live[i] && live[j] && snap[i] && snap[j] && *live[i] > *live[j] && *snap[i] < *snap[j]) return false;
    return true;
}

void RyzenTab::refreshCppc() {
    // highest_perf is live: amd_x3d_mode, prefcore and firmware rewrite it at runtime, so it is
    // re-read on every refresh instead of being trusted from construction time.
    //
    // X3D parts: amd_x3d_vcache's mode decides which CCD firmware ranks first. "frequency" is
    // the firmware's own ranking (frequency CCD first, as on a part without the driver);
    // "cache" re-ranks the V-Cache CCD above it. How (offset, clip, re-scale) is firmware's
    // business and differs between parts, so nothing is computed from the numbers. The column
    // always shows the frequency-mode ranking: read live in frequency mode (and recorded once it
    // is stable for two reads), shown from that record in cache mode. Without the driver, on
    // non-X3D parts or before the first record, the live value is shown as read.
    QString mode;
    const QDir drv(QStringLiteral("/sys/bus/platform/drivers/amd_x3d_vcache"));
    for (const QString &d : drv.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (auto m = pp::readText(drv.filePath(d) + "/amd_x3d_mode")) { mode = m->trimmed(); break; }

    int lo = INT_MAX, hi = 0;
    for (int v : std::as_const(layout_.l3Kib)) { lo = std::min(lo, v); hi = std::max(hi, v); }
    const bool x3d = layout_.l3Kib.size() >= 2 && hi > lo;
    const bool known = x3d && (mode == QLatin1String("cache") || mode == QLatin1String("frequency"));
    const bool reference = known && mode == QLatin1String("frequency");  // live = the ranking we show
    const bool remap = known && mode == QLatin1String("cache");          // live differs: use the record

    QSettings st(guiIni(), QSettings::IniFormat);
    QJsonObject base = known ? QJsonDocument::fromJson(st.value(cppcBaseKey()).toByteArray()).object() : QJsonObject();
    bool dirty = false;
    const bool modeSteady = !mode.isEmpty() && mode == cppcPrevMode_;
    auto complete = [](const QList<std::optional<int>> &l) {
        return !l.isEmpty() && std::all_of(l.begin(), l.end(), [](const auto &v) { return v.has_value(); });
    };

    // Pass 1: live ranks of every CCD (+ the frequency-mode record).
    QMap<int, QList<std::optional<int>>> liveOf, snapOf;
    for (int ccd = 0; ccd < ccdCount_; ++ccd) {
        auto &cores = layout_.cores[ccd];
        QList<std::optional<int>> live, snap;
        for (int i = 0; i < cores.size(); ++i) {
            auto &c = cores[i];
            if (parkedNow_.contains(ccd)) c.highestPerf.reset();
            else {
                bool ok = false;
                const int hp = pp::readText(QStringLiteral("/sys/devices/system/cpu/cpu%1/acpi_cppc/highest_perf").arg(c.cpus.first()))
                                   .value_or(QString()).toInt(&ok);
                c.highestPerf = ok ? std::optional(hp) : std::nullopt;
            }
            live << c.highestPerf;
            const QJsonValue b = base.value(QStringLiteral("%1.%2").arg(ccd).arg(i));
            snap << (b.isDouble() ? std::optional(b.toInt()) : std::nullopt);
        }
        if (reference && complete(live) && modeSteady && live == cppcPrevLive_.value(ccd)) {
            // Frequency mode and unchanged since the last read: record it (breaks cache-mode ties).
            for (int i = 0; i < live.size(); ++i)
                if (snap[i] != live[i]) { base[QStringLiteral("%1.%2").arg(ccd).arg(i)] = *live[i]; dirty = true; }
            snap = live;
        }
        cppcPrevLive_[ccd] = live;
        liveOf[ccd] = live;
        snapOf[ccd] = snap;
    }
    // Cache mode reverses which CCD firmware ranks first. The frequency-mode view is that reversed
    // back: each CCD keeps its own core order and takes the other CCD's set of rank values (its
    // k-th best core gets the other CCD's k-th best value). Needs both CCDs read, same core count.
    const bool swap = remap && ccdCount_ == 2 && complete(liveOf.value(0)) && complete(liveOf.value(1))
                      && liveOf.value(0).size() == liveOf.value(1).size();

    // Pass 2: what each slot shows.
    for (int ccd = 0; ccd < ccdCount_; ++ccd) {
        const QList<std::optional<int>> live = liveOf.value(ccd);
        QList<std::optional<int>> snap = snapOf.value(ccd);
        bool useSnap = remap && complete(snap);
        if (useSnap && !sameOrder(live, snap)) {  // stale record (firmware changed): forget it
            for (int i = 0; i < snap.size(); ++i) base.remove(QStringLiteral("%1.%2").arg(ccd).arg(i));
            dirty = true;
            useSnap = false;
        }
        QList<std::optional<int>> shown = useSnap ? snap : live;
        QSet<int> tied;  // cache mode clipped these to one rank: their order is not known yet
        if (!useSnap && swap) {
            QList<int> vals;
            for (const auto &v : liveOf.value(1 - ccd)) vals << *v;
            std::sort(vals.begin(), vals.end(), std::greater<>());
            QList<int> order(live.size());
            std::iota(order.begin(), order.end(), 0);
            std::stable_sort(order.begin(), order.end(), [&](int x, int y) { return *live[x] > *live[y]; });
            for (int k = 0; k < order.size();) {
                int e = k;
                while (e + 1 < order.size() && *live[order[e + 1]] == *live[order[k]]) ++e;
                for (int j = k; j <= e; ++j) {
                    shown[order[j]] = vals[k];
                    if (e > k) tied.insert(order[j]);
                }
                k = e + 1;
            }
        }
        const bool unresolved = remap && !useSnap && !swap;  // cache mode, nothing to map from: live

        QList<int> perf;
        for (const auto &x : shown) if (x) perf << *x;
        std::sort(perf.begin(), perf.end(), std::greater<>());
        // Stars only where the firmware actually ranks (prefcore off: every core reads the same).
        const bool ranked = perf.size() >= 2 && perf.first() != perf.last();
        const int bestCut = ranked ? perf[1] : INT_MAX;
        for (const Slot &s : std::as_const(slots_)) {
            if (s.ccd != ccd) continue;
            const std::optional<int> v = s.slot < shown.size() ? shown[s.slot] : std::nullopt;
            const std::optional<int> lv = s.slot < live.size() ? live[s.slot] : std::nullopt;
            const bool best = v && *v >= bestCut;
            const bool isTied = tied.contains(s.slot);
            const QString mark = !v ? QString() : unresolved ? QStringLiteral("↑") : isTied ? QStringLiteral("=") : QString();
            s.cppc->setText(v ? mark + QString::number(*v) + (best ? QStringLiteral(" ★") : QString()) : QStringLiteral("–"));
            s.cppc->setStyleSheet(QStringLiteral("color:%1;%2 background:transparent;")
                                      .arg(v ? (best ? theme::WARN : theme::FG_DIM) : theme::MUTED, best ? " font-weight:700;" : ""));
            QString tip = QStringLiteral("Firmware preferred-core rank (CPPC highest_perf)") +
                          (v ? ": " + QString::number(*v) : QStringLiteral(" (not available)"));
            if (remap && v && lv)
                tip += QStringLiteral("\nShown as in frequency mode (%1). Live value in cache mode: %2.")
                           .arg(useSnap ? QStringLiteral("recorded") : QStringLiteral("CCD rank sets exchanged")).arg(*lv);
            if (isTied)
                tip += "\n= cache mode gives these cores the same rank, so their order is not known yet.\n"
                       "One visit to frequency mode (Optimizations) records it.";
            if (unresolved && v)
                tip += QStringLiteral("\n↑ = live cache-mode value (the other CCD is not readable, e.g. parked).");
            s.cppc->setToolTip(tip + (ranked ? QStringLiteral("\n★ marks the two best on this CCD.") : QStringLiteral("\nAll cores report the same rank.")));
        }
    }
    if (dirty) st.setValue(cppcBaseKey(), QJsonDocument(base).toJson(QJsonDocument::Compact));

    if (!mode.isEmpty() && mode != x3dMode_) {
        if (log_ && x3d) log(mode == QLatin1String("cache")
            ? QStringLiteral("3D V-Cache mode: cache — firmware ranks the V-Cache CCD first. The CPPC column shows it reversed "
                             "back to the frequency-mode view (each CCD keeps its core order and takes the other CCD's ranks).")
            : QStringLiteral("3D V-Cache mode: %1 — CPPC column shows the live ranking and records it for cache mode.").arg(mode), "cmd");
        x3dMode_ = mode;
    }
    cppcPrevMode_ = mode;
}

// ── live monitor ────────────────────────────────────────────────────────────

static QString findHwmon(const QString &name) {
    static QString cached;
    if (!cached.isEmpty() && pp::readText(cached + "/name") == name) return cached;
    const QDir d(QStringLiteral("/sys/class/hwmon"));
    for (const QString &h : d.entryList({"hwmon*"}, QDir::Dirs))
        if (pp::readText(d.filePath(h) + "/name") == name) return cached = d.filePath(h);
    return {};
}

void RyzenTab::monitorTick() {
    if (!isVisible()) return;
    // Effective clock per CPU: /proc/cpuinfo "cpu MHz" is the kernel's APERF/MPERF sample on
    // x86 (what CoreFreq computes from the same counters), readable without root.
    QHash<int, double> mhz;
    {
        QFile f(QStringLiteral("/proc/cpuinfo"));
        if (f.open(QIODevice::ReadOnly)) {
            int cpu = -1;
            for (const QByteArray &line : f.read(1 << 20).split('\n')) {
                if (line.startsWith("processor")) cpu = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                else if (line.startsWith("cpu MHz") && cpu >= 0) mhz[cpu] = line.mid(line.indexOf(':') + 1).trimmed().toDouble();
            }
        }
    }
    double maxMhz = 0;
    for (const Slot &s : std::as_const(slots_)) {
        const auto cores = layout_.cores.value(s.ccd);
        const ryzen::PhysCore *pc = s.slot < cores.size() && !parkedNow_.contains(s.ccd) ? &cores[s.slot] : nullptr;
        double m = 0;
        if (pc) for (int c : pc->cpus) m = std::max(m, mhz.value(c));
        maxMhz = std::max(maxMhz, m);
        s.mhz->setText(m > 0 ? QString::number(m, 'f', 0) : QStringLiteral("–"));
    }

    // Temperatures: k10temp Tctl + one Tccd per die (CoreFreq reads the same SMU sensors over SMN).
    QStringList sum;
    for (QLabel *l : std::as_const(ccdTemps_)) l->clear();
    if (const QString hw = findHwmon(QStringLiteral("k10temp")); !hw.isEmpty()) {
        for (const QString &lf : QDir(hw).entryList({"temp*_label"}, QDir::Files)) {
            const QString label = pp::readText(hw + '/' + lf).value_or(QString());
            bool ok = false;
            const double t = pp::readText(hw + '/' + QString(lf).replace("_label", "_input")).value_or(QString()).toInt(&ok) / 1000.0;
            if (!ok) continue;
            if (label == QLatin1String("Tctl")) sum.prepend(QStringLiteral("Tctl %1 °C").arg(t, 0, 'f', 0));
            else if (label.startsWith(QLatin1String("Tccd")))
                if (QLabel *l = ccdTemps_.value(label.mid(4).toInt() - 1)) l->setText(QStringLiteral("%1 °C").arg(t, 0, 'f', 0));
        }
    }
    // Boost (CPB): global knob, or per policy on drivers that only expose that.
    auto boost = pp::readText(QStringLiteral("/sys/devices/system/cpu/cpufreq/boost"));
    if (!boost) boost = pp::readText(QStringLiteral("/sys/devices/system/cpu/cpufreq/policy0/boost"));
    if (boost) sum << QStringLiteral("Boost <span style='color:%1'>%2</span>").arg(*boost == "1" ? theme::OK : theme::WARN, *boost == "1" ? "on" : "off");
    if (maxMhz > 0) sum << QStringLiteral("max %1 MHz").arg(maxMhz, 0, 'f', 0);
    tempText_ = sum.join(QStringLiteral(" · "));
    showMsr(msr_);
    refreshCppc();
}

void RyzenTab::showMsr(const QJsonObject &s) {
    // monitorTick() re-shows the last sample every second: count limit time only for a new helper sample.
    const bool fresh = &s != &msr_;
    msr_ = s;
    const QJsonObject cpus = s.value("cpus").toObject();
    QHash<int, double> ccdW;
    for (const Slot &sl : std::as_const(slots_)) {
        const auto cores = layout_.cores.value(sl.ccd);
        const ryzen::PhysCore *pc = sl.slot < cores.size() && !parkedNow_.contains(sl.ccd) ? &cores[sl.slot] : nullptr;
        double v = 0;
        std::optional<double> w;
        if (pc) {
            for (int c : pc->cpus) v = std::max(v, cpus.value(QString::number(c)).toObject().value("v").toDouble());
            // Core energy is one counter per core: every sibling reads the same value.
            const QJsonValue jw = cpus.value(QString::number(pc->cpus.first())).toObject().value("w");
            if (jw.isDouble()) w = jw.toDouble();
        }
        if (w) ccdW[sl.ccd] += *w;
        sl.volt->setText(v > 0 ? QString::number(v, 'f', 3) : QStringLiteral("–"));
        sl.watt->setText(w ? QString::number(*w, 'f', 2) : QStringLiteral("–"));
    }
    QString text = tempText_;
    if (s.value("pkg_w").isDouble())
        text += (text.isEmpty() ? QString() : QStringLiteral(" · ")) + QStringLiteral("Package %1 W").arg(s.value("pkg_w").toDouble(), 0, 'f', 1);
    for (auto it = ccdW.cbegin(); it != ccdW.cend(); ++it)
        text += QStringLiteral(" · CCD%1 cores %2 W").arg(it.key()).arg(it.value(), 0, 'f', 1);
    if (s.contains("error")) text += QStringLiteral(" · <span style='color:%1'>%2</span>").arg(theme::DANGER, s.value("error").toString().toHtmlEscaped());
    // Limit reasons (SMU PM table through ryzen_smu): which limit is holding the clocks right now, and
    // how many seconds each one has held them since the monitor started.
    const QJsonObject pm = s.value("pm").toObject();
    if (!pm.isEmpty()) {
        QStringList parts, binding;
        for (const QJsonValue &v : pm.value("limits").toArray()) {
            const QJsonObject l = v.toObject();
            const QString n = l.value("name").toString();
            const double pct = l.value("pct").toDouble();
            const bool at = pct >= LIMIT_PCT;
            if (at && fresh) limitSecs_[n] += MONITOR_MS / 1000.0;
            if (at) binding << n;
            parts << QStringLiteral("<span style='color:%1'>%2 %3/%4 %5</span>")
                         .arg(at ? theme::WARN : theme::FG_DIM, n.toHtmlEscaped())
                         .arg(l.value("value").toDouble(), 0, 'f', 0).arg(l.value("limit").toDouble(), 0, 'f', 0)
                         .arg(l.value("unit").toString());
        }
        if (!parts.isEmpty()) {
            text += QStringLiteral("<br>Limits: ") + parts.join(QStringLiteral(" · "));
            text += binding.isEmpty() ? QStringLiteral(" — <span style='color:%1'>none binding</span>").arg(theme::OK)
                                      : QStringLiteral(" — <b style='color:%1'>held by %2</b>").arg(theme::WARN, binding.join('+'));
            QStringList held;
            for (auto it = limitSecs_.cbegin(); it != limitSecs_.cend(); ++it)
                if (it.value() >= 1) held << QStringLiteral("%1 %2 s").arg(it.key()).arg(it.value(), 0, 'f', 0);
            if (!held.isEmpty()) text += QStringLiteral(" · at limit since start: ") + held.join(QStringLiteral(", "));
        } else if (pm.contains("error")) {
            text += QStringLiteral("<br><span style='color:%1'>Limits: %2%3</span>").arg(theme::MUTED, pm.value("error").toString().toHtmlEscaped(),
                        pm.contains("version") ? QStringLiteral(" (PM table %1)").arg(pm.value("version").toString()) : QString());
        }
    }
    monLbl_->setText(text);
}

void RyzenTab::startMsrStream() {
    if (msrStream_ || qEnvironmentVariableIsSet("CENTURION_PGO_TRAIN")) return;
    QString pkexec;
    for (const char *c : {"/usr/bin/pkexec", "/bin/pkexec"})
        if (QFileInfo(QString::fromLatin1(c)).isFile()) { pkexec = QString::fromLatin1(c); break; }
    if (pkexec.isEmpty() || !QFileInfo(helperPath()).isFile()) { showMsr({{"error", "pkexec or ryzen-co-helper not found"}}); return; }
    // Parented to the app, not the tab: once pkexec has exec'd the root helper we cannot kill
    // it (EPERM), and a QProcess destructor would block the GUI waiting for it. Closing stdin
    // makes it exit within one interval.
    auto *p = new QProcess(QCoreApplication::instance());
    msrStream_ = p;
    msrGotSample_ = false;
    limitSecs_.clear();
    p->setProgram(pkexec);
    p->setArguments({helperPath()});
    p->setProcessChannelMode(QProcess::SeparateChannels);
    connect(p, &QProcess::readyReadStandardOutput, this, [this, p] {
        while (p->canReadLine()) {
            const QJsonObject o = QJsonDocument::fromJson(p->readLine(1 << 20)).object();
            if (o.isEmpty() || o.value("stream_end").toBool()) continue;
            if (!o.value("ok").toBool()) {
                const QString err = o.value("error").toString();
                showMsr({{"error", err.startsWith(QLatin1String("unknown op")) ? QStringLiteral("installed ryzen-co-helper predates the monitor — reinstall")
                                                                              : err}});
                continue;
            }
            msrGotSample_ = true;
            showMsr(o);
        }
        if (p->bytesAvailable() > (1 << 20)) p->readAll();  // runaway without newlines: drop
    });
    // Context is `p` (it outlives the tab); `self` guards every touch of the tab.
    QPointer<RyzenTab> self(this);
    connect(p, &QProcess::finished, p, [self, this, p](int code, QProcess::ExitStatus) {
        p->deleteLater();
        if (!self || msrStream_ != p) return;  // tab gone, or stopped on purpose
        msrStream_ = nullptr;
        if (!msrGotSample_ && (code == 126 || code == 127)) {
            showMsr({{"error", code == 126 ? QStringLiteral("authorization dismissed") : QStringLiteral("polkit did not authorize the monitor")}});
            return;  // no re-prompt loop; untick and tick again to retry
        }
        // The helper's 4 h ceiling (or a crash after samples): carry on if still wanted.
        if (msrGotSample_ && msrOn_->isChecked() && isVisible())
            QTimer::singleShot(MONITOR_MS, this, [this] { if (msrOn_->isChecked() && isVisible() && !msrStream_) startMsrStream(); });
    });
    connect(p, &QProcess::errorOccurred, p, [self, this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        if (self && msrStream_ == p) msrStream_ = nullptr;
        p->deleteLater();
    });
    p->start();
    p->write(QJsonDocument(QJsonObject{{"op", "monitor_stream"}, {"params", QJsonObject{{"interval_ms", MONITOR_MS}, {"limits", true}}}})
                 .toJson(QJsonDocument::Compact) + '\n');
}

void RyzenTab::stopMsrStream() {
    if (!msrStream_) return;
    QProcess *p = msrStream_;
    msrStream_ = nullptr;
    disconnect(p, &QProcess::readyReadStandardOutput, this, nullptr);
    p->closeWriteChannel();  // helper sees EOF and exits; finished() deletes it
    if (p->state() == QProcess::Starting) p->kill();
}
