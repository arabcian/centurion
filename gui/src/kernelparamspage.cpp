#include "kernelparamspage.h"
#include "privileged.h"
#include "theme.h"
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
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
    lay->addLayout(row);

    status_ = new QLabel;
    status_->setStyleSheet(QStringLiteral("color:%1;").arg(theme::FG_DIM));
    lay->addWidget(status_);

    tree_ = new QTreeWidget;
    tree_->setColumnCount(3);
    tree_->setHeaderLabels({tr("Setting"), tr("Value"), tr("Writable")});
    tree_->setRootIsDecorated(false);
    tree_->setUniformRowHeights(true);
    tree_->setAlternatingRowColors(true);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    tree_->header()->resizeSection(0, 520);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    lay->addWidget(tree_, 1);

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
    status_->setText(tr("Reading…"));
    privileged::run(helper, QJsonObject{{"op", "ksnap"}, {"roots", QJsonArray{root}}}, this, [this, root](const privileged::Result &r) {
        busy_ = false;
        if (!r.reached) { status_->setText(r.message()); return; }
        if (!r.json.contains(QStringLiteral("entries"))) {
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
        fill(entries);
        msg += tr("%1 settings").arg(entries.size());
        if (r.json.value(QStringLiteral("truncated")).toBool()) msg += tr(" — list cut short (%1)").arg(r.json.value(QStringLiteral("reason")).toString());
        status_->setText(msg);
    }, 60000);
}

void KernelParamsPage::fill(const QJsonArray &entries) {
    tree_->setUpdatesEnabled(false);
    tree_->clear();
    QList<QTreeWidgetItem *> items;
    items.reserve(entries.size());
    for (const QJsonValue &v : entries) {
        const QJsonObject e = v.toObject();
        const QString value = e.value(QStringLiteral("v")).toString();
        const QString first = value.section(QLatin1Char('\n'), 0, 0);
        auto *it = new QTreeWidgetItem({e.value(QStringLiteral("p")).toString(), value.contains(QLatin1Char('\n')) ? first + QStringLiteral(" …") : first,
                                        e.value(QStringLiteral("w")).toBool() ? tr("yes") : QString()});
        it->setToolTip(1, value);
        items.append(it);
    }
    tree_->addTopLevelItems(items);
    tree_->sortItems(0, Qt::AscendingOrder);
    tree_->setUpdatesEnabled(true);
    applyFilter();
}

void KernelParamsPage::applyFilter() {
    const QString f = filter_->text().trimmed();
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        auto *it = tree_->topLevelItem(i);
        it->setHidden(!f.isEmpty() && !it->text(0).contains(f, Qt::CaseInsensitive) && !it->text(1).contains(f, Qt::CaseInsensitive));
    }
}
