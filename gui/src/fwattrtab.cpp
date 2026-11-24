#include <algorithm>
#include <QFontMetrics>
#include <memory>
#include "fwattrtab.h"
#include "systemdiagnostics.h"
#include "diagreportview.h"
#include <QDateTime>
#include <QThread>
#include <QFileInfo>
#include <QStandardPaths>
#include <QSaveFile>
#include <QShowEvent>
#include "platformprofile.h"
#include "prewrite.h"
#include "privileged.h"
#include "systools.h"
#include "theme.h"
#include "experimentalui.h"

#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLineEdit>
#include <QTableWidget>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMap>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

// CENTURION_FWATTR_BASE: dev-only override for rendering against a fake tree. The
// helper still refuses any path outside /sys/class/firmware-attributes.
static const QString BASE = qEnvironmentVariableIsSet("CENTURION_FWATTR_BASE")
    ? qEnvironmentVariable("CENTURION_FWATTR_BASE") : QStringLiteral("/sys/class/firmware-attributes");

static QString helperPath() { return privileged::helperPath(QStringLiteral("fwattr-helper")); }
static QString gpuHelperPath() { return privileged::helperPath(QStringLiteral("legion-gpu-helper")); }

// Attributes lenovo-wmi-other lists with min=max=step=0: the kernel rejects any
// sysfs write to them (EINVAL). They are written through \_SB.GZFD.WMAE instead.
// Ranges: Dynamic Boost ceiling/floor 0..25 W (as Legion Space shows them);
// cTGP 5..150 until the helper reports this GPU model's own ceiling.
// Lower bound 1, never 0: writing 0 makes the firmware treat the feature as off
// and the kernel drops the attribute from sysfs (only a Windows power-profile
// reset brought it back). The helpers refuse 0 as well.
struct WmiKnob { const char *attr, *key; int lo, hi; };
static const WmiKnob WMI_KNOBS[] = {
    {"gpu_nv_ctgp", "ctgp", 5, 150},
    {"gpu_nv_ppab", "boost_up", 1, 25},
    {"gpu_nv_cpu_boost", "boost_down", 1, 25},
};
// Shown, never written (not even by a scene): dgpu_enable is the same firmware call as
// GameZone NotifyDGPUStatus — a write runs \_SB.GZFD.HTPL, which ejects the dGPU from the
// bus or bus-checks it back (a hot-unplug, not a setting); dgpu_didvid is an identifier;
// gpu_mode is computed by the firmware from the GPU limits (and its read pokes the EC).
static bool readOnlyAttr(const QString &n) {
    return n == QLatin1String("dgpu_enable") || n == QLatin1String("dgpu_didvid") || n == QLatin1String("gpu_mode");
}

// Sysfs-backed limits that legion-gpu-helper can also reach over WMAE; used
// only when the attribute has vanished from sysfs AND the WMAE read-back was
// seen to match sysfs on this machine (recorded in the range cache).
static const std::pair<const char *, const char *> WMAE_FALLBACK[] = {
    {"ppt_pl1_spl", "spl"}, {"ppt_pl2_sppt", "sppt"}, {"ppt_pl3_fppt", "fppt"}, {"cpu_temp", "cpu_temp"},
};

/// Last seen firmware ranges, so a row can still be drawn after its attribute
/// disappeared from sysfs. ~/.cache/centurion/fwattr-ranges.json
static QString rangeCacheFile() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/centurion/fwattr-ranges.json");
}
static QJsonObject readRangeCache() {
    QFile f(rangeCacheFile());
    return f.open(QIODevice::ReadOnly) && f.size() < 256 * 1024 ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
}
static void writeRangeCache(const QJsonObject &o) {
    QDir().mkpath(QFileInfo(rangeCacheFile()).absolutePath());
    QSaveFile f(rangeCacheFile());
    if (f.open(QIODevice::WriteOnly)) { f.write(QJsonDocument(o).toJson()); f.commit(); }
}
/// The GameZone "other method" WMI interface (WMAE) is present.
static bool wmaeAvailable() {
    return !QDir(QStringLiteral("/sys/bus/wmi/devices")).entryList({"DC2A8805-3A8C-41BA-A6F7-092E0089CD3B*"},
                                                                  QDir::Dirs | QDir::System | QDir::NoDotAndDotDot).isEmpty();
}

bool FwattrTab::present() {
    const QDir base(BASE);
    for (const QString &dev : base.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System))
        if (!QDir(base.filePath(dev) + QStringLiteral("/attributes")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty())
            return true;
    return wmaeAvailable();
}

static std::optional<int> readInt(const QString &p) {
    auto s = pp::readText(p);
    bool ok = false;
    int v = s ? s->toInt(&ok) : 0;
    return ok ? std::optional(v) : std::nullopt;
}

QList<FwAttr> FwattrTab::discover() {
    QList<FwAttr> out;
    const QDir base(BASE);
    for (const QString &dev : base.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System, QDir::Name)) {
        const QDir attrs(base.filePath(dev) + QStringLiteral("/attributes"));
        for (const QString &name : attrs.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            const QString a = attrs.filePath(name) + '/';
            auto cur = readInt(a + "current_value"), def = readInt(a + "default_value"),
                 mn = readInt(a + "min_value"), mx = readInt(a + "max_value"), st = readInt(a + "scalar_increment");
            auto disp = pp::readText(a + "display_name");
            if (!cur || !def || !mn || !mx || !st || !disp) continue;
            FwAttr f;
            f.device = dev; f.name = name; f.path = a + "current_value"; f.displayName = *disp;
            f.capturedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
            f.current = *cur; f.def = *def; f.min = *mn; f.max = *mx; f.step = std::max(*st, 1);
            // Some BIOS revisions report min=max=step=0 (gpu_nv_ctgp etc.): no usable range.
            f.ranged = !(*mn == 0 && *mx == 0 && *st == 0);
            if (readOnlyAttr(name)) { f.readOnly = true; f.ranged = false; out.append(f); continue; }
            if (!f.ranged)
                for (const WmiKnob &k : WMI_KNOBS)
                    if (name == QLatin1String(k.attr)) { f.wmiKey = QString::fromLatin1(k.key); f.wmiMin = k.lo; f.wmiMax = k.hi; }
            out.append(f);
        }
    }

    // Attributes missing from sysfs that are still reachable over WMAE.
    if (!wmaeAvailable()) return out;
    auto have = [&](const char *attr) { return std::any_of(out.cbegin(), out.cend(), [&](const FwAttr &a) { return a.name == QLatin1String(attr); }); };
    const QJsonObject cache = readRangeCache();
    const QString dev = out.isEmpty() ? cache.value("_device").toString(QStringLiteral("lenovo-wmi-other-0")) : out.first().device;
    for (const WmiKnob &k : WMI_KNOBS) {
        if (have(k.attr)) continue;
        const QJsonObject c = cache.value(QLatin1String(k.attr)).toObject();
        FwAttr f;
        f.device = dev; f.name = QString::fromLatin1(k.attr);
        f.displayName = c.value("display").toString(f.name) + QStringLiteral("  (missing from sysfs — via WMI)");
        f.ranged = false; f.wmiKey = QString::fromLatin1(k.key); f.wmiMin = k.lo; f.wmiMax = k.hi;
        out.append(f);
    }
    for (const auto &[attr, key] : WMAE_FALLBACK) {
        const QJsonObject c = cache.value(QLatin1String(attr)).toObject();
        if (have(attr) || !c.value("wmae_verified").toBool()) continue;
        FwAttr f;
        f.device = dev; f.name = QString::fromLatin1(attr);
        f.displayName = c.value("display").toString(f.name) + QStringLiteral("  (missing from sysfs — via WMI)");
        f.ranged = false; f.wmiKey = QString::fromLatin1(key);
        f.wmiMin = std::max(1, c.value("min").toInt(1)); f.wmiMax = std::max(f.wmiMin, c.value("max").toInt(f.wmiMin));
        f.def = c.value("default").toInt(f.wmiMin);
        out.append(f);
    }
    return out;
}

static QString category(const QString &n) {
    if (n.startsWith("cpu") || n.startsWith("ppt_cpu") || n.startsWith("ppt_pl")) return QStringLiteral("CPU");
    if (n.startsWith("gpu") || n.startsWith("dgpu")) return QStringLiteral("GPU");
    return QStringLiteral("Other");
}

FwattrTab::FwattrTab(QWidget *parent) : QWidget(parent) {
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 6, 8, 6);
    lay->setSpacing(6);

    banner_ = new QLabel;
    banner_->setWordWrap(true);
    banner_->setStyleSheet(theme::banner(theme::ACCENT) + QStringLiteral(" padding: 6px 10px;"));
    banner_->hide();
    lay->addWidget(banner_);

    // Firmware CPU OC (LENOVO_CPU_METHOD via legion-gpu-helper). Same store as the
    // BIOS setup page; applied by the firmware at the next boot.
    {
        auto *oc = new QGroupBox(QStringLiteral("CPU Overclocking (Firmware, Next Boot)"));
        auto *ol = new QHBoxLayout(oc);
        ol->setSpacing(10);
        auto *note = new QLabel;
        note->setProperty("role", "muted");
        oc->hide();
        lay->addWidget(oc);
        privileged::run(gpuHelperPath(), QJsonObject{{"op", "fw_oc"}}, this, [this, oc, ol, note](const privileged::Result &r) {
            if (!r.ok()) {
                if (r.message().contains(QLatin1String("unknown"))) return;  // not a Legion / old helper
                ol->addWidget(note); note->setWordWrap(true);
                note->setText(QStringLiteral("Firmware OC unavailable: ") + r.message());
                oc->show();
                return;
            }
            const QJsonObject t = r.json.value("tunes").toObject();
            const int mode = r.json.value("bios_oc_mode").toInt(-1);
            static const struct { const char *key, *text, *tip; } DEFS[] = {
                {"pbo_scalar", "PBO scalar", "Precision Boost Overdrive scalar (x)."},
                {"boost_mhz", "Boost override (MHz)", "Max CPU boost clock override, added to stock fmax."},
                {"curve_optimizer", "All-core CO", "Firmware all-core Curve Optimizer; negative = undervolt.\nRuntime per-core CO stays in the Ryzen tab."},
            };
            // One Apply for all tunes: writes only the changed ones, in one helper call.
            auto spins = std::make_shared<QList<std::pair<QString, QSpinBox *>>>();
            auto *btn = new QPushButton("Apply");
            btn->setObjectName("btnAccent");
            btn->setEnabled(false);
            btn->setToolTip("Stored in the BIOS, applied at the next boot. Asks for the administrator password.");
            auto dirty = [spins, btn] {
                bool any = false;
                for (const auto &[k, sb] : *spins) any |= sb->value() != sb->property("stored").toInt();
                btn->setEnabled(any);
            };
            for (const auto &d : DEFS) {
                if (!t.contains(d.key)) continue;
                const QJsonObject v = t.value(d.key).toObject();
                auto *lbl = new QLabel(d.text);
                auto *sb = new QSpinBox;
                sb->setRange(v.value("min").toInt(), v.value("max").toInt());
                sb->setValue(v.value("value").toInt());
                sb->setProperty("stored", sb->value());
                sb->setToolTip(d.tip);
                connect(sb, &QSpinBox::valueChanged, this, dirty);
                spins->append({QString::fromLatin1(d.key), sb});
                ol->addWidget(lbl); ol->addWidget(sb); ol->addSpacing(8);
            }
            connect(btn, &QPushButton::clicked, this, [this, spins, btn, note, dirty] {
                QJsonObject vals;
                for (const auto &[k, sb] : *spins)
                    if (sb->value() != sb->property("stored").toInt()) vals[k] = sb->value();
                if (vals.isEmpty()) return;
                btn->setEnabled(false);
                prewrite::backupThen(QStringLiteral("biosoc"), this, [this, spins, note, vals, dirty] {
                    privileged::run(privileged::helperPath(privileged::FIRMWARE_HELPER),
                                    QJsonObject{{"op", "set_fw_oc_many"}, {"values", vals}}, this,
                                    [spins, note, vals, dirty](const privileged::Result &r) {
                        if (r.ok())
                            for (const auto &[k, sb] : *spins) if (vals.contains(k)) sb->setProperty("stored", sb->value());
                        note->setText(r.ok() ? QStringLiteral("%1 change(s) saved — reboot to apply").arg(vals.size())
                                             : QStringLiteral("Failed: ") + r.message());
                        dirty();
                    }, privileged::FIRMWARE_TIMEOUT_MS);
                });
            });
            if (!spins->isEmpty()) ol->addWidget(btn);
            ol->addStretch(1);
            ol->addWidget(note);
            note->setText(mode == 0 ? QStringLiteral("OC is disabled in BIOS setup — values are ignored")
                                    : QStringLiteral("Takes effect after reboot"));
            oc->show();
        });
    }

    auto *top = new QHBoxLayout;
    auto *src = new QLabel(BASE);
    src->setProperty("role", "muted");
    top->addWidget(src);
    top->addStretch();
    rescan_ = new QPushButton("Rescan");
    connect(rescan_, &QPushButton::clicked, this, &FwattrTab::rebuild);
    top->addWidget(rescan_);
    auto *inspect = new QPushButton(tr("Parameter inspector…"));
    connect(inspect, &QPushButton::clicked, this, [this] {
        QDialog dlg(this);
        dlg.setWindowTitle(tr("Firmware Parameter Inspector"));
        dlg.resize(920, 520);
        auto *layout = new QVBoxLayout(&dlg);
        auto *hint = new QLabel(tr("Snapshot of the firmware editor. Last-known values are not live measurements; unsupported ranges are not inferred. Opening this inspector does not rescan or write firmware."));
        hint->setWordWrap(true); layout->addWidget(hint);
        auto *search = new QLineEdit;
        search->setPlaceholderText(tr("Search parameter, device or access state…"));
        layout->addWidget(search);
        auto *table = new QTableWidget(rows_.size(), 9);
        table->setHorizontalHeaderLabels({tr("Parameter"), tr("Device"), tr("Last known"), tr("Draft"), tr("Range"), tr("Default"), tr("Access"), tr("Source"), tr("Captured at")});
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->verticalHeader()->hide();
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->horizontalHeader()->setStretchLastSection(true);
        for (int i = 0; i < rows_.size(); ++i) {
            const auto &row = rows_[i]; const auto &a = row.info;
            const bool wmi = a.viaWmi();
            const bool known = !wmi || (row.wmiKnown && wmiReady_ && !wmiStale_ && !busy_ && !wmiReading_);
            const QString range = wmi ? QStringLiteral("%1 … %2 (WMI)").arg(a.wmiMin).arg(a.wmiMax)
                : a.ranged ? QStringLiteral("%1 … %2; %3 %4").arg(a.min).arg(a.max).arg(tr("step")).arg(a.step) : QStringLiteral("—");
            const QString access = a.readOnly ? tr("Read-only") : (!a.ranged && !wmi) ? tr("No valid range")
                : busy_ ? tr("Operation in progress") : locked_ ? tr("Profile locked") : !known ? tr("WMI not verified") : tr("Reported available");
            const QStringList cells{a.displayName + QStringLiteral(" [") + a.name + QStringLiteral("]"), a.device,
                known ? QString::number(a.current) : QStringLiteral("—"), QString::number(row.spin->value()), range,
                a.ranged && !wmi ? QString::number(a.def) : QStringLiteral("—"), access,
                wmi ? tr("WMI readback") : tr("sysfs snapshot"), known && !a.capturedAt.isEmpty() ? a.capturedAt : tr("Unknown")};
            for (int c = 0; c < cells.size(); ++c) { auto *item = new QTableWidgetItem(cells[c]); item->setToolTip(cells[c]); table->setItem(i, c, item); }
        }
        connect(search, &QLineEdit::textChanged, table, [table](const QString &query) {
            for (int r = 0; r < table->rowCount(); ++r) {
                bool match = query.trimmed().isEmpty();
                for (int c = 0; c < table->columnCount(); ++c) match |= table->item(r, c)->text().contains(query.trimmed(), Qt::CaseInsensitive);
                table->setRowHidden(r, !match);
            }
        });
        layout->addWidget(table, 1);
        const auto profile = pp::Watcher::instance().current();
        auto *profileLabel = new QLabel(tr("Platform profile (last observed): %1").arg(profile.value_or(tr("Unknown"))));
        profileLabel->setTextFormat(Qt::PlainText); profileLabel->setWordWrap(true); layout->addWidget(profileLabel);
        auto *sources = new QPushButton(tr("Read Power Limit Sources…"));
        sources->setEnabled(!powerSourcesPending_ && !qEnvironmentVariableIsSet("CENTURION_FWATTR_BASE"));
        auto *sourceView = new DiagReportView; sourceView->hide(); layout->addWidget(sourceView, 1);
        connect(sources, &QPushButton::clicked, &dlg, [this, sources, sourceView, &dlg] {
            if (powerSourcesPending_) return;
            powerSourcesPending_ = true; sources->setEnabled(false);
            auto report = std::make_shared<diag::Report>();
            auto *worker = QThread::create([report] {
                *report = systemdiagnostics::powerLimits();
                // This inspector is strictly informational, even when S08 has actionable findings.
                for (auto &f : report->findings) { f.fix = {}; f.fixLabel.clear(); }
            });
            connect(worker, &QThread::finished, this, [this] { powerSourcesPending_ = false; });
            connect(worker, &QThread::finished, &dlg, [sources, sourceView, report] {
                sources->setEnabled(true); sourceView->setReport(*report); sourceView->show();
            });
            connect(worker, &QThread::finished, worker, &QObject::deleteLater);
            worker->start();
        });
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
        connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        layout->addWidget(bb); dlg.exec();
    });
    top->addWidget(inspect);
    applyAll_ = new QPushButton("Apply All");
    applyAll_->setObjectName("btnAccent");
    connect(applyAll_, &QPushButton::clicked, this, &FwattrTab::applyAll);
    top->addWidget(applyAll_);
    lay->addLayout(top);

    tabs_ = new QTabWidget;
    tabs_->setDocumentMode(true);
    lay->addWidget(tabs_);

    status_ = new QLabel;
    status_->setProperty("role", "muted");
    status_->setWordWrap(true);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(status_);
    statusTimer_ = new QTimer(this);
    statusTimer_->setSingleShot(true);
    connect(statusTimer_, &QTimer::timeout, status_, &QLabel::clear);

    rebuild();
    // Lock state follows profile-change notifications instead of a 2 s poll.
    connect(&pp::Watcher::instance(), &pp::Watcher::changed, this, &FwattrTab::refreshLockState);
    refreshLockState();
}

void FwattrTab::showStatus(const QString &m, int t) {
    status_->setText(m);
    if (t) statusTimer_->start(t);
}

void FwattrTab::setBusy(bool b) {
    busy_ = b;
    tabs_->setEnabled(!b && !locked_ && !wmiReading_);
    applyAll_->setEnabled(!b && !locked_ && !wmiReading_);
    rescan_->setEnabled(!b && !wmiReading_);
}

void FwattrTab::refreshLockState() {
    // Firmware only honours these writes in Custom. No platform-profile
    // interface at all → nothing to lock against.
    // Cached handler + value (the old code rescanned /sys/class/platform-profile every 2 s).
    pp::Watcher &w = pp::Watcher::instance();
    w.check();
    const auto profile = w.current();
    const bool locked = profile && *profile != QLatin1String("custom");
    if (locked == locked_) return;
    locked_ = locked;
    setBusy(busy_);
    if (locked) {
        QString label = *profile;
        label.replace('-', ' ');
        if (!label.isEmpty()) label[0] = label[0].toUpper();
        banner_->setText(QStringLiteral(
            "Power profile is currently \"%1\", not Custom. The firmware only accepts changes to these "
            "attributes in Custom mode, so editing is disabled here. Switch to Custom on the Home tab to unlock it.")
            .arg(label));
        banner_->show();
    } else {
        banner_->hide();
    }
}

void FwattrTab::rebuild() {
    if (busy_ || wmiReading_) return;
    ++rowsRevision_;
    while (tabs_->count()) { QWidget *w = tabs_->widget(0); tabs_->removeTab(0); w->deleteLater(); }
    rows_.clear();
    tauLabel_ = nullptr; tauSlider_ = nullptr; tauSpin_ = nullptr; tauNote_ = nullptr;  // went with the old pages

    const QList<FwAttr> attrs = discover();
    {   // Remember the firmware ranges while the attributes are there.
        QJsonObject cache = readRangeCache();
        for (const FwAttr &a : attrs) {
            if (a.path.isEmpty()) continue;  // synthesized (missing) row
            QJsonObject c = cache.value(a.name).toObject();
            c["display"] = a.displayName;
            if (a.ranged) { c["min"] = a.min; c["max"] = a.max; c["default"] = a.def; }
            cache[a.name] = c;
            cache["_device"] = a.device;
        }
        writeRangeCache(cache);
    }
    if (attrs.isEmpty()) { tabs_->addTab(new QLabel("No firmware-attributes device found."), QStringLiteral("—")); return; }

    QMap<QString, QList<FwAttr>> byDev;
    for (const FwAttr &a : attrs) byDev[a.device].append(a);

    for (auto it = byDev.cbegin(); it != byDev.cend(); ++it) {
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        auto *page = new QWidget;
        auto *pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 4, 2, 2);
        pl->setSpacing(6);
        for (const QString &group : {QStringLiteral("CPU"), QStringLiteral("GPU"), QStringLiteral("Other")}) {
            QList<FwAttr> items;
            for (const FwAttr &a : *it) if (category(a.name) == group) items.append(a);
            if (items.isEmpty()) continue;
            auto *box = new QGroupBox(group);
            auto *g = new QGridLayout(box);
            g->setHorizontalSpacing(8);
            g->setVerticalSpacing(2);
            g->setColumnStretch(1, 1);
            int r = 0;
            for (const FwAttr &a : items) {
                const int index = rows_.size();
                auto *label = new QLabel(a.displayName);
                QString tip = QStringLiteral("%1\nmin=%2  max=%3  step=%4  default=%5")
                                  .arg(a.name).arg(a.min).arg(a.max).arg(a.step).arg(a.def);
                if (a.viaWmi()) tip += QStringLiteral("\n⚙ no firmware range — written via Lenovo WMI (acpi_call), range %1–%2").arg(a.wmiMin).arg(a.wmiMax);
                else if (a.readOnly) tip += QStringLiteral("\n🔒 read-only here: a status value, or a write that is a hardware action (dgpu_enable hot-unplugs the GPU)");
                else if (!a.ranged) tip += QStringLiteral("\n⚠ firmware reports no valid range for this attribute");
                label->setToolTip(tip);
                g->addWidget(label, r, 0);

                QSlider *slider = nullptr;
                const int lo = std::max(1, a.viaWmi() ? a.wmiMin : a.min), hi = std::max(lo, a.viaWmi() ? a.wmiMax : a.max);
                if (a.ranged || a.viaWmi()) {
                    slider = new QSlider(Qt::Horizontal);
                    slider->setRange(lo, hi);
                    slider->setSingleStep(a.viaWmi() ? 1 : a.step);
                    slider->setPageStep(a.viaWmi() ? 1 : a.step);
                    slider->setValue(a.current);
                    if (a.viaWmi()) slider->setStyleSheet(QStringLiteral("QSlider::sub-page:horizontal { background: %1; }"
                                                                         "QSlider::sub-page:horizontal:disabled { background: %2; }").arg(theme::PURPLE, theme::BG4));
                    g->addWidget(slider, r, 1);
                } else {
                    auto *w = new QLabel(QStringLiteral("⚠ no range"));
                    w->setProperty("tone", "warn");
                    g->addWidget(w, r, 1);
                }
                auto *spin = new QSpinBox;
                spin->setMinimumWidth(std::max(70, spin->sizeHint().width()));
                if (a.viaWmi()) spin->setRange(std::max(1, a.wmiMin), std::max(1, a.wmiMax));
                else if (a.ranged) { spin->setRange(std::max(1, a.min), std::max(1, a.max)); spin->setSingleStep(a.step); }
                else spin->setRange(1, std::max({1, a.current, a.def}));
                spin->setValue(a.current);
                g->addWidget(spin, r, 2);
                if (slider) {
                    connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
                    connect(spin, &QSpinBox::valueChanged, slider, &QSlider::setValue);
                }
                auto *apply = new QPushButton("Apply");
                apply->setMinimumWidth(64);
                connect(apply, &QPushButton::clicked, this, [this, index] { applyRow(index); });
                g->addWidget(apply, r, 3);
                auto *def = new QPushButton("Default");
                def->setMinimumWidth(72);
                connect(def, &QPushButton::clicked, spin, [spin, d = a.def] { spin->setValue(d); });
                g->addWidget(def, r, 4);
                // No authoritative range → nothing safe to offer: whole row disabled.
                if (!a.ranged && !a.viaWmi()) { spin->setEnabled(false); apply->setEnabled(false); def->setEnabled(false); }
                if (a.viaWmi()) { spin->setEnabled(false); if (slider) slider->setEnabled(false); apply->setEnabled(false); }  // until readWmi confirms acpi_call
                rows_.append({a, slider, spin, apply, def});
                ++r;
            }
            if (group == QLatin1String("CPU") && !tauSlider_) {
                // PL1 tau: laid out like the rows above but never editable (no kernel attribute; on AMD
                // nothing in the ACPI tables passes a write on). Hidden until the EC has been read.
                const QString tip = QStringLiteral(
                    "PL1 tau: how long the CPU may stay above PL1 before it is held to it. The firmware keeps it in the EC\n"
                    "(WMI feature 0x01070000) without a kernel attribute, in the steps of Intel's time-window encoding\n"
                    "(20, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160 s). On AMD nothing in the ACPI tables passes it on,\n"
                    "so whether changing it would do anything is unknown: shown, not offered.");
                tauLabel_ = new QLabel(QStringLiteral("CPU PL1 tau (time window)"));
                tauSlider_ = new QSlider(Qt::Horizontal);
                tauSlider_->setRange(20, 160);
                tauSlider_->setEnabled(false);
                tauSpin_ = new QSpinBox;
                tauSpin_->setMinimumWidth(std::max(70, tauSpin_->sizeHint().width()));
                tauSpin_->setRange(0, 999);
                tauSpin_->setSuffix(QStringLiteral(" s"));
                tauSpin_->setButtonSymbols(QAbstractSpinBox::NoButtons);
                tauSpin_->setEnabled(false);
                tauNote_ = new QLabel(QStringLiteral("🔒 read-only"));
                tauNote_->setProperty("role", "muted");
                tauNote_->setAlignment(Qt::AlignCenter);
                for (QWidget *w : {static_cast<QWidget *>(tauLabel_), static_cast<QWidget *>(tauSlider_),
                                   static_cast<QWidget *>(tauSpin_), static_cast<QWidget *>(tauNote_)})
                    w->setToolTip(tip);
                g->addWidget(tauLabel_, r, 0);
                g->addWidget(tauSlider_, r, 1);
                g->addWidget(tauSpin_, r, 2);
                g->addWidget(tauNote_, r, 3, 1, 2);
                ++r;
                showTau();
            }
            // Explain the WMI rows right under the box that contains them.
            bool anyWmi = false;
            for (const FwAttr &a : items) anyWmi |= a.viaWmi();
            if (anyWmi) {
                auto *note = new QLabel(QStringLiteral(
                    "<span style='color:%1'>⚙</span> <b>cTGP</b>, <b>power performance aware boost</b> and <b>GPU to CPU dynamic "
                    "boost</b> are reported by the firmware without a valid range (min = max = 0), so the kernel refuses "
                    "to write them through sysfs. These three are written directly through the Lenovo WMI method "
                    "(<tt>\\_SB.GZFD.WMAE</tt>) with <tt>acpi_call</tt> and read back from it. Boost limits use 1–25 W (0 is never written: the firmware would drop the attribute); "
                    "cTGP range follows the GPU model: 5 W up to its vBIOS max power limit minus Dynamic Boost (150 W on an RTX 5080 Laptop). "
                    "Needs the <tt>acpi_call</tt> module.").arg(theme::PURPLE));
                note->setTextFormat(Qt::RichText);
                note->setWordWrap(true);
                note->setProperty("role", "muted");
                g->addWidget(note, r++, 0, 1, 5);
            }
            pl->addWidget(box);
        }
        pl->addStretch();
        scroll->setWidget(page);
        tabs_->addTab(scroll, it.key());
    }
    setBusy(false);
    // WMI rows are read through pkexec + acpi_call: only once the tab is
    // actually shown. The constructor runs at login with the app hidden in the
    // tray, where it used to cost a root helper run (and, in a session polkit
    // does not treat as active, a password prompt out of nowhere).
    wmiStale_ = true;
    if (isVisible()) readWmi();
}

void FwattrTab::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    // Scenes, profile changes and other tools rewrite these values while the tab is not looking, so every
    // (re)open starts with a full rescan: sysfs attributes are re-read by rebuild(), which also queues the
    // WMI read. rebuild() is a no-op while an apply is running; the stale flag still catches the WMI read then.
    rebuild();
    if (wmiStale_) readWmi();
}

/// Value snapped onto the driver's min + k·step grid (the helper checks the
/// range but not the step; an off-grid value is rounded by the WMI method
/// in firmware-specific ways, so it's never sent).
int FwattrTab::snapped(const Row &r) const {
    const int v = r.spin->value();
    if (r.info.viaWmi() || !r.info.ranged || r.info.step <= 1) return v;
    const int k = qRound(double(v - r.info.min) / r.info.step);
    return std::clamp(r.info.min + k * r.info.step, r.info.min, r.info.max);
}

bool FwattrTab::confirmChanges(const QList<int> &indices) {
    if (busy_ || wmiReading_ || locked_ || indices.isEmpty()) return false;
    const auto revision = rowsRevision_;
    struct Change { int index; QString path, key; int oldValue, draft; };
    QList<Change> captured;
    QStringList lines;
    for (int i : indices) {
        if (i < 0 || i >= rows_.size()) return false;
        const auto &row = rows_.at(i); const auto &a = row.info;
        if (a.readOnly || (!a.ranged && !a.viaWmi())) return false;
        if (a.viaWmi() && (!wmiReady_ || wmiStale_ || !row.wmiKnown)) return false;
        // Firmware sysfs reads are direct, never display-cache evidence. A
        // change since the last scan requires another explicit rescan/review.
        const auto current = a.viaWmi() ? std::optional<int>(a.current) : readInt(a.path);
        if (!current || *current != a.current) {
            QMessageBox::warning(this, tr("Firmware changes"), tr("A parameter changed or cannot be read. Rescan and review the draft before applying."));
            return false;
        }
        const int value = snapped(row);
        captured.append(Change{i, a.path, a.wmiKey, *current, value});
        const QString transport = a.viaWmi() ? tr("WMI (last read, not a fresh verification)") : tr("sysfs (direct read)");
        lines << QStringLiteral("%1 / %2: %3 → %4 · %5").arg(a.device, a.name).arg(*current).arg(value).arg(transport);
    }
    QMessageBox dialog(QMessageBox::Question, tr("Firmware changes"),
        tr("Review the exact firmware values to be sent. The numbers use the firmware's native units. Cancel sends no write request."),
        QMessageBox::Ok | QMessageBox::Cancel, this);
    dialog.setTextFormat(Qt::PlainText);
    dialog.setInformativeText(lines.join(QLatin1Char('\n')));
    dialog.setDefaultButton(QMessageBox::Cancel);
    if (dialog.exec() != QMessageBox::Ok) return false;
    refreshLockState();
    if (busy_ || wmiReading_ || locked_ || revision != rowsRevision_) return false;
    for (const auto &change : captured) {
        if (change.index >= rows_.size()) return false;
        const auto &row = rows_.at(change.index);
        if (row.info.path != change.path || row.info.wmiKey != change.key || row.info.current != change.oldValue || snapped(row) != change.draft) return false;
        if (!row.info.viaWmi() && readInt(change.path) != std::optional<int>(change.oldValue)) {
            QMessageBox::warning(this, tr("Firmware changes"), tr("A parameter changed during confirmation. Nothing was sent.")); return false;
        }
        if (row.info.viaWmi() && (!wmiReady_ || wmiStale_ || !row.wmiKnown)) return false;
    }
    return true;
}

void FwattrTab::applyRow(int index) {
    if (busy_ || wmiReading_ || locked_ || index < 0 || index >= rows_.size() || rows_[index].info.readOnly) return;
    if (!confirmChanges({index})) return;
    if (rows_[index].info.viaWmi()) { applyWmi({index}); return; }
    if (!rows_[index].info.ranged) return;
    const int value = snapped(rows_[index]);
    rows_[index].spin->setValue(value);
    const QString name = rows_[index].info.name, path = rows_[index].info.path;
    setBusy(true);
    privileged::run(helperPath(), QJsonObject{{"path", path}, {"value", value}}, this,
        [this, index, value, name](const privileged::Result &r) {
            setBusy(false);
            if (!r.reached) { QMessageBox::critical(this, "Authorization failed", r.error); return; }
            if (!r.ok()) {
                QMessageBox::critical(this, "Error", name + " not written:\n" + (r.message().isEmpty() ? "unknown error" : r.message()));
                return;
            }
            if (index >= rows_.size()) return;
            const auto actual = readInt(rows_[index].info.path);
            if (actual) rows_[index].info.current = *actual;
            if (!actual || *actual != value) {
                showStatus(tr("%1: write returned, but direct readback did not verify the requested value.").arg(name), 10000);
                return;
            }
            showStatus(tr("%1 = %2 (readback matches)").arg(name).arg(value));
        });
}

void FwattrTab::applyAll() {
    if (busy_ || wmiReading_ || locked_) return;
    QList<int> changed, wmiChanged;
    QJsonArray items;
    for (int i = 0; i < rows_.size(); ++i) {
        Row &r = rows_[i];
        if (r.info.readOnly) continue;
        if (r.info.viaWmi()) { if (wmiReady_ && !wmiStale_ && r.wmiKnown && r.spin->value() != r.info.current) wmiChanged << i; continue; }
        if (!r.info.ranged) continue;
        const int v = snapped(r);
        if (v == r.info.current) continue;
        changed << i;
        items.append(QJsonObject{{"path", r.info.path}, {"value", v}});
    }
    if (changed.isEmpty() && wmiChanged.isEmpty()) { QMessageBox::information(this, "Apply All", "No changes to apply."); return; }
    if (!confirmChanges(changed + wmiChanged)) return;
    if (changed.isEmpty()) { applyWmi(wmiChanged); return; }
    for (int i : changed) rows_[i].spin->setValue(snapped(rows_[i]));
    setBusy(true);
    privileged::run(helperPath(), QJsonDocument(items).toJson(QJsonDocument::Compact), this,
        [this, changed, wmiChanged, items](const privileged::Result &r) {
            setBusy(false);
            if (!r.reached) { QMessageBox::critical(this, "Authorization failed", r.error); return; }
            const QJsonArray results = r.json.value("results").toArray();
            QStringList failures;
            int applied = 0;
            for (int k = 0; k < changed.size(); ++k) {
                Row &row = rows_[changed[k]];
                const QJsonObject res = results.at(k).toObject();
                const int requested = items.at(k).toObject().value(QStringLiteral("value")).toInt();
                const auto actual = readInt(row.info.path);
                if (actual) { row.info.current = *actual; row.info.capturedAt = QDateTime::currentDateTime().toString(Qt::ISODate); }
                if (res.value("ok").toBool() && res.value("path").toString() == row.info.path && actual && *actual == requested) ++applied;
                else failures << row.info.name + ": " + res.value("error").toString(tr("Write or direct readback not verified"));
            }
            if (failures.isEmpty()) showStatus(QStringLiteral("Applied %1 attribute(s)").arg(applied));
            else QMessageBox::warning(this, "Some writes failed",
                QStringLiteral("%1/%2 attribute(s) applied.\n\nFailures:\n").arg(applied).arg(changed.size()) + failures.join('\n'));
            if (!wmiChanged.isEmpty()) {
                if (failures.isEmpty() && r.ok()) applyWmi(wmiChanged);
                else showStatus(tr("WMI phase not sent because the sysfs phase was not fully verified. Review and retry separately."), 10000);
            }
        });
}

void FwattrTab::applyWmi(const QList<int> &indices) {
    if (busy_ || wmiReading_ || locked_ || indices.isEmpty()) return;
    if (!wmiReady_) {
        QMessageBox::warning(this, "acpi_call needed", QStringLiteral("These attributes are written through acpi_call, which is not available.\n"
                                            "Load it with: modprobe acpi_call (install: %1).").arg(systools::installCommand(QStringLiteral("acpi_call"))));
        return;
    }
    QJsonObject vals;
    for (int i : indices) {
        if (i < 0 || i >= rows_.size() || rows_[i].info.readOnly || !rows_[i].info.viaWmi() || !rows_[i].wmiKnown || wmiStale_) return;
        vals[rows_[i].info.wmiKey] = rows_[i].spin->value();
    }
    setBusy(true);
    privileged::run(gpuHelperPath(), QJsonObject{{"op", "apply"}, {"values", vals}}, this,
        [this](const privileged::Result &r) {
            setBusy(false);
            wmiStale_ = true;
            if (!r.reached) { QMessageBox::critical(this, "Authorization failed", r.error); readWmi(); return; }
            QStringList fails, oks;
            for (const auto &x : r.json.value("results").toArray()) {
                const QJsonObject o = x.toObject();
                (o.value("ok").toBool() ? oks : fails) << o.value("message").toString();
            }
            if (!fails.isEmpty()) QMessageBox::warning(this, "WMI write failed", fails.join('\n'));
            if (!oks.isEmpty()) showStatus(tr("WMI write returned; refreshing hardware values.") + QStringLiteral(" · ") + oks.join(" · "), 5000);
            wmiStale_ = true;
            readWmi();  // show what the EC holds now
        });
}

void FwattrTab::readWmi() {
    bool any = false;
    for (const Row &r : std::as_const(rows_)) any |= r.info.viaWmi();
    if (!any || busy_ || wmiReading_) return;
    wmiReading_ = true;
    const auto revision = rowsRevision_;
    setBusy(false);
    privileged::run(gpuHelperPath(), QJsonObject{{"op", "status"}}, this, [this, revision](const privileged::Result &r) {
        wmiReading_ = false;
        setBusy(false);
        if (revision != rowsRevision_) { wmiStale_ = true; return; }
        wmiReady_ = r.ok() && r.json.value("acpi").toBool();
        wmiStale_ = !wmiReady_;
        const QJsonObject vals = r.json.value("values").toObject();
        tauSeconds_ = r.json.value("pl1_tau_s").toInt();
        showTau();
        {   // WMAE id check for the CPU limits: the fallback is only offered
            // once WMAE has been seen to report exactly what sysfs reports.
            QJsonObject cache = readRangeCache();
            bool changed = false;
            for (auto it = vals.begin(); it != vals.end(); ++it) {
                const QJsonObject o = it.value().toObject();
                if (!o.contains("wmae") || !o.value("present").toBool() || !o.contains("value")) continue;
                const QString attr = o.value("attr").toString();
                const bool match = o.value("wmae").toInt() == o.value("value").toInt();
                QJsonObject c = cache.value(attr).toObject();
                if (c.value("wmae_verified").toBool() != match) { c["wmae_verified"] = match; cache[attr] = c; changed = true; }
                if (!match) showStatus(QStringLiteral("⚠ %1: WMAE reads %2 but sysfs %3 — its WMI fallback stays off.")
                                           .arg(attr).arg(o.value("wmae").toInt()).arg(o.value("value").toInt()), 10000);
            }
            if (changed) writeRangeCache(cache);
        }
        for (Row &row : rows_) {
            if (!row.info.viaWmi()) continue;
            const QJsonObject o = vals.value(row.info.wmiKey).toObject();
            const bool have = wmiReady_ && o.value("value").isDouble();
            row.wmiKnown = have;
            if (have) {
                const int v = o.value("value").toInt();
                row.info.current = v;
                row.info.capturedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
                // Range from the helper: for cTGP it follows the GPU model (vBIOS
                // max power limit minus Dynamic Boost), cached while the dGPU sleeps.
                if (o.contains("min") && o.contains("max")) {
                    const int lo = std::max(1, o.value("min").toInt()), hi = std::max(lo, o.value("max").toInt());
                    row.info.wmiMin = lo; row.info.wmiMax = hi;
                    row.spin->setRange(lo, hi);
                    if (row.slider) row.slider->setRange(lo, hi);
                }
                row.spin->setValue(v);
            }
            row.spin->setEnabled(have);
            if (row.slider) row.slider->setEnabled(have);
            if (row.apply) row.apply->setEnabled(have);
            if (row.def) row.def->setEnabled(have);
            if (!have) row.spin->setToolTip(wmiReady_ ? o.value("error").toString() : QStringLiteral("acpi_call not loaded"));
        }
        if (!wmiReady_ && r.reached) showStatus("acpi_call not loaded — the three WMI-handled GPU attributes are read-only (modprobe acpi_call).", 8000);
    });
}

void FwattrTab::showTau() {
    if (!tauSlider_) return;
    const bool have = tauSeconds_ > 0;
    for (QWidget *w : {static_cast<QWidget *>(tauLabel_), static_cast<QWidget *>(tauSlider_),
                       static_cast<QWidget *>(tauSpin_), static_cast<QWidget *>(tauNote_)})
        w->setVisible(have);
    if (!have) return;
    tauSlider_->setRange(std::min(20, tauSeconds_), std::max(160, tauSeconds_));
    tauSlider_->setValue(tauSeconds_);
    tauSpin_->setValue(tauSeconds_);
}

QMap<QString, int> FwattrTab::wmiValues() const {
    QMap<QString, int> out;
    if (!wmiReady_ || wmiStale_ || wmiReading_) return out;
    for (const Row &r : rows_) if (r.info.viaWmi() && r.wmiKnown) out.insert(r.info.name, r.info.current);
    return out;
}
