#include "kernelparamspage.h"
#include "privileged.h"
#include "theme.h"
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QSaveFile>
#include <QDateTime>
#include <QJsonDocument>
#include <QMessageBox>
#include <QSysInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableView>
#include <QSortFilterProxyModel>
#include <QScrollBar>
#include <QItemSelectionModel>
#include <QTimer>
#include "kernelparamsmodel.h"
#include "workspacestate.h"
#include <QVBoxLayout>

// The sched_* sysctls are world-readable: listed from /proc/sys/kernel in the scheduler scope.
static QJsonArray schedSysctls() {
    QJsonArray out;
    const QDir dir(QStringLiteral("/proc/sys/kernel"));
    for (const QFileInfo &fi : dir.entryInfoList({QStringLiteral("sched_*"), QStringLiteral("numa_balancing*")}, QDir::Files, QDir::Name)) {
        QFile f(fi.filePath());
        if (!f.open(QIODevice::ReadOnly)) continue;
        out.append(QJsonObject{{"p", fi.filePath()}, {"v", QString::fromUtf8(f.read(4096)).trimmed()}, {"w", bool(fi.permissions() & QFileDevice::WriteOwner)}});
    }
    return out;
}

KernelParamsPage::KernelParamsPage(QWidget *parent) : QWidget(parent) {
    auto *lay = new QVBoxLayout(this);
    auto *about = new QLabel(tr("Live values of kernel settings, including the root-only debugfs ones (read-only: nothing is changed here)."));
    about->setWordWrap(true);
    about->setStyleSheet(QStringLiteral("color:%1;").arg(theme::FG_DIM));
    lay->addWidget(about);

    auto *row = new QHBoxLayout;
    scope_ = new QComboBox;
    // The item data is the root handed to ksnap; the scheduler scope also lists the sched_* sysctls (read here).
    scope_->addItem(tr("Scheduler (EEVDF)"), QStringLiteral("/sys/kernel/debug/sched"));
    scope_->addItem(tr("x86 scheduling (debugfs)"), QStringLiteral("/sys/kernel/debug/x86"));
    scope_->addItem(tr("CPU frequency and idle"), QStringLiteral("/sys/devices/system/cpu"));
    scope_->addItem(tr("Memory management"), QStringLiteral("/sys/kernel/mm"));
    scope_->addItem(tr("Kernel module parameters"), QStringLiteral("/sys/module"));
    scope_->addItem(tr("Everything under /sys"), QStringLiteral("/sys"));
    filter_ = new QLineEdit;
    filter_->setPlaceholderText(tr("Filter by name or value"));
    filter_->setClearButtonEnabled(true);
    auto *btn = new QPushButton(tr("Read"));
    row->addWidget(scope_);
    row->addWidget(filter_, 1);
    row->addWidget(btn);
    export_ = new QPushButton(tr("Export and Save As…"));
    export_->setEnabled(false);
    row->addWidget(export_);
    connect(export_, &QPushButton::clicked, this, &KernelParamsPage::exportSnapshot);
    lay->addLayout(row);

    status_ = new QLabel;
    status_->setStyleSheet(QStringLiteral("color:%1;").arg(theme::FG_DIM));
    lay->addWidget(status_);

    tree_ = new QTableView;
    model_ = new KernelParamsModel(tree_);
    proxy_ = new QSortFilterProxyModel(tree_);
    proxy_->setSourceModel(model_);
    proxy_->setFilterRole(KernelParamsModel::SearchRole);
    proxy_->setFilterKeyColumn(0);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    tree_->setModel(proxy_);
    tree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->verticalHeader()->hide();
    tree_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    tree_->verticalHeader()->setDefaultSectionSize(fontMetrics().height()+8);
    tree_->setAlternatingRowColors(true);
    tree_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    tree_->horizontalHeader()->resizeSection(0, 520);
    tree_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    tree_->setSortingEnabled(true); tree_->sortByColumn(0,Qt::AscendingOrder);
    lay->addWidget(tree_, 1);
    const auto stateKey=workspace::key(theme::experimentalEnabled(),QStringLiteral("health/kernel-parameters"));
    const auto saved=workspace::read(stateKey);
    const auto scopeIndex=scope_->findData(saved.value(QStringLiteral("scope")).toString());
    if(scopeIndex>=0) scope_->setCurrentIndex(scopeIndex);
    filter_->setMaxLength(512); filter_->setText(saved.value(QStringLiteral("filter")).toString());
    const auto width=saved.value(QStringLiteral("width")).toInt(520);
    if(width>=40 && width<=4000) tree_->setColumnWidth(0,width);
    const auto sort=saved.value(QStringLiteral("sort")).toInt(0);
    if(sort>=0 && sort<3) tree_->sortByColumn(sort,saved.value(QStringLiteral("descending")).toBool()?Qt::DescendingOrder:Qt::AscendingOrder);
    applyFilter();
    const auto saveState=[this,stateKey] {
        workspace::write(stateKey,QJsonObject{{"scope",scope_->currentData().toString()},{"filter",filter_->text()},
            {"width",tree_->columnWidth(0)},{"sort",tree_->horizontalHeader()->sortIndicatorSection()},
            {"descending",tree_->horizontalHeader()->sortIndicatorOrder()==Qt::DescendingOrder}});
    };
    auto *stateTimer=new QTimer(this); stateTimer->setSingleShot(true); stateTimer->setInterval(250);
    connect(stateTimer,&QTimer::timeout,this,saveState);
    connect(filter_,&QLineEdit::textChanged,stateTimer,[stateTimer]{stateTimer->start();});
    connect(scope_,&QComboBox::activated,this,[saveState](int){saveState();});
    connect(tree_->horizontalHeader(),&QHeaderView::sectionResized,stateTimer,[stateTimer]{stateTimer->start();});
    connect(tree_->horizontalHeader(),&QHeaderView::sortIndicatorChanged,stateTimer,[stateTimer]{stateTimer->start();});

    connect(btn, &QPushButton::clicked, this, &KernelParamsPage::read);
    connect(scope_, &QComboBox::activated, this, &KernelParamsPage::read);
    connect(filter_, &QLineEdit::textChanged, this, &KernelParamsPage::applyFilter);
}

void KernelParamsPage::read() {
    if (busy_) return;
    const QString helper = privileged::helperPath(QStringLiteral("health-reader"));
    if (!QFileInfo(helper).isExecutable()) { status_->setText(tr("health-reader is not installed.")); return; }
    const QString root = scope_->currentData().toString();
    busy_ = true;
    export_->setEnabled(false);
    scope_->setEnabled(false);
    capturedScope_.clear();
    captured_ = QJsonArray{};
    if(viewScope_!=root) { model_->clear(); viewScope_=root; }
    status_->setText(model_->rowCount()>0?tr("Reading… previous capture is shown until the new read completes."):tr("Reading…"));
    privileged::run(helper, QJsonObject{{"op", "ksnap"}, {"roots", QJsonArray{root}}}, this, [this, root](const privileged::Result &r) {
        busy_ = false;
        scope_->setEnabled(true);
        if (!r.ok()) { model_->clear(); status_->setText(r.message()); return; }
        if (!r.json.value(QStringLiteral("entries")).isArray()) {
            model_->clear();
            status_->setText(r.json.value(QStringLiteral("ok")).toBool() ? tr("The installed health-reader is too old for this page — reinstall Centurion.") : r.message());
            return;
        }
        QJsonArray entries = r.json.value(QStringLiteral("entries")).toArray();
        QString msg;
        if (root == QLatin1String("/sys/kernel/debug/sched")) {
            const QJsonArray sys = schedSysctls();
            for (const QJsonValue &v : sys) entries.append(v);  // the tree sorts
            if (!r.json.value(QStringLiteral("debugfs_mounted")).toBool()) msg = tr("debugfs is not mounted: only the sysctls are shown.") + QLatin1Char(' ');
        }
        if(!fill(entries)) { model_->clear(); status_->setText(tr("Invalid or duplicate setting paths in capture.")); return; }
        msg += tr("%1 settings").arg(entries.size());
        if (r.json.value(QStringLiteral("truncated")).toBool()) msg += tr(" — list cut short (%1)").arg(r.json.value(QStringLiteral("reason")).toString());
        status_->setText(msg);
        captured_ = entries;
        capturedMeta_ = r.json; capturedMeta_.remove(QStringLiteral("entries"));
        capturedScope_ = root;
        capturedAt_ = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        capturedStatus_ = msg;
        export_->setEnabled(true);
    }, 60000);
}

bool KernelParamsPage::fill(const QJsonArray &entries) {
    const auto path=tree_->currentIndex().data(KernelParamsModel::PathRole).toString();
    const auto anchor=tree_->indexAt(QPoint(1,1)).data(KernelParamsModel::PathRole).toString();
    if(!model_->replace(entries)) return false;
    const auto selected=proxy_->mapFromSource(model_->byPath(path));
    if(selected.isValid()) tree_->selectionModel()->setCurrentIndex(selected,QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);
    else tree_->selectionModel()->clear();
    const auto top=proxy_->mapFromSource(model_->byPath(anchor));
    if(top.isValid()) tree_->scrollTo(top,QAbstractItemView::PositionAtTop);
    return true;
}

void KernelParamsPage::applyFilter() {
    proxy_->setFilterFixedString(filter_->text().trimmed());
}

void KernelParamsPage::exportSnapshot() {
    if (busy_ || capturedScope_.isEmpty()) return;
    if (QMessageBox::question(this, tr("Export kernel settings"), tr("The snapshot can contain private device identifiers and configuration. All captured rows, including hidden rows and multiline values, will be saved. Continue?")) != QMessageBox::Yes) return;
    const auto path = QFileDialog::getSaveFileName(this, tr("Export kernel settings"), QStringLiteral("centurion-kernel-settings.json"), tr("JSON files (*.json)"));
    if (path.isEmpty()) return;
    const QJsonObject snapshot{{"schema", 1}, {"value_limit_bytes",4096}, {"scope", capturedScope_}, {"captured_utc", capturedAt_}, {"kernel", QSysInfo::kernelVersion()}, {"status", capturedStatus_}, {"capture_receipt", capturedMeta_}, {"entries", captured_}};
    const auto bytes = QJsonDocument(snapshot).toJson(QJsonDocument::Indented);
    QSaveFile f(path); f.setDirectWriteFallback(false);
    if (!f.open(QIODevice::WriteOnly) || !f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) || f.write(bytes) != bytes.size() || !f.commit()) {
        QMessageBox::warning(this, tr("Export kernel settings"), tr("Snapshot could not be saved.")); return;
    }
    QMessageBox::information(this, tr("Export kernel settings"), tr("Saved %1 captured settings.").arg(captured_.size()));
}
