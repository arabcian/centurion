#include "advancedhealth.h"

#include "advanceddiagnostics.h"
#include "diagnostictools.h"
#include "diagreportview.h"
#include "diagsources.h"
#include "eventinfo.h"
#include "evidencereports.h"
#include "privileged.h"
#include "ryzentab.h"
#include "systemdiagnostics.h"
#include "theme.h"

#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

const QList<std::pair<const char *, const char *>> &features() {
    static const QList<std::pair<const char *, const char *>> f = {
        {"H18", QT_TRANSLATE_NOOP("AdvancedHealthPage", "Guided triage: start here")},
        {"H01", QT_TRANSLATE_NOOP("AdvancedHealthPage", "Boot Regression Detective")},
        {"H06", QT_TRANSLATE_NOOP("AdvancedHealthPage", "NVMe Deep Health")},
        {"H10", QT_TRANSLATE_NOOP("AdvancedHealthPage", "USB Stability Watch")},
        {"H12", QT_TRANSLATE_NOOP("AdvancedHealthPage", "Sleep / Resume Failure Matrix")},
        {"H13", QT_TRANSLATE_NOOP("AdvancedHealthPage", "CO Evidence Cross-Link")},
        {"H15", QT_TRANSLATE_NOOP("AdvancedHealthPage", "Pre-crash Coverage Inspector")},
        {"H16", QT_TRANSLATE_NOOP("AdvancedHealthPage", "GPU Memory Reliability")},
        {"H17", QT_TRANSLATE_NOOP("AdvancedHealthPage", "Incident notebook")}};
    return f;
}

QHash<int, evidencereports::CoreRef> coreMap() {
    QHash<int, evidencereports::CoreRef> map;
    if (!diagsrc::read(QStringLiteral("/proc/cpuinfo"), 4096).contains(QStringLiteral("AuthenticAMD"))) return map;
    const ryzen::Layout l = ryzen::detect();
    for (auto it = l.cores.cbegin(); it != l.cores.cend(); ++it)
        for (int slot = 0; slot < it.value().size(); ++slot)
            for (int cpu : it.value()[slot].cpus) map.insert(cpu, {it.key(), slot, l.l3Kib.value(it.key())});
    return map;
}

// The stored statuses stay English (the note file is shared across languages); shown translated.
[[maybe_unused]] const char *const STATUS_NAMES[] = {
    QT_TRANSLATE_NOOP("AdvancedHealthPage", "New"), QT_TRANSLATE_NOOP("AdvancedHealthPage", "Investigating"),
    QT_TRANSLATE_NOOP("AdvancedHealthPage", "Monitoring"), QT_TRANSLATE_NOOP("AdvancedHealthPage", "Resolved"),
    QT_TRANSLATE_NOOP("AdvancedHealthPage", "Reopened")};
QString statusLabel(const QString &s) { return QCoreApplication::translate("AdvancedHealthPage", s.toUtf8().constData()); }

}  // namespace

namespace investigations {

QStringList opsFor(const QString &code) {
    QStringList ops;
    if (QStringList{"H01", "H12", "H13", "H15"}.contains(code)) ops << QStringLiteral("boots");
    if (QStringList{"H02", "H03", "H04", "H05", "H09", "H13", "H16"}.contains(code)) ops << QStringLiteral("diagnostics");
    if (code == QStringLiteral("H06")) ops << QStringLiteral("nvme_deep");
    return ops;
}

diag::Report reportFor(const QString &code, const QHash<QString, QJsonObject> &d) {
    namespace ad = advanceddiagnostics;
    namespace er = evidencereports;
    const QJsonArray boots = d.value(QStringLiteral("boots")).value(QStringLiteral("boots")).toArray();
    const QJsonObject ev = d.value(QStringLiteral("diagnostics"));
    if (code == QStringLiteral("H01")) return ad::bootRegression(boots, ad::legacyVersions(), ad::currentVersions());
    if (code == QStringLiteral("H02")) return er::mceAtlas(ev, coreMap());
    if (code == QStringLiteral("H03")) return er::aerDelta(ev, diagsrc::loadJson(diagsrc::dataDir() + QStringLiteral("/aer-last-sample.json")));
    if (code == QStringLiteral("H04")) return er::xidCases(ev);
    if (code == QStringLiteral("H05")) return er::f2fsGuard(ev, er::f2fsVolumes(ev), diagsrc::savedEvents());
    if (code == QStringLiteral("H06")) return ad::nvmeHealth(d.value(QStringLiteral("nvme_deep")));
    if (code == QStringLiteral("H09")) return er::radioLedger(ev, diagsrc::savedEvents());
    if (code == QStringLiteral("H10")) return ad::usbStability(diagsrc::savedKernel(), ad::usbPresent());
    if (code == QStringLiteral("H12")) return ad::sleepMatrix(ad::sleepInputsFromSystem(), boots);
    if (code == QStringLiteral("H13")) return ad::coCrossLink(ad::coRecordsFromSystem(), boots, ev);
    if (code == QStringLiteral("H15")) return ad::flightCoverage(boots);
    if (code == QStringLiteral("H16")) return ad::gpuMemory(ev, ad::nvidiaGpus(), {});
    if (code == QStringLiteral("S06")) return diagnostictools::nvidiaParams();
    return systemdiagnostics::byCode(code);
}

void fetch(QObject *ctx, const QStringList &ops, std::function<void(QHash<QString, QJsonObject>, QStringList)> done) {
    struct State { QStringList left; QHash<QString, QJsonObject> got; QStringList failed; std::function<void(QHash<QString, QJsonObject>, QStringList)> done; };
    auto st = std::make_shared<State>(State{ops, {}, {}, std::move(done)});
    auto step = std::make_shared<std::function<void()>>();
    *step = [ctx, st, step] {
        if (st->left.isEmpty()) { st->done(st->got, st->failed); *step = nullptr; return; }
        const QString op = st->left.takeFirst();
        // nvme-cli runs twice per controller (up to 8): its own, longer limit.
        privileged::run(privileged::helperPath(QStringLiteral("health-reader")), QJsonObject{{"op", op}}, ctx,
                        [st, step, op](const privileged::Result &r) {
            if (r.ok()) st->got.insert(op, r.json);
            else st->failed << QStringLiteral("%1: %2").arg(op, r.message());
            if (*step) (*step)();
        }, op == QStringLiteral("nvme_deep") ? 90000 : 30000);
    };
    (*step)();
}

}  // namespace investigations

AdvancedHealthPage::AdvancedHealthPage(QWidget *parent) : QWidget(parent) {
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(8, 7, 8, 7);
    auto *help = new QLabel(tr("Investigations read what Centurion already records (boot history, saved kernel logs, sleep log, CO Search records) and "
                               "say what it means. Nothing runs until you press Run; no hardware setting is changed."));
    help->setWordWrap(true);
    help->setProperty("role", "muted");
    v->addWidget(help);
    auto *top = new QHBoxLayout;
    feature_ = new QComboBox;
    for (const auto &[code, name] : features())
        feature_->addItem(QString::fromLatin1(code) + QStringLiteral(" · ") + tr(name), QString::fromLatin1(code));
    top->addWidget(feature_, 1);
    category_ = new QComboBox;
    for (const advanceddiagnostics::TriageCategory &c : advanceddiagnostics::triageCategories())
        category_->addItem(QCoreApplication::translate("advanceddiagnostics", c.label), c.key);
    category_->setToolTip(tr("What is going wrong? Triage runs the checks that fit and collects what needs a look."));
    top->addWidget(category_, 1);
    scan_ = new QPushButton(tr("Run"));
    top->addWidget(scan_);
    gpuQuery_ = new QPushButton(tr("Query ECC (wakes the GPU)"));
    gpuQuery_->setToolTip(tr("Runs nvidia-smi -q -d ECC,ROW_REMAPPER,PAGE_RETIREMENT. A sleeping dGPU is woken for it."));
    top->addWidget(gpuQuery_);
    v->addLayout(top);
    message_ = new QLabel;
    message_->setWordWrap(true);
    message_->setProperty("role", "muted");
    v->addWidget(message_);

    stack_ = new QStackedWidget;
    view_ = new DiagReportView;
    stack_->addWidget(view_);
    // H17: incidents on the left, what one means and its note on the right.
    incidentPage_ = new QWidget;
    auto *ih = new QVBoxLayout(incidentPage_);
    ih->setContentsMargins(0, 0, 0, 0);
    auto *filterRow = new QHBoxLayout;
    incidentFilter_ = new QComboBox;
    incidentFilter_->addItem(tr("Open incidents"), QStringLiteral("open"));
    incidentFilter_->addItem(tr("All incidents"), QStringLiteral("all"));
    filterRow->addWidget(new QLabel(tr("Show:")));
    filterRow->addWidget(incidentFilter_);
    filterRow->addStretch();
    ih->addLayout(filterRow);
    auto *split = new QSplitter(Qt::Horizontal);
    incidents_ = new QTreeWidget;
    incidents_->setHeaderLabels({tr("When"), tr("Kind"), tr("Event"), tr("Status")});
    incidents_->setRootIsDecorated(false);
    incidents_->setAlternatingRowColors(true);
    incidents_->setUniformRowHeights(true);
    split->addWidget(incidents_);
    auto *right = new QWidget;
    auto *rv = new QVBoxLayout(right);
    rv->setContentsMargins(6, 0, 0, 0);
    incidentInfo_ = new QTextBrowser;
    incidentInfo_->setOpenLinks(false);
    rv->addWidget(incidentInfo_, 1);
    auto *srow = new QHBoxLayout;
    incidentStatus_ = new QComboBox;
    for (const QString &s : advanceddiagnostics::incidentStatuses()) incidentStatus_->addItem(statusLabel(s), s);
    srow->addWidget(new QLabel(tr("Status:")));
    srow->addWidget(incidentStatus_, 1);
    rv->addLayout(srow);
    note_ = new QPlainTextEdit;
    note_->setPlaceholderText(tr("Your notes: what you tried, what changed, what happened next (max 2000 characters). Kept privately; the kernel log is not touched."));
    note_->setMaximumHeight(110);
    rv->addWidget(note_);
    save_ = new QPushButton(tr("Save note"));
    rv->addWidget(save_, 0, Qt::AlignRight);
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    ih->addWidget(split, 1);
    stack_->addWidget(incidentPage_);
    v->addWidget(stack_, 1);

    connect(feature_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { updateMode(); });
    connect(scan_, &QPushButton::clicked, this, [this] { run(); });
    connect(gpuQuery_, &QPushButton::clicked, this, [this] { activeGpuQuery(); });
    connect(incidents_, &QTreeWidget::currentItemChanged, this, [this] { selectIncident(); });
    connect(incidentFilter_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { refreshIncidents(); });
    connect(save_, &QPushButton::clicked, this, [this] { saveNote(); });
    updateMode();
}

void AdvancedHealthPage::updateMode() {
    const QString code = feature_->currentData().toString();
    const bool incidents = code == QStringLiteral("H17");
    stack_->setCurrentWidget(incidents ? incidentPage_ : static_cast<QWidget *>(view_));
    category_->setVisible(code == QStringLiteral("H18"));
    gpuQuery_->setVisible(code == QStringLiteral("H16"));
    gpuQuery_->setEnabled(false);  // after the passive part has run
    scan_->setText(incidents ? tr("Reload") : tr("Run"));
    message_->clear();
    if (incidents) refreshIncidents();
    else view_->setMessage(code == QStringLiteral("H18") ? tr("Pick what is going wrong and press Run.") : tr("Press Run to read the sources of this investigation."));
}

void AdvancedHealthPage::run() {
    if (busy_) return;
    const QString code = feature_->currentData().toString();
    if (code == QStringLiteral("H17")) { refreshIncidents(); return; }
    QStringList codes{code};
    QString label;
    if (code == QStringLiteral("H18")) {
        for (const advanceddiagnostics::TriageCategory &c : advanceddiagnostics::triageCategories())
            if (c.key == category_->currentData().toString()) { codes = c.checks; label = category_->currentText(); }
    }
    QStringList ops;
    for (const QString &c : codes) for (const QString &op : investigations::opsFor(c)) if (!ops.contains(op)) ops << op;
    busy_ = true;
    scan_->setEnabled(false);
    feature_->setEnabled(false);  // a late reply must not land under another investigation
    category_->setEnabled(false);
    view_->setMessage(tr("Reading…"));
    message_->setText(ops.isEmpty() ? QString() : tr("Reading %1 through health-reader (read-only)…").arg(ops.join(QStringLiteral(", "))));
    investigations::fetch(this, ops, [this, code, codes, label](const QHash<QString, QJsonObject> &data, const QStringList &failed) {
        busy_ = false;
        scan_->setEnabled(true);
        feature_->setEnabled(true);
        category_->setEnabled(true);
        lastEvidence_ = data.value(QStringLiteral("diagnostics"));
        if (code == QStringLiteral("H18")) {
            QList<diag::Report> reports;
            for (const QString &c : codes) reports << investigations::reportFor(c, data);
            view_->setReport(advanceddiagnostics::triage(label, reports));
        } else {
            view_->setReport(investigations::reportFor(code, data));
        }
        gpuQuery_->setEnabled(code == QStringLiteral("H16"));
        message_->setText(failed.isEmpty() ? QString() : tr("Not available: %1. The report says Unknown where that data was needed.").arg(failed.join(QStringLiteral("; "))));
    });
}

void AdvancedHealthPage::activeGpuQuery() {
    if (feature_->currentData().toString() != QStringLiteral("H16") || busy_) return;
    QString exe = QStandardPaths::findExecutable(QStringLiteral("nvidia-smi"));
    if (exe.isEmpty()) exe = QStandardPaths::findExecutable(QStringLiteral("nvidia-smi"), {QStringLiteral("/opt/bin"), QStringLiteral("/usr/bin")});
    if (exe.isEmpty()) { message_->setText(tr("nvidia-smi is not installed.")); return; }
    busy_ = true;
    gpuQuery_->setEnabled(false);
    scan_->setEnabled(false);
    message_->setText(tr("Asking the driver (this wakes the GPU)…"));
    auto *p = new QProcess(this);
    p->setProcessChannelMode(QProcess::MergedChannels);
    QPointer<QProcess> guard(p);
    auto finish = [this, p](const QString &out, bool ok) {
        busy_ = false;
        scan_->setEnabled(true);
        gpuQuery_->setEnabled(true);
        view_->setReport(advanceddiagnostics::gpuMemory(lastEvidence_, advanceddiagnostics::nvidiaGpus(), ok ? out : QString()));
        message_->setText(ok ? QString() : tr("nvidia-smi failed or timed out; ECC state unknown."));
        p->deleteLater();
    };
    connect(p, &QProcess::finished, this, [p, finish](int code, QProcess::ExitStatus st) {
        const bool ok = st == QProcess::NormalExit && code == 0 && !p->property("timedOut").toBool();
        finish(QString::fromUtf8(p->readAll().left(64 * 1024)), ok);
    });
    connect(p, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError e) { if (e == QProcess::FailedToStart) finish({}, false); });
    QTimer::singleShot(10000, p, [guard] { if (guard && guard->state() != QProcess::NotRunning) { guard->setProperty("timedOut", true); guard->kill(); } });
    p->start(exe, {QStringLiteral("-q"), QStringLiteral("-d"), QStringLiteral("ECC,ROW_REMAPPER,PAGE_RETIREMENT")});
}

void AdvancedHealthPage::refreshIncidents() {
    const QString keep = incidents_->currentItem() ? incidents_->currentItem()->data(0, Qt::UserRole).toString() : QString();
    const bool openOnly = incidentFilter_->currentData().toString() == QStringLiteral("open");
    incidents_->clear();
    const auto list = advanceddiagnostics::incidents(diagsrc::savedEvents(), advanceddiagnostics::incidentNotes());
    QTreeWidgetItem *select = nullptr;
    for (const advanceddiagnostics::Incident &i : list) {
        if (openOnly && i.status == QStringLiteral("Resolved")) continue;
        auto *it = new QTreeWidgetItem({i.line.time, i.line.kind.toUpper(), i.line.title, statusLabel(i.status)});
        it->setData(0, Qt::UserRole, i.line.key());
        it->setData(1, Qt::UserRole, i.line.text);
        it->setToolTip(2, i.line.text);
        if (i.line.level == QStringLiteral("critical")) it->setForeground(1, QColor(theme::DANGER));
        incidents_->addTopLevelItem(it);
        if (i.line.key() == keep) select = it;
    }
    for (int c = 0; c < 3; ++c) incidents_->resizeColumnToContents(c);
    message_->setText(list.isEmpty() ? tr("No classified event is saved yet (Health → Events & alerts records them while Centurion runs).")
                                     : tr("%1 saved event(s), newest first.").arg(list.size()));
    if (select) incidents_->setCurrentItem(select);
    else if (incidents_->topLevelItemCount()) incidents_->setCurrentItem(incidents_->topLevelItem(0));
    else selectIncident();
}

void AdvancedHealthPage::selectIncident() {
    QTreeWidgetItem *it = incidents_->currentItem();
    save_->setEnabled(it);
    if (!it) { incidentInfo_->setPlainText(tr("Select an event.")); note_->clear(); return; }
    const QString key = it->data(0, Qt::UserRole).toString();
    const QString title = it->text(2), text = it->data(1, Qt::UserRole).toString();
    const int code = QRegularExpression(QStringLiteral(R"(Xid (\d+))")).match(title).captured(1).toInt();
    const QJsonObject ev{{"kind", it->text(1).toLower()}, {"code", code}, {"title", title}, {"text", text}};
    incidentInfo_->setHtml(eventinfo::html(ev, title));
    // The saved status and note of this incident: an unchanged Save must never erase them.
    const QJsonObject saved = advanceddiagnostics::incidentNote(key);
    note_->setPlainText(saved.value(QStringLiteral("note")).toString());
    const int at = incidentStatus_->findData(saved.value(QStringLiteral("status")).toString(QStringLiteral("New")));
    incidentStatus_->setCurrentIndex(at >= 0 ? at : 0);
}

void AdvancedHealthPage::saveNote() {
    QTreeWidgetItem *it = incidents_->currentItem();
    if (!it) return;
    const QString err = advanceddiagnostics::saveIncident(it->data(0, Qt::UserRole).toString(), incidentStatus_->currentData().toString(), note_->toPlainText());
    message_->setText(err.isEmpty() ? tr("Note saved.") : tr("Not saved: %1").arg(err));
    if (err.isEmpty()) it->setText(3, incidentStatus_->currentText());
}
