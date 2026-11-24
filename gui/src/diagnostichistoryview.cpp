#include "diagnostichistoryview.h"
#include "diagnostichistorydata.h"
#include "diagreport.h"
#include "diagsources.h"
#include "theme.h"
#include "workspacestate.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QMap>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <algorithm>

namespace diagnostichistory {
namespace {
QString tr(const char *s) { return QCoreApplication::translate("DiagnosticHistory",s); }
struct RecordedEvent {
    QString boot, reportedBoot, time, level, title, detail, id;
    quint64 sequence;
    RecordSource source;
};
QString logName(RecordSource source) {
    return source == RecordSource::HealthLog ? QStringLiteral("health.log") : QStringLiteral("kernel.log");
}
QList<RecordedEvent> recordedEvents() {
    QList<RecordedEvent> out; QSet<QByteArray> seen;
    const auto add=[&](const QList<diagsrc::LogLine>& lines, RecordSource source) {
        const auto name = logName(source);
        for(const auto &line:lines) {
            // Exact duplicate only. Different text sharing a sequence is retained,
            // and a legacy boot prefix is never promoted to a full UUID.
            const auto key=QCryptographicHash::hash(QJsonDocument(QJsonArray{name,line.boot,QString::number(line.seq),line.time,line.level,line.kind,line.title,line.text}).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256);
            if(seen.contains(key)) continue;
            seen.insert(key);
            out.append({QString::fromStdString(fullBoot(line.boot.toStdString())),diag::redactPrivateText(line.boot).left(80),line.time.left(80),line.level.left(80),diag::redactPrivateText(line.title).left(300),diag::redactPrivateText(line.text).left(8192),QString::fromLatin1(key.toHex()),line.seq,source});
        }
    };
    add(diagsrc::savedEvents(1000), RecordSource::HealthLog);
    add(diagsrc::savedKernel(1000), RecordSource::KernelLog);
    return out;
}
QString recordText(const QJsonArray &r) {
    if(r.isEmpty()) return tr("Unsupported record format");
    const auto kind=r.at(0).toString();
    if(kind==QLatin1String("finding") && r.size()==5)
        return tr("Finding")+QStringLiteral(": ")+r.at(2).toString()+QStringLiteral(" · ")+diag::levelName(static_cast<diag::Level>(std::clamp(r.at(1).toInt(2),0,4)))+QStringLiteral("\n")+r.at(3).toString()+QStringLiteral("\n")+r.at(4).toString();
    if(kind==QLatin1String("table") && r.size()==4) return tr("Readings")+QStringLiteral(": ")+r.at(1).toString()+QStringLiteral("\n")+r.at(3).toString();
    if(kind==QLatin1String("row") && r.size()==5) {
        QStringList fields; const auto headers=r.at(2).toArray(),cells=r.at(3).toArray();
        for(qsizetype i=0;i<cells.size();++i) fields<<(i<headers.size()?headers.at(i).toString():QString::number(i+1))+QStringLiteral(": ")+cells.at(i).toString();
        return r.at(1).toString()+QStringLiteral(" · ")+diag::levelName(static_cast<diag::Level>(std::clamp(r.at(4).toInt(2),0,4)))+QStringLiteral(" · ")+fields.join(QStringLiteral(" | "));
    }
    if(kind==QLatin1String("limit") && r.size()==2) return tr("Evidence limit")+QStringLiteral(": ")+r.at(1).toString();
    return tr("Unsupported record format");
}
QString describe(const Entry &e) {
    QString out=tr("Report: %1\nCapture time: %2\nCapture boot: %3\nLocale: %4\nNote: %5\n\n").arg(e.code,e.time,e.boot.isEmpty()?tr("Unknown / legacy"):e.boot,e.locale,e.note);
    if(e.boot.isEmpty()&&!e.reportedBoot.isEmpty()) out+=tr("Recorded boot label (not an identity): %1\n").arg(e.reportedBoot);
    for(const auto &v:e.snapshot.value("records").toArray()) {
        if(out.size()>256*1024) {out+=tr("Display limit reached; archived snapshot is unchanged.");break;}
        out+=diag::redactPrivateText(recordText(v.toArray()))+QStringLiteral("\n\n");
    }
    return out;
}
QString compare(const Entry&a,const Entry&b) {
    if(a.code!=b.code || a.locale.isEmpty() || a.locale!=b.locale) return tr("Comparison requires the same report code and locale.");
    QString out=tr("Same report type; changed records are removed/added. Not a causal or device identity comparison.\n\n");
    switch(relation(a.boot.toStdString(),b.boot.toStdString())) {
    case Relation::SameCaptureBoot:out+=tr("Same capture boot.\n");break;
    case Relation::DifferentCaptureBoot:out+=tr("Different capture boots.\n");break;
    default:out+=tr("Capture boot relationship unknown.\n");break;
    }
    if(a.snapshot.value("verdict")!=b.snapshot.value("verdict")) out+=tr("Report verdict changed.\n");
    QMap<QByteArray,int> delta;
    for(const auto &v:a.snapshot.value("records").toArray()) --delta[QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact)];
    for(const auto &v:b.snapshot.value("records").toArray()) ++delta[QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact)];
    int n=0;
    for(auto it=delta.cbegin();it!=delta.cend();++it) {
        if(!it.value()) continue;
        if(++n>1000 || out.size()>256*1024) {out+=tr("Display limit reached; archived snapshot is unchanged.");break;}
        out+=QStringLiteral("%1%2 × %3\n").arg(it.value()>0?QStringLiteral("+"):QStringLiteral("−")).arg(qAbs(it.value())).arg(diag::redactPrivateText(recordText(QJsonDocument::fromJson(it.key()).array())));
    }
    if(!n) out+=tr("No structured record differences.");
    return out;
}
}
void open(QWidget *owner) {
    const auto archive=load(diagsrc::dataDir()+QStringLiteral("/diagnostic-history"));
    const auto events=recordedEvents();
    QDialog dlg(owner); dlg.setWindowTitle(tr("Cross-Boot Diagnostic History"));dlg.resize(1050,700);
    auto *layout=new QVBoxLayout(&dlg);
    auto *note=new QLabel(tr("Saved local reports and bounded saved kernel/health log tails only. Capture boot and event boot are different types of provenance. Sharing a boot does not prove a common device or cause. Wall-clock ordering is descriptive only."));note->setWordWrap(true);layout->addWidget(note);
    auto *status=new QLabel(tr("%1 snapshots · %2 rejected records or files · %3 saved log entries (up to 1000 per source)").arg(archive.entries.size()).arg(archive.rejected).arg(events.size()));layout->addWidget(status);
    if(archive.limited) layout->addWidget(new QLabel(tr("Archive scan limit reached; some files were not read.")));
    auto *filter=new QLineEdit;filter->setPlaceholderText(tr("Filter report, boot, source, time, event or note…"));filter->setMaxLength(512);layout->addWidget(filter);
    auto *sources = new QComboBox;
    sources->addItem(tr("All sources"), int(RecordSource::All));
    sources->addItem(tr("Archived reports"), int(RecordSource::Report));
    sources->addItem(tr("Health log"), int(RecordSource::HealthLog));
    sources->addItem(tr("Kernel log"), int(RecordSource::KernelLog));
    auto *boots = new QComboBox;
    boots->addItem(tr("Any boot identity"), int(BootScope::All));
    boots->addItem(tr("Full boot identity"), int(BootScope::Identified));
    boots->addItem(tr("Unresolved boot identity"), int(BootScope::Unresolved));
    auto *scopes = new QHBoxLayout;
    auto *sourceLabel = new QLabel(tr("Source:"));
    sourceLabel->setBuddy(sources);
    scopes->addWidget(sourceLabel);
    scopes->addWidget(sources, 1);
    auto *bootLabel = new QLabel(tr("Boot identity:"));
    bootLabel->setBuddy(boots);
    scopes->addWidget(bootLabel);
    scopes->addWidget(boots, 1);
    layout->addLayout(scopes);
    auto *split=new QSplitter(Qt::Horizontal);layout->addWidget(split,1);
    auto *tree=new QTreeWidget;tree->setHeaderLabels({tr("Boot / report / event"),tr("Relationship")});tree->setUniformRowHeights(true);tree->header()->setSectionResizeMode(0,QHeaderView::Stretch);split->addWidget(tree);
    auto *detail=new QPlainTextEdit;detail->setReadOnly(true);split->addWidget(detail);split->setStretchFactor(1,2);
    auto *bar=new QHBoxLayout;auto *left=new QComboBox;auto *right=new QComboBox;auto *diff=new QPushButton(tr("Compare snapshots"));bar->addWidget(left,1);bar->addWidget(right,1);bar->addWidget(diff);layout->addLayout(bar);
    constexpr int recordIdentityRole = Qt::UserRole + 1;
    QMap<QString,QTreeWidgetItem*> groups;
    for(qsizetype i=0;i<archive.entries.size();++i) {
        const auto &e=archive.entries.at(i);
        // Unknown boot entries each get their own group. No prefix matching.
        const auto groupKey=e.boot.isEmpty()?e.id+QStringLiteral("/unknown"):e.boot;
        if(!groups.contains(groupKey)) {
            auto *g=new QTreeWidgetItem(tree,{e.boot.isEmpty()?tr("Unknown capture boot"):e.boot,e.boot.isEmpty()?tr("Unlinked capture"):tr("Full boot identity")});
            g->setData(0,Qt::UserRole,-1);groups.insert(groupKey,g);
        }
        const auto label=e.time+QStringLiteral(" · ")+e.code+QStringLiteral(" · ")+e.title;
        auto *item=new QTreeWidgetItem(groups.value(groupKey),{label,tr("Archived report (capture boot)")});item->setData(0,Qt::UserRole,int(i));
        item->setData(0, recordIdentityRole, QStringLiteral("report/") + e.id);
        left->addItem(label,int(i));right->addItem(label,int(i));
    }
    for(qsizetype i=0;i<events.size();++i) {
        const auto &e=events.at(i);
        const auto groupKey=e.boot.isEmpty()?QStringLiteral("/unresolved-events"):e.boot;
        if(!groups.contains(groupKey)) {
            auto *g=new QTreeWidgetItem(tree,{e.boot.isEmpty()?tr("Unresolved event boots (not one session)"):e.boot,e.boot.isEmpty()?tr("No boot identity"):tr("Full boot identity")});
            g->setData(0,Qt::UserRole,-1);groups.insert(groupKey,g);
        }
        const auto label=e.time+QStringLiteral(" · ")+logName(e.source)+QStringLiteral(" · ")+(e.title.isEmpty()?e.detail.left(160):e.title);
        auto *item=new QTreeWidgetItem(groups.value(groupKey),{label,e.boot.isEmpty()?tr("Unlinked saved event"):tr("Recorded event boot")});
        item->setData(0,Qt::UserRole,-2-int(i));
        item->setData(0, recordIdentityRole, QStringLiteral("event/") + e.id);
    }
    if(right->count()>1) right->setCurrentIndex(1);
    diff->setEnabled(left->count()>1);
    QObject::connect(tree,&QTreeWidget::currentItemChanged,&dlg,[&](QTreeWidgetItem *item){
        const int i=item?item->data(0,Qt::UserRole).toInt():-1;
        if(i>=0 && i<archive.entries.size()) detail->setPlainText(describe(archive.entries.at(i)));
        else if(i<=-2 && -2-i<events.size()) {
            const auto &e=events.at(-2-i);
            detail->setPlainText(tr("Source: %1\nRecorded time: %2\nEvent boot: %3\nSequence: %4\nLevel: %5\nRecord ID: %6\n\n%7\n%8").arg(logName(e.source),e.time,e.boot.isEmpty()?tr("Unknown / legacy"):e.boot,QString::number(e.sequence),e.level,e.id,e.title,e.detail)
                +(e.boot.isEmpty()?QStringLiteral("\n")+tr("Recorded boot label (not an identity): %1").arg(e.reportedBoot):QString()));
        } else detail->setPlainText(tr("Select an archived report or saved event."));
    });
    QObject::connect(diff,&QPushButton::clicked,&dlg,[&]{
        const int a=left->currentData().toInt(),b=right->currentData().toInt();
        if(a>=0&&b>=0&&a<archive.entries.size()&&b<archive.entries.size()) detail->setPlainText(compare(archive.entries.at(a),archive.entries.at(b)));
    });
    const auto applyFilter=[&]{
        const auto q=filter->text().trimmed();
        const auto sourceFilter = static_cast<RecordSource>(sources->currentData().toInt());
        const auto bootFilter = static_cast<BootScope>(boots->currentData().toInt());
        for(auto *group:groups) {
            bool any=false;
            for(int i=0;i<group->childCount();++i) {
                auto *item=group->child(i);const auto n=item->data(0,Qt::UserRole).toInt();
                bool match=q.isEmpty()||item->text(0).contains(q,Qt::CaseInsensitive)||group->text(0).contains(q,Qt::CaseInsensitive);
                if (n >= 0) {
                    const auto &e = archive.entries.at(n);
                    match |= e.reportedBoot.contains(q, Qt::CaseInsensitive) || e.note.contains(q, Qt::CaseInsensitive);
                    match &= matchesScope(RecordSource::Report, !e.boot.isEmpty(), sourceFilter, bootFilter);
                } else {
                    const auto &e = events.at(-2 - n);
                    match |= e.reportedBoot.contains(q, Qt::CaseInsensitive) || e.detail.contains(q, Qt::CaseInsensitive)
                        || e.level.contains(q, Qt::CaseInsensitive);
                    match &= matchesScope(e.source, !e.boot.isEmpty(), sourceFilter, bootFilter);
                }
                item->setHidden(!match);any|=match;
                if(!match&&tree->currentItem()==item) {tree->setCurrentItem(nullptr);detail->clear();}
            }
            group->setHidden(!any);if (any && (!q.isEmpty() || sourceFilter != RecordSource::All || bootFilter != BootScope::All))
                group->setExpanded(true);
        }
    };
    const auto key=workspace::key(theme::experimentalEnabled(),QStringLiteral("health/cross-boot-history"));
    workspace::rememberSplitter(split,key+QStringLiteral("/split"));
    const auto saved = workspace::read(key);
    filter->setText(saved.value("filter").toString());
    sources->setCurrentIndex(std::max(0, sources->findData(saved.value("source").toInt(-1))));
    boots->setCurrentIndex(std::max(0, boots->findData(saved.value("boot_scope").toInt(-1))));
    QObject::connect(filter, &QLineEdit::textChanged, &dlg, applyFilter);
    QObject::connect(sources, &QComboBox::currentIndexChanged, &dlg, applyFilter);
    QObject::connect(boots, &QComboBox::currentIndexChanged, &dlg, applyFilter);
    tree->expandAll();
    applyFilter();
    workspace::restoreSelection(tree, saved.value("selected_record").toString(), recordIdentityRole);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Close);QObject::connect(buttons,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);layout->addWidget(buttons);
    dlg.exec();
    const auto *current = tree->currentItem();
    workspace::write(key, QJsonObject{{"filter", filter->text()}, {"source", sources->currentData().toInt()},
                                      {"boot_scope", boots->currentData().toInt()},
                                      {"selected_record", current ? current->data(0, recordIdentityRole).toString() : QString()}});
}
}
