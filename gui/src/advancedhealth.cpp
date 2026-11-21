#include "advancedhealth.h"
#include "advanceddiagnostics.h"
#include "privileged.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QPointer>

AdvancedHealthPage::AdvancedHealthPage(QWidget *parent) : QWidget(parent) {
    auto *v=new QVBoxLayout(this);
    v->setContentsMargins(8,7,8,7);
    auto *help=new QLabel(tr("On-demand investigations. Data may be incomplete; an event near a fault does not prove its cause. No hardware settings are changed."));
    help->setWordWrap(true);
    help->setProperty("role","muted");
    v->addWidget(help);
    auto *top=new QHBoxLayout;
    feature_=new QComboBox;
    const QList<QPair<const char*,const char*>> names={
        {"H01","Boot Regression Detective"},{"H06","NVMe Deep Health"},
        {"H10","USB Stability Watch"},{"H12","Sleep/Resume Failure Matrix"},
        {"H13","CO Evidence Cross-Link"},{"H15","Pre-crash Coverage Inspector"},
        {"H16","GPU Memory Reliability View"},{"H17","Incident Lifecycle"},
        {"H18","Guided Diagnostic Wizard"}};
    for (const auto &entry : names) feature_->addItem(QString::fromLatin1(entry.first)+QStringLiteral(" · ")+
        QCoreApplication::translate("AdvancedHealthPage",entry.second), QString::fromLatin1(entry.first));
    top->addWidget(feature_,1);
    wizardCategory_ = new QComboBox;
    for (const auto &category : {"Xid/GSP", "MCE/CO", "NVMe/F2FS", "Wi-Fi/BT", "Sleep", "USB"})
        wizardCategory_->addItem(QString::fromLatin1(category));
    wizardCategory_->setToolTip(tr("Select the incident family; guidance never executes repair commands."));
    top->addWidget(wizardCategory_);
    scan_=new QPushButton(tr("Run diagnostic"));
    top->addWidget(scan_);
    gpuQuery_=new QPushButton(tr("Active NVIDIA ECC query (may wake GPU)"));
    gpuQuery_->setToolTip(tr("Opt-in: launches nvidia-smi only after you click; a sleeping dGPU may wake up."));
    top->addWidget(gpuQuery_);
    auto *copy=new QPushButton(tr("Copy"));
    top->addWidget(copy);
    v->addLayout(top);
    message_=new QLabel(tr("Choose a diagnostic and press Run. No data has been acquired."));
    message_->setWordWrap(true);
    message_->setProperty("role","muted");
    v->addWidget(message_);
    incidentRow_=new QWidget;
    auto *incidentLayout=new QVBoxLayout(incidentRow_);
    incidentLayout->setContentsMargins(0,0,0,0);
    auto *events=new QHBoxLayout;
    event_=new QComboBox;
    events->addWidget(event_,1);
    incidentStatus_=new QComboBox;
    for (const QString &s : {QStringLiteral("New"),QStringLiteral("Investigating"),QStringLiteral("Monitoring"),
                             QStringLiteral("Resolved"),QStringLiteral("Reopened")}) incidentStatus_->addItem(s);
    events->addWidget(incidentStatus_);
    incidentLayout->addLayout(events);
    note_=new QPlainTextEdit;
    note_->setPlaceholderText(tr("Private incident note (max 2000 characters). Original kernel history is never modified."));
    note_->setMaximumHeight(90);
    incidentLayout->addWidget(note_);
    save_=new QPushButton(tr("Save incident note"));
    incidentLayout->addWidget(save_);
    v->addWidget(incidentRow_);
    output_=new QPlainTextEdit;
    output_->setReadOnly(true);
    output_->setLineWrapMode(QPlainTextEdit::NoWrap);
    output_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    output_->setPlainText(tr("No report yet."));
    v->addWidget(output_,1);
    connect(feature_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int){updateMode();});
    connect(scan_,&QPushButton::clicked,this,[this]{run();});
    connect(copy,&QPushButton::clicked,this,[this]{QApplication::clipboard()->setText(output_->toPlainText());});
    connect(event_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int){selectIncident();});
    connect(save_,&QPushButton::clicked,this,[this]{saveNote();});
    connect(gpuQuery_,&QPushButton::clicked,this,[this]{activeGpuQuery();});
    updateMode();
}
void AdvancedHealthPage::updateMode() {
    const QString code=feature_->currentData().toString();
    incidentRow_->setVisible(code==QStringLiteral("H17"));
    gpuQuery_->setVisible(code==QStringLiteral("H16"));
    wizardCategory_->setVisible(code==QStringLiteral("H18"));
    scan_->setText(code==QStringLiteral("H17") ? tr("Load incidents") : tr("Run diagnostic"));
    if (code==QStringLiteral("H17")) refreshIncidents();
    else output_->setPlainText(tr("Press Run diagnostic to read this source."));
}
void AdvancedHealthPage::refreshIncidents() {
    event_->blockSignals(true);
    const QString selected=event_->currentData().toString();
    event_->clear();
    for (const QString &key : advanceddiagnostics::incidentKeys()) event_->addItem(key,key);
    if (event_->count()==0) event_->addItem(tr("No retained incident identifiers"),QString());
    const int at=event_->findData(selected);
    event_->setCurrentIndex(at<0 ? 0 : at);
    event_->blockSignals(false);
    selectIncident();
}
void AdvancedHealthPage::selectIncident() {
    const QString key=event_->currentData().toString();
    output_->setPlainText(key.isEmpty() ? tr("No retained, keyed event available. Run Health > Events & alerts first.") :
                                    advanceddiagnostics::incidentText(key));
    // Load this incident's *saved* values instead of resetting them to New/empty.
    // An unchanged Save must never silently erase an existing investigation.
    const QJsonObject saved = advanceddiagnostics::incidentNote(key);
    note_->setPlainText(saved.value(QStringLiteral("note")).toString());
    const int at = incidentStatus_->findText(saved.value(QStringLiteral("status")).toString(QStringLiteral("New")));
    incidentStatus_->setCurrentIndex(at >= 0 ? at : 0);
    save_->setEnabled(!key.isEmpty());
}
void AdvancedHealthPage::saveNote() {
    const QString key=event_->currentData().toString();
    if (key.isEmpty()) return;
    const QString result=advanceddiagnostics::saveIncident(key,incidentStatus_->currentText(),note_->toPlainText());
    message_->setText(result.isEmpty() ? tr("Private incident note saved.") : result);
    if (result.isEmpty()) output_->setPlainText(advanceddiagnostics::incidentText(key));
}
void AdvancedHealthPage::showReport(const QString &code, const QJsonObject &boots, const QJsonObject &evidence, const QJsonObject &nvme) {
    QString report = advanceddiagnostics::healthReport(code,boots,evidence,nvme);
    if (code == QStringLiteral("H18")) {
        const QString tag = wizardCategory_->currentText() + QStringLiteral(":");
        const QStringList lines = report.split(QLatin1Char('\n'));
        QStringList guidance;
        for (const QString &line : lines) if (line.startsWith(tag)) guidance << line;
        report = tr("GUIDED DIAGNOSTIC — %1\n\n1. Identify and record the incident and its boot ID.\n"
                    "2. Inspect the relevant evidence; never assume time proximity proves causality.\n"
                    "3. %2\n4. Save the result to an incident note (H17) and compare the next boot.\n\n"
                    "No changes or repairs have been performed.")
                  .arg(wizardCategory_->currentText(), guidance.isEmpty() ? tr("No guidance available for this category.") : guidance.join(QLatin1Char('\n')));
    }
    output_->setPlainText(report);
    message_->setText(tr("Read-only report finished. Missing data remains Unknown; not a clean-health verdict."));
    busy_=false;
    scan_->setEnabled(true);
    feature_->setEnabled(true);
}
void AdvancedHealthPage::run() {
    if (busy_) return;
    const QString code=feature_->currentData().toString();
    if (code==QStringLiteral("H17")) {refreshIncidents();return;}
    if (code==QStringLiteral("H10")||code==QStringLiteral("H18")) {showReport(code);return;}
    const QString op=code==QStringLiteral("H06") ? QStringLiteral("nvme_deep") :
                     code==QStringLiteral("H16") ? QStringLiteral("diagnostics") : QStringLiteral("boots");
    busy_=true;
    scan_->setEnabled(false);
    feature_->setEnabled(false); // a late response must not be displayed under a different diagnostic
    message_->setText(tr("Collecting requested read-only evidence…"));
    // Never try to silently fall back to zero events when polkit is denied.
    privileged::run(privileged::helperPath(QStringLiteral("health-reader")),QJsonObject{{"op",op}},this,
        [this,code](const privileged::Result &r) {
            if (!r.ok()) {
                if (code==QStringLiteral("H16")) {
                    showReport(code); // sysfs inventory is still readable without privileged kmsg
                    message_->setText(tr("Kernel events unavailable (%1). Passive PCI inventory only; ECC remains Unknown.").arg(r.message()));
                    return;
                }
                message_->setText(tr("Unavailable (not healthy): %1").arg(r.message()));
                busy_=false; scan_->setEnabled(true);feature_->setEnabled(true);return;
            }
            if (code==QStringLiteral("H13")) {
                const QJsonObject boots=r.json;
                privileged::run(privileged::helperPath(QStringLiteral("health-reader")),
                                QJsonObject{{"op",QStringLiteral("diagnostics")}},this,
                    [this,boots,code](const privileged::Result &d) {
                        if (!d.ok()) {
                            // Boot+CO history is still useful, but no new MCE
                            // scan can be claimed when permission is missing.
                            showReport(code,boots);
                            message_->setText(tr("MCE scan unavailable; CO history only, no core verdict."));
                        } else showReport(code,boots,d.json);
                    },20000);
                return;
            }
            if (code==QStringLiteral("H06")) showReport(code,{}, {},r.json);
            else if (code==QStringLiteral("H16")) showReport(code,{},r.json);
            else showReport(code,r.json);
        },code==QStringLiteral("H06") ? 90000 : 30000);
}
void AdvancedHealthPage::activeGpuQuery() {
    if (feature_->currentData().toString()!=QStringLiteral("H16") || busy_) return;
    const QString exe=QStandardPaths::findExecutable(QStringLiteral("nvidia-smi"));
    if (exe.isEmpty()) {message_->setText(tr("nvidia-smi is not installed; ECC data unavailable."));return;}
    message_->setText(tr("Explicit active NVIDIA query in progress; this can wake the discrete GPU."));
    busy_=true;gpuQuery_->setEnabled(false);scan_->setEnabled(false);
    auto *process=new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process,&QProcess::finished,this,[this,process](int code, QProcess::ExitStatus exitStatus){
        const bool timedOut=process->property("centurionTimedOut").toBool();
        const bool success=!timedOut && exitStatus==QProcess::NormalExit && code==0;
        const QString output=QString::fromUtf8(process->readAll().left(8192));
        output_->setPlainText(output_->toPlainText()+QStringLiteral("\n\nACTIVE NVIDIA ECC QUERY (user opted in)\n")+
            (success ? (output.isEmpty() ? QStringLiteral("No ECC output; UNKNOWN.") : output) :
                QStringLiteral("UNKNOWN: NVIDIA query failed or timed out; any partial output is not a verified ECC reading.")));
        busy_=false;gpuQuery_->setEnabled(true);scan_->setEnabled(true);
        message_->setText(success ? tr("Active query finished. ECC N/A is not evidence of healthy VRAM.") :
                                    tr("NVIDIA query failed or timed out; GPU memory status is unknown."));
        process->deleteLater();
    });
    connect(process,&QProcess::errorOccurred,this,[this,process](QProcess::ProcessError e){
        if (e!=QProcess::FailedToStart) return;
        message_->setText(tr("Failed to start NVIDIA query; ECC unavailable."));
        busy_=false;gpuQuery_->setEnabled(true);scan_->setEnabled(true);
        process->deleteLater();
    });
    QPointer<QProcess> guard(process);
    QTimer::singleShot(5000,process,[guard]{if(guard && guard->state()!=QProcess::NotRunning) {
        guard->setProperty("centurionTimedOut", true); guard->kill();
    }});
    process->start(exe,{QStringLiteral("--query-gpu=pci.bus_id,ecc.mode.current,ecc.errors.corrected.volatile.total,ecc.errors.uncorrected.volatile.total"),
                         QStringLiteral("--format=csv,noheader")});
}
