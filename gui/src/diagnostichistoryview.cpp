#include "diagnostichistoryview.h"
#include "diagnosticlogarchive.h"
#include "diagreport.h"
#include "diagsources.h"
#include "theme.h"
#include "workspacestate.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QMessageBox>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QMap>
#include <QSplitter>
#include <QSet>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <algorithm>

namespace diagnostichistory {
namespace {
QString tr(const char *s) { return QCoreApplication::translate("DiagnosticHistory",s); }
QString coverageText(const diagsrc::LogTail &tail, RecordSource source) {
    QStringList rows;
    const auto name = logName(source);
    for (int index = 0; index < 2; ++index) {
        const auto &file = tail.files.at(index);
        const auto filename = index == 0 ? name + QStringLiteral(".1") : name;
        QStringList facts;
        switch (file.state) {
        case diagsrc::LogFileState::Missing: facts << tr("File absent"); break;
        case diagsrc::LogFileState::Unreadable: facts << tr("Could not read safely"); break;
        case diagsrc::LogFileState::Read:
            facts << tr("Read");
            if (file.omittedBytes) facts << tr("%1 older bytes outside the scan limit").arg(file.omittedBytes);
            if (file.rejectedLines) facts << tr("%1 invalid or oversized lines omitted").arg(file.rejectedLines);
            if (file.incompleteTail) facts << tr("Incomplete final line omitted");
            break;
        }
        rows << tr("%1: %2").arg(filename, facts.join(QStringLiteral(" · ")));
    }
    if (tail.omittedLines) rows << tr("%1: %2 older records outside the display limit").arg(name).arg(tail.omittedLines);
    return rows.join(QLatin1Char('\n'));
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
QString captureTimeLabel(const Entry &e) {
    if (e.capturedAt.isValid()) return e.time;
    return e.time.isEmpty() ? tr("Unknown time") : tr("%1 (time unverified)").arg(e.time);
}
QString eventTimeLabel(const RecordedEvent &e) {
    if (e.time.isEmpty()) return tr("Unknown time");
    // Validate date and time separately because the log has no UTC offset.
    const bool valid = e.time.size() == 19 && e.time.at(10) == QLatin1Char(' ')
        && QDate::fromString(e.time.left(10), Qt::ISODate).isValid()
        && QTime::fromString(e.time.mid(11), QStringLiteral("HH:mm:ss")).isValid();
    return (valid ? tr("%1 (UTC offset unknown)") : tr("%1 (time unverified)")).arg(e.time);
}
QString describe(const Entry &e) {
    QString out=tr("Report: %1\nCapture time: %2\nCapture boot: %3\nLocale: %4\nNote: %5\n\n").arg(e.code,captureTimeLabel(e),e.boot.isEmpty()?tr("Unknown / legacy"):e.boot,e.locale,e.note);
    out += (e.imported ? tr("Source: Imported report archive") : tr("Source: Local report archive")) + QLatin1Char('\n');
    if(e.boot.isEmpty()&&!e.reportedBoot.isEmpty()) out+=tr("Recorded boot label (not an identity): %1\n").arg(e.reportedBoot);
    for(const auto &v:e.snapshot.value("records").toArray()) {
        if(out.size()>256*1024) {out+=tr("Display limit reached; archived snapshot is unchanged.");break;}
        out+=diag::redactPrivateText(recordText(v.toArray()))+QStringLiteral("\n\n");
    }
    return out;
}
bool comparable(const Entry &a, const Entry &b) {
    return a.id != b.id && a.code == b.code && !a.locale.isEmpty() && a.locale == b.locale;
}
QString compare(const Entry&a,const Entry&b) {
    if(!comparable(a,b)) return tr("Choose two different snapshots with the same report code and language.");
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
static bool openHistory(QWidget *owner) {
    const auto dir = archiveDirectory();
    const auto archive = load(dir);
    const auto health = diagsrc::savedLog(diagsrc::SavedLog::Health, 1000);
    const auto kernel = diagsrc::savedLog(diagsrc::SavedLog::Kernel, 1000);
    const auto importedLogs = readImportedLogs(dir);
    const auto events = recordedEvents(health, kernel, importedLogs.value_or(QList<RecordedEvent>{}));
    QDialog dlg(owner); dlg.setWindowTitle(tr("Cross-Boot Diagnostic History"));dlg.resize(1050,700);
    auto *layout=new QVBoxLayout(&dlg);
    auto *note=new QLabel(tr("Local and imported report snapshots, and bounded saved kernel and health logs. Capture boot and event boot are different types of provenance. Sharing a boot does not prove a common device or cause. Wall-clock ordering is descriptive only."));note->setWordWrap(true);layout->addWidget(note);
    const QString statusText = tr("%1 snapshots · %2 rejected records or files · %3 log entries in view")
        .arg(archive.entries.size()).arg(archive.rejected).arg(events.size());
    if (!importedLogs) layout->addWidget(new QLabel(tr("The imported log archive could not be read.")));
    auto *status = new QLabel(statusText);
    status->setWordWrap(true);
    auto *summary = new QHBoxLayout;
    summary->addWidget(status, 1);
    auto *showCoverage = new QPushButton(tr("Log coverage"));
    showCoverage->setCheckable(true);
    summary->addWidget(showCoverage);
    layout->addLayout(summary);
    auto *coverage = new QPlainTextEdit;
    coverage->setReadOnly(true);
    coverage->setMaximumHeight(130);
    coverage->setAccessibleName(tr("Log coverage"));
    coverage->setPlainText(coverageText(health, RecordSource::HealthLog) + QLatin1Char('\n')
                           + coverageText(kernel, RecordSource::KernelLog) + QLatin1Char('\n')
                           + tr("Events with a full boot identity are ordered by sequence within that boot."));
    coverage->hide();
    layout->addWidget(coverage);
    QObject::connect(showCoverage, &QPushButton::toggled, coverage, &QWidget::setVisible);
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
    constexpr int exactBootRole = Qt::UserRole + 1;
    QSet<QString> identifiedBoots;
    for (const auto &entry : archive.entries) if (!entry.boot.isEmpty()) identifiedBoots.insert(entry.boot);
    for (const auto &event : events) if (!event.boot.isEmpty()) identifiedBoots.insert(event.boot);
    QStringList bootIds = identifiedBoots.values();
    bootIds.sort();
    const auto currentBoot = full(diagsrc::bootId());
    for (const auto &id : bootIds) {
        boots->addItem(id == currentBoot ? tr("%1 (current boot)").arg(id) : id, int(BootScope::Identified));
        boots->setItemData(boots->count() - 1, id, exactBootRole);
    }
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
    constexpr int groupIdentityRole = Qt::UserRole + 2;
    left->setAccessibleName(tr("First snapshot"));
    right->setAccessibleName(tr("Second snapshot"));
    right->setPlaceholderText(tr("No compatible snapshot"));
    QMap<QString,QTreeWidgetItem*> groups;
    for(qsizetype i=0;i<archive.entries.size();++i) {
        const auto &e=archive.entries.at(i);
        // Unknown boot entries each get their own group. No prefix matching.
        const auto groupKey=e.boot.isEmpty()?e.id+QStringLiteral("/unknown"):e.boot;
        if(!groups.contains(groupKey)) {
            auto *g=new QTreeWidgetItem(tree,{e.boot.isEmpty()?tr("Unknown capture boot"):e.boot,e.boot.isEmpty()?tr("Unlinked capture"):tr("Full boot identity")});
            g->setData(0,Qt::UserRole,-1);g->setData(0,groupIdentityRole,groupKey);groups.insert(groupKey,g);
        }
        const auto label=captureTimeLabel(e)+QStringLiteral(" · ")+e.code+QStringLiteral(" · ")+e.title;
        auto *item=new QTreeWidgetItem(groups.value(groupKey),{label,
            e.imported ? tr("Imported report (capture boot)") : tr("Archived report (capture boot)")});item->setData(0,Qt::UserRole,int(i));
        item->setData(0, recordIdentityRole, QStringLiteral("report/") + e.id);
        left->addItem(label,int(i));
        left->setItemData(left->count()-1,e.id,recordIdentityRole);
    }
    for(qsizetype i=0;i<events.size();++i) {
        const auto &e=events.at(i);
        const auto groupKey=e.boot.isEmpty()?QStringLiteral("/unresolved-events"):e.boot;
        if(!groups.contains(groupKey)) {
            auto *g=new QTreeWidgetItem(tree,{e.boot.isEmpty()?tr("Unresolved event boots (not one session)"):e.boot,e.boot.isEmpty()?tr("No boot identity"):tr("Full boot identity")});
            g->setData(0,Qt::UserRole,-1);g->setData(0,groupIdentityRole,groupKey);groups.insert(groupKey,g);
        }
        const auto label=eventTimeLabel(e)+QStringLiteral(" · ")+logName(e.source)+QStringLiteral(" · ")+(e.title.isEmpty()?e.detail.left(160):e.title);
        auto *item = new QTreeWidgetItem(groups.value(groupKey), {label,
            e.imported ? tr("Imported log event") : e.boot.isEmpty() ? tr("Unlinked saved event") : tr("Recorded event boot")});
        item->setData(0,Qt::UserRole,-2-int(i));
        item->setData(0, recordIdentityRole, QStringLiteral("event/") + e.id);
    }
    const auto setComparisonTargets = [&](const QString &preferred) {
        right->clear();
        int differentBoot = -1;
        if (left->currentIndex() >= 0) {
            const auto &first = archive.entries.at(left->currentData().toInt());
            for (int i = 0; i < left->count(); ++i) {
                const int index = left->itemData(i).toInt();
                const auto &candidate = archive.entries.at(index);
                if (!comparable(first, candidate)) continue;
                if (differentBoot < 0 && !first.boot.isEmpty() && !candidate.boot.isEmpty() && first.boot != candidate.boot)
                    differentBoot = right->count();
                right->addItem(left->itemText(i), index);
                right->setItemData(right->count()-1, candidate.id, recordIdentityRole);
            }
        }
        const int savedTarget = preferred.isEmpty() ? -1 : right->findData(preferred, recordIdentityRole);
        right->setCurrentIndex(right->count() == 0 ? -1 : std::max(0, savedTarget >= 0 ? savedTarget : differentBoot));
        right->setEnabled(right->count() > 0);
        diff->setEnabled(right->currentIndex() >= 0);
    };
    QString desiredSelection;
    QObject::connect(tree,&QTreeWidget::currentItemChanged,&dlg,[&](QTreeWidgetItem *item){
        if (item) desiredSelection = item->data(0, recordIdentityRole).toString();
        const int i=item?item->data(0,Qt::UserRole).toInt():-1;
        if(i>=0 && i<archive.entries.size()) detail->setPlainText(describe(archive.entries.at(i)));
        else if(i<=-2 && -2-i<events.size()) {
            const auto &e=events.at(-2-i);
            QStringList files;
            if (e.rotated) files << logName(e.source) + QStringLiteral(".1");
            if (e.current) files << logName(e.source);
            if (e.imported) files << tr("Imported %1").arg(logName(e.source));
            detail->setPlainText(tr("Source: %1\nRecorded time: %2\nEvent boot: %3\nSequence: %4\nLevel: %5\nRecord ID: %6\n\n%7\n%8").arg(files.join(QStringLiteral(", ")),eventTimeLabel(e),e.boot.isEmpty()?tr("Unknown / legacy"):e.boot,QString::number(e.sequence),e.level,e.id,e.title,e.detail)
                +(e.boot.isEmpty()?QStringLiteral("\n")+tr("Recorded boot label (not an identity): %1").arg(e.reportedBoot):QString()));
        } else detail->setPlainText(tr("Select an archived report or saved event."));
    });
    QObject::connect(diff,&QPushButton::clicked,&dlg,[&]{
        const int a=left->currentData().toInt(),b=right->currentData().toInt();
        if(a>=0&&b>=0&&a<archive.entries.size()&&b<archive.entries.size()) detail->setPlainText(compare(archive.entries.at(a),archive.entries.at(b)));
    });
    QSet<QString> collapsedGroups;
    const auto applyFilter=[&]{
        const auto q=filter->text().trimmed();
        const auto sourceFilter = static_cast<RecordSource>(sources->currentData().toInt());
        const auto bootFilter = static_cast<BootScope>(boots->currentData().toInt());
        const auto exactBoot = boots->currentData(exactBootRole).toString();
        const bool filtering = !q.isEmpty() || sourceFilter != RecordSource::All || bootFilter != BootScope::All;
        int shownReports = 0, shownEvents = 0;
        for(auto *group:groups) {
            bool any=false;
            for(int i=0;i<group->childCount();++i) {
                auto *item=group->child(i);const auto n=item->data(0,Qt::UserRole).toInt();
                bool match=q.isEmpty()||item->text(0).contains(q,Qt::CaseInsensitive)||group->text(0).contains(q,Qt::CaseInsensitive);
                if (n >= 0) {
                    const auto &e = archive.entries.at(n);
                    match |= e.reportedBoot.contains(q, Qt::CaseInsensitive) || e.note.contains(q, Qt::CaseInsensitive);
                    match &= matchesScope(RecordSource::Report, !e.boot.isEmpty(), sourceFilter, bootFilter)
                        && (exactBoot.isEmpty() || e.boot == exactBoot);
                } else {
                    const auto &e = events.at(-2 - n);
                    match |= e.reportedBoot.contains(q, Qt::CaseInsensitive) || e.detail.contains(q, Qt::CaseInsensitive)
                        || e.level.contains(q, Qt::CaseInsensitive);
                    match &= matchesScope(e.source, !e.boot.isEmpty(), sourceFilter, bootFilter)
                        && (exactBoot.isEmpty() || e.boot == exactBoot);
                }
                if (match) {
                    if (n >= 0) ++shownReports;
                    else ++shownEvents;
                }
                if(!match&&tree->currentItem()==item) {tree->setCurrentItem(nullptr);detail->clear();}
                item->setHidden(!match);any|=match;
            }
            group->setHidden(!any);
            if (any) {
                const QSignalBlocker expansion(tree);
                group->setExpanded(filtering || !collapsedGroups.contains(group->data(0, groupIdentityRole).toString()));
            }
        }
        status->setText(filtering ? statusText + QLatin1Char('\n')
            + tr("Matching filters: %1 snapshots and %2 saved events.").arg(shownReports).arg(shownEvents) : statusText);
        if (!tree->currentItem()) workspace::restoreSelection(tree, desiredSelection, recordIdentityRole);
    };
    const auto key=workspace::key(theme::experimentalEnabled(),QStringLiteral("health/cross-boot-history"));
    workspace::rememberSplitter(split,key+QStringLiteral("/split"));
    const auto saved = workspace::read(key);
    showCoverage->setChecked(saved.value("show_log_coverage").toBool());
    desiredSelection = saved.value("selected_record").toString();
    for (const auto &value : saved.value("collapsed_groups").toArray()) {
        const auto id = value.toString();
        if (groups.contains(id) && collapsedGroups.size() < 128) collapsedGroups.insert(id);
    }
    const auto savedFirst = saved.value("compare_first").toString();
    const int firstIndex = left->findData(savedFirst, recordIdentityRole);
    if (firstIndex >= 0) left->setCurrentIndex(firstIndex);
    setComparisonTargets(saved.value("compare_second").toString());
    QObject::connect(left, &QComboBox::currentIndexChanged, &dlg, [&] {
        setComparisonTargets(right->currentData(recordIdentityRole).toString());
    });
    filter->setText(saved.value("filter").toString());
    sources->setCurrentIndex(std::max(0, sources->findData(saved.value("source").toInt(-1))));
    const auto savedBoot = saved.value("boot_id").toString();
    const int bootIndex = savedBoot.isEmpty() ? -1 : boots->findData(savedBoot, exactBootRole);
    boots->setCurrentIndex(bootIndex >= 0 ? bootIndex : std::max(0, boots->findData(saved.value("boot_scope").toInt(-1))));
    QObject::connect(filter, &QLineEdit::textChanged, &dlg, applyFilter);
    QObject::connect(sources, &QComboBox::currentIndexChanged, &dlg, applyFilter);
    QObject::connect(boots, &QComboBox::currentIndexChanged, &dlg, applyFilter);
    applyFilter();
    QObject::connect(tree, &QTreeWidget::itemCollapsed, &dlg, [&](QTreeWidgetItem *item) {
        if (const auto id = item->data(0, groupIdentityRole).toString(); !id.isEmpty()) collapsedGroups.insert(id);
    });
    QObject::connect(tree, &QTreeWidget::itemExpanded, &dlg, [&](QTreeWidgetItem *item) {
        collapsedGroups.remove(item->data(0, groupIdentityRole).toString());
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *import = buttons->addButton(tr("Import report archive…"), QDialogButtonBox::ActionRole);
    bool changed = false;
    QObject::connect(import, &QPushButton::clicked, &dlg, [&] {
        const auto path = QFileDialog::getOpenFileName(&dlg, tr("Import report archive"), {}, tr("JSON files (*.json)"));
        if (path.isEmpty()) return;
        QString message;
        switch (importArchive(dir, path)) {
        case ImportArchive::Imported: changed = true; dlg.accept(); return;
        case ImportArchive::AlreadyPresent: message = tr("These snapshots are already in the archive."); break;
        case ImportArchive::Invalid: message = tr("Choose a valid version 1 report archive containing snapshots of one report."); break;
        case ImportArchive::Conflict: message = tr("The import conflicts with a saved snapshot or a damaged archive."); break;
        case ImportArchive::Full: message = tr("Archive full. Import leaves saved snapshots in place."); break;
        case ImportArchive::Busy: message = tr("The archive is currently in use."); break;
        case ImportArchive::WriteError: message = tr("Could not save the imported archive."); break;
        }
        QMessageBox::information(&dlg, dlg.windowTitle(), message);
    });

    auto *importLog = buttons->addButton(tr("Import saved log…"), QDialogButtonBox::ActionRole);
    importLog->setToolTip(tr("Centurion health.log and kernel.log files up to 4 MiB. The import archive holds up to 1000 entries per source and 1 MiB total."));
    QObject::connect(importLog, &QPushButton::clicked, &dlg, [&] {
        const auto path = QFileDialog::getOpenFileName(&dlg, tr("Import saved log"));
        if (path.isEmpty()) return;
        const auto result = importSavedLog(dir, path);
        QString message;
        switch (result.status) {
        case ImportArchive::Imported:
        case ImportArchive::AlreadyPresent:
            message = tr("%1 imported · %2 duplicates · %3 unreadable or incomplete lines")
                .arg(result.added).arg(result.duplicates).arg(result.omitted);
            break;
        case ImportArchive::Invalid: message = tr("Choose a Centurion saved log up to 4 MiB with at least one complete entry."); break;
        case ImportArchive::Conflict: message = tr("The imported log archive could not be read."); break;
        case ImportArchive::Full: message = tr("Imported log archive full. Delete imported entries or choose a smaller file."); break;
        case ImportArchive::Busy: message = tr("The archive is currently in use."); break;
        case ImportArchive::WriteError: message = tr("Could not save the imported archive."); break;
        }
        QMessageBox::information(&dlg, dlg.windowTitle(), message);
        if (result.status == ImportArchive::Imported) { changed = true; dlg.accept(); }
    });

    auto *remove = buttons->addButton(tr("Delete imported record…"), QDialogButtonBox::ActionRole);
    const auto selectedImport = [&]() -> std::optional<int> {
        const auto *item = tree->currentItem();
        const int i = item ? item->data(0, Qt::UserRole).toInt() : -1;
        if (i >= 0 && i < archive.entries.size() && archive.entries.at(i).imported) return i;
        if (i <= -2 && -2 - i < events.size() && events.at(-2 - i).imported) return i;
        return std::nullopt;
    };
    remove->setEnabled(selectedImport().has_value());
    QObject::connect(tree, &QTreeWidget::currentItemChanged, &dlg, [&] {
        remove->setEnabled(selectedImport().has_value());
    });
    QObject::connect(remove, &QPushButton::clicked, &dlg, [&] {
        const auto i = selectedImport();
        if (!i || QMessageBox::question(&dlg, dlg.windowTitle(), tr("Delete this imported record from the local archive?"))
                          != QMessageBox::Yes) return;
        const bool deleted = *i >= 0
            ? removeImported(dir, archive.entries.at(*i).code, archive.entries.at(*i).id)
            : removeImportedLog(dir, events.at(-2 - *i).id);
        if (!deleted) {
            QMessageBox::warning(&dlg, dlg.windowTitle(), tr("Could not delete the imported record."));
            return;
        }
        changed = true;
        dlg.accept();
    });
    layout->addWidget(buttons);
    dlg.exec();
    QJsonArray collapsed;
    for (auto it = groups.cbegin(); it != groups.cend() && collapsed.size() < 128; ++it)
        if (collapsedGroups.contains(it.key())) collapsed.append(it.key());
    workspace::write(key, QJsonObject{{"filter", filter->text()}, {"source", sources->currentData().toInt()},
                                      {"boot_scope", boots->currentData().toInt()},
                                      {"boot_id", boots->currentData(exactBootRole).toString()},
                                      {"compare_first", left->currentData(recordIdentityRole).toString()},
                                      {"compare_second", right->currentData(recordIdentityRole).toString()},
                                      {"show_log_coverage", showCoverage->isChecked()},
                                      {"collapsed_groups", collapsed},
                                      {"selected_record", desiredSelection}});
    return changed;
}

void open(QWidget *owner) {
    while (openHistory(owner)) {}
}
}
