#include "curvepage.h"
#include "memtestbox.h"
#include "privileged.h"
#include "sysinfo.h"
#include "theme.h"
#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMap>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

static const char *JOURNAL = "/var/lib/centurion/oc-journal.jsonl";
static const char *EXPOSURE = "/var/lib/centurion/exposure.json";
static constexpr qint64 EVENT_SLACK_S = 60;      // an error logged just after the interval still counts for it
static constexpr qint64 RESET_BURST_MS = 60000;  // errors come in bursts: one reset per minute at most

static QSettings guiIni() {
    return QSettings(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/centurion/gui.ini"),
                     QSettings::IniFormat);
}

static QString bootId() {
    QFile f(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    return f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.readAll()).trimmed() : QString();
}

static QString groupOf(const QString &kind) { return kind == QLatin1String("gpu_curve") ? QStringLiteral("gpu") : QStringLiteral("cpu"); }

struct Rec { qint64 t; QString boot, group, state; };

static QList<Rec> journal() {
    QList<Rec> out;
    QFile f(QString::fromLatin1(JOURNAL));
    if (!f.open(QIODevice::ReadOnly) || f.size() > (1 << 20)) return out;
    for (const QByteArray &l : f.readAll().split('\n')) {
        const QJsonObject o = QJsonDocument::fromJson(l).object();
        if (o.isEmpty()) continue;
        QString st = o.value("state").toString();
        if (st == QLatin1String("reset")) st = QStringLiteral("stock");
        out.append({o.value("t").toInteger(), o.value("boot").toString(), groupOf(o.value("kind").toString()), st});
    }
    std::stable_sort(out.begin(), out.end(), [](const Rec &a, const Rec &b) { return a.t < b.t; });
    return out;
}

QString CurvePage::currentState(const QString &group) {
    const QString boot = bootId();
    QString st = QStringLiteral("stock");
    for (const Rec &r : journal()) if (r.boot == boot && r.group == group) st = r.state;
    return st;
}

static QString profileOf(const QString &state) {
    return state.startsWith(QLatin1String("profile:")) ? state.mid(8) : QString();
}

static QString pretty(const QString &state) {
    if (state.startsWith(QLatin1String("profile:"))) return "'" + state.mid(8) + "'";
    if (state == QLatin1String("custom")) return QStringLiteral("unsaved edits (Apply in the tab)");
    return state;
}

CurvePage::CurvePage(QWidget *parent) : QWidget(parent) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(6);
    auto *intro = new QLabel(QStringLiteral(
        "Each curve the tabs, scenes or game hooks applied, against the kernel errors logged while it was in effect "
        "(Monitor page's saved log). GPU curve ↔ NVIDIA Xid / GSP / D3cold; CPU curve ↔ machine checks and lockups. "
        "A curve that collects errors much faster than stock is the first thing to back off."));
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral("color:%1").arg(theme::MUTED));
    root->addWidget(intro);

    table_ = new QTreeWidget;
    table_->setColumnCount(6);
    table_->setHeaderLabels({QStringLiteral("Curve"), QStringLiteral("State"), QStringLiteral("In effect"), QStringLiteral("Errors"),
                             QStringLiteral("Per 10 h"), QStringLiteral("Last error")});
    table_->setRootIsDecorated(false);
    table_->setAlternatingRowColors(true);
    for (int i = 0; i < 5; ++i) table_->header()->setSectionResizeMode(i, QHeaderView::ResizeToContents);
    table_->header()->setStretchLastSection(true);
    root->addWidget(table_, 1);
    note_ = new QLabel;
    note_->setWordWrap(true);
    note_->setTextFormat(Qt::RichText);
    root->addWidget(note_);

    gpuReset_ = new QCheckBox(QStringLiteral("Take the GPU curve back to stock after an Xid while one is applied"));
    gpuReset_->setToolTip(QStringLiteral("Xid 13/31/43/61/62/69/109/119/120 while a GPU curve is in effect: the curve is reset at once\n"
                                         "(not after Xid 79 / 154 — the card is gone then and the driver must not be touched)."));
    cpuReset_ = new QCheckBox(QStringLiteral("Take the CPU curve back to stock after a machine check or lockup while one is applied"));
    gpuReset_->setChecked(guiIni().value("guard/gpu_reset", false).toBool());
    cpuReset_->setChecked(guiIni().value("guard/cpu_reset", false).toBool());
    connect(gpuReset_, &QCheckBox::toggled, this, [](bool on) { guiIni().setValue("guard/gpu_reset", on); });
    connect(cpuReset_, &QCheckBox::toggled, this, [](bool on) { guiIni().setValue("guard/cpu_reset", on); });
    root->addWidget(gpuReset_);
    root->addWidget(cpuReset_);
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("<span style='color:%1'>Without these ticked, such an error only raises a notification naming the curve.</span>").arg(theme::MUTED)), 1);
    auto *bRefresh = new QPushButton(QStringLiteral("Refresh"));
    connect(bRefresh, &QPushButton::clicked, this, &CurvePage::recompute);
    row->addWidget(bRefresh);
    root->addLayout(row);
    memTest_ = new MemTestBox;
    connect(memTest_, &MemTestBox::alert, this, &CurvePage::alert);
    root->addWidget(memTest_);
}

void CurvePage::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    recompute();
}

namespace {
struct Boot { QString id; qint64 start = 0, end = 0; };
struct Interval { QString group, state, boot8; qint64 from, to; };
struct Stat { double hours = 0; int errors = 0; QString last; qint64 lastT = 0; };
}

void CurvePage::recompute() {
    const QList<Rec> recs = journal();
    const QString cur = bootId();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    // Boots: centurion-boot-guard's record, completed by the journal for boots it did not see.
    QMap<QString, Boot> boots;
    {
        QFile f(QString::fromLatin1(EXPOSURE));
        if (f.open(QIODevice::ReadOnly) && f.size() < (4 << 20)) {
            for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).object().value("boots").toArray()) {
                const QJsonObject b = v.toObject();
                Boot x{b.value("boot").toString(), b.value("start").toInteger(), b.value("end").toInteger()};
                if (!x.id.isEmpty()) boots.insert(x.id, x);
            }
        }
    }
    for (const Rec &r : recs) {
        Boot &b = boots[r.boot];
        if (b.id.isEmpty()) { b.id = r.boot; b.start = r.t; }
        b.start = std::min(b.start, r.t);
        if (!b.end || b.end < r.t) b.end = std::max(b.end, r.t);
    }
    if (boots.contains(cur)) boots[cur].end = now;
    // A boot that died without its end: until the next boot started (a lower bound for its last state).
    QList<Boot> order = boots.values();
    std::sort(order.begin(), order.end(), [](const Boot &a, const Boot &b) { return a.start < b.start; });
    for (int i = 0; i + 1 < order.size(); ++i)
        if (order[i].end <= order[i].start) order[i].end = order[i + 1].start;

    QList<Interval> iv;
    QMap<QString, Stat> stats;  // "group|state"
    for (const Boot &b : order) {
        if (b.end <= b.start) continue;
        for (const char *gc : {"gpu", "cpu"}) {
            const QString g = QLatin1String(gc);
            QString state = QStringLiteral("stock");
            qint64 from = b.start;
            auto close = [&](qint64 to) {
                if (to <= from) return;
                iv.append({g, state, b.id.left(8), from, to});
                stats[g + '|' + state].hours += (to - from) / 3600.0;
            };
            for (const Rec &r : recs) {
                if (r.boot != b.id || r.group != g || r.t < b.start) continue;
                close(r.t);
                state = r.state;
                from = r.t;
            }
            close(b.end);
        }
    }

    // Errors from the saved kernel-error log (Monitor page).
    QString dir = qEnvironmentVariable("XDG_STATE_HOME");
    if (dir.isEmpty()) dir = QDir::homePath() + QStringLiteral("/.local/state");
    static const QRegularExpression re(QStringLiteral("^(\\d{4}-\\d\\d-\\d\\d \\d\\d:\\d\\d:\\d\\d)\\s+boot=(\\S+) seq=\\d+\\s+\\S+\\s+(\\S+)\\s+(.*?)\\s+\\|"));
    int unmatched = 0;
    for (const QString &name : {QStringLiteral("health.log.1"), QStringLiteral("health.log")}) {
        QFile f(dir + QStringLiteral("/centurion/") + name);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        while (!f.atEnd()) {
            const QRegularExpressionMatch m = re.match(QString::fromUtf8(f.readLine()));
            if (!m.hasMatch()) continue;
            const QString kind = m.captured(3).toUpper(), title = m.captured(4);
            QString g;
            if (kind == QLatin1String("XID")) {
                if (title.startsWith(QLatin1String("Xid 154")) || title.startsWith(QLatin1String("Xid 45:"))) continue;
                g = QStringLiteral("gpu");
            } else if (kind == QLatin1String("GSP") || kind == QLatin1String("D3COLD")) g = QStringLiteral("gpu");
            else if (kind == QLatin1String("MCE") || kind == QLatin1String("LOCKUP")) g = QStringLiteral("cpu");
            else continue;
            const qint64 t = QDateTime::fromString(m.captured(1), QStringLiteral("yyyy-MM-dd HH:mm:ss")).toSecsSinceEpoch();
            const QString b8 = m.captured(2);
            const Interval *hit = nullptr;
            for (const Interval &x : std::as_const(iv))
                if (x.group == g && x.boot8 == b8 && t >= x.from && t <= x.to + EVENT_SLACK_S) hit = &x;
            if (!hit) { ++unmatched; continue; }
            Stat &s = stats[g + '|' + hit->state];
            ++s.errors;
            if (t >= s.lastT) { s.lastT = t; s.last = m.captured(1) + QStringLiteral("  ") + title.left(60); }
        }
    }

    table_->clear();
    const bool cpuIntel = sysinfo::isIntel();
    for (auto it = stats.cbegin(); it != stats.cend(); ++it) {
        const QString g = it.key().section('|', 0, 0), state = it.key().section('|', 1);
        const Stat &s = it.value();
        if (s.hours < 0.01 && !s.errors) continue;
        const double rate = s.hours > 0 ? s.errors / s.hours * 10 : 0;
        auto *item = new QTreeWidgetItem({g == QLatin1String("gpu") ? QStringLiteral("GPU (NVIDIA)") : cpuIntel ? QStringLiteral("CPU (undervolt)") : QStringLiteral("CPU (Curve Optimizer)"),
                                          pretty(state), QStringLiteral("%1 h").arg(s.hours, 0, 'f', s.hours < 10 ? 1 : 0),
                                          QString::number(s.errors), s.hours >= 0.5 ? QString::number(rate, 'f', 2) : QStringLiteral("—"), s.last});
        // Worse than stock of the same curve by a clear margin: highlighted.
        const Stat stock = stats.value(g + QStringLiteral("|stock"));
        const double stockRate = stock.hours > 0 ? stock.errors / stock.hours * 10 : 0;
        if (state != QLatin1String("stock") && s.errors >= 2 && s.hours >= 0.5 && rate > std::max(0.2, 2 * stockRate))
            for (int i = 0; i < 6; ++i) item->setForeground(i, QColor(QLatin1String(theme::WARN)));
        table_->addTopLevelItem(item);
    }
    if (!table_->topLevelItemCount())
        table_->addTopLevelItem(new QTreeWidgetItem({QStringLiteral("No curve history yet — it starts with the first curve applied after this update."),
                                                     QString(), QString(), QString(), QString(), QString()}));
    note_->setText(QStringLiteral("<span style='color:%1'>Now: GPU curve %2 · CPU curve %3%4</span>")
                       .arg(theme::MUTED, pretty(currentState("gpu")), pretty(currentState("cpu")),
                            unmatched ? QStringLiteral(" · %1 older error(s) outside the recorded curve history").arg(unmatched) : QString()));
}

static bool nvidiaUsable() {
    const QDir d(QStringLiteral("/sys/bus/pci/devices"));
    for (const QString &e : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System)) {
        auto rd = [&](const char *f) { QFile x(d.filePath(e) + '/' + QLatin1String(f)); return x.open(QIODevice::ReadOnly) ? x.readAll().trimmed() : QByteArray(); };
        if (rd("vendor") == "0x10de" && rd("class").startsWith("0x03") && QFileInfo::exists(d.filePath(e) + QStringLiteral("/driver"))
            && rd("power/runtime_status") != "error")
            return !QDir(QStringLiteral("/proc/driver/nvidia/gpus")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty();
    }
    return false;
}

void CurvePage::resetGpuCurve(const QString &title) {
    const QString st = currentState(QStringLiteral("gpu"));
    if (st == QLatin1String("stock")) return;
    lastGpuReset_ = QDateTime::currentMSecsSinceEpoch();
    privileged::run(privileged::helperPath(QStringLiteral("nvcurve-root-helper")), QJsonObject{{"op", "reset_gpu_curve"}}, this,
                    [this, title, st](const privileged::Result &r) {
                        if (r.ok()) Q_EMIT curveReset(QStringLiteral("gpu"), profileOf(st), title + QStringLiteral("\nGPU curve reset to stock."));
                        else Q_EMIT alert(QStringLiteral("GPU curve guard"), QStringLiteral("Could not reset the GPU curve: ") + r.message());
                        if (isVisible()) recompute();
                    }, 120000);
}

void CurvePage::onFault(const QJsonObject &e) {
    memTest_->onFault(e);
    const QString kind = e.value("kind").toString();
    const QString title = e.value("title").toString();
    if (kind == QLatin1String("xid")) {
        const int code = e.value("code").toInt();
        static const QList<int> curveLike{13, 31, 43, 61, 62, 69, 79, 109, 119, 120};
        if (!curveLike.contains(code)) return;
        const QString st = currentState("gpu");
        if (st == QLatin1String("stock")) return;
        QString msg = title + QStringLiteral("\nGPU curve in effect: ") + pretty(st) + '.';
        const bool gone = code == 79;
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (gpuReset_->isChecked() && !gone && nowMs - lastGpuReset_ > RESET_BURST_MS && nvidiaUsable()) {
            resetGpuCurve(title);
            msg += QStringLiteral(" Resetting it to stock…");
        } else if (gone) {
            msg += QStringLiteral(" The card fell off the bus: reboot (the curve is gone with it).");
        }
        Q_EMIT alert(QStringLiteral("GPU curve suspect"), msg);
    } else if (kind == QLatin1String("mce") || kind == QLatin1String("lockup")) {
        const QString st = currentState("cpu");
        if (st == QLatin1String("stock")) return;
        QString msg = title + QStringLiteral("\nCPU curve in effect: ") + pretty(st) + '.';
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (cpuReset_->isChecked() && nowMs - lastCpuReset_ > RESET_BURST_MS) {
            lastCpuReset_ = nowMs;
            const bool intel = sysinfo::isIntel();
            privileged::run(privileged::helperPath(intel ? QStringLiteral("intel-uv-helper") : QStringLiteral("ryzen-co-helper")),
                            QJsonObject{{"op", "reset"}}, this,
                            [this, st](const privileged::Result &r) {
                                if (r.ok()) Q_EMIT curveReset(QStringLiteral("cpu"), profileOf(st), QStringLiteral("CPU curve reset to stock."));
                                else Q_EMIT alert(QStringLiteral("CPU curve guard"), QStringLiteral("Could not reset the CPU curve: ") + r.message());
                                if (isVisible()) recompute();
                            }, 120000);
            msg += QStringLiteral(" Resetting it to stock…");
        }
        Q_EMIT alert(QStringLiteral("CPU curve suspect"), msg);
    } else {
        return;
    }
    if (isVisible()) recompute();
}
