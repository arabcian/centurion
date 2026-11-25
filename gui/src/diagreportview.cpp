#include "edidfields.h"
#include "acpievents.h"
#include "hierarchyviewmodel.h"
#include <QTreeView>
#include <QSortFilterProxyModel>
#include "diagnostictools.h"
#include "charts.h"
#include <QPainter>
#include <QTextBlock>
#include <cmath>
#include <QSpinBox>
#include <QTextCursor>
#include <QMap>
#include <QHash>
#include <QRegularExpression>
#include <QItemSelectionModel>
#include "diagreportview.h"
#include "diagnostichistoryview.h"
#include <QInputDialog>
#include "workspacestate.h"
#include "diagsources.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QLockFile>
#include <QSet>
#include <QUuid>
#include "i18n.h"

#include "sysadmin.h"
#include "theme.h"
#include "toolcatalog.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDateTime>
#include <QComboBox>
#include <QCheckBox>
#include <QSplitter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QSaveFile>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>
#include <QFileInfo>

using diag::Level;

namespace {

QSettings guiSettings() {
    return QSettings(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/centurion/gui.ini"), QSettings::IniFormat);
}

const char *color(Level l) {
    switch (l) {
    case Level::Attention: return theme::DANGER;
    case Level::Review: return theme::WARN;
    case Level::Ok: return theme::OK;
    case Level::Unknown: return theme::MUTED;
    case Level::Info: break;
    }
    return theme::FG_DIM;
}

QLabel *text(const QString &s, const char *tone = nullptr) {
    auto *l = new QLabel(s);
    l->setWordWrap(true);
    l->setTextFormat(Qt::PlainText);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (tone) l->setProperty("tone", tone);
    return l;
}

QWidget *findingCard(const diag::Finding &f, DiagReportView *view) {
    auto *card = new QFrame;
    card->setObjectName(QStringLiteral("diagCard"));
    card->setStyleSheet(QStringLiteral("QFrame#diagCard{background:%1;border:1px solid %2;border-left:3px solid %3;border-radius:8px}"
                                       "QFrame#diagCard QLabel{background:transparent;border:none}")
                            .arg(theme::BG2, theme::BORDER, color(f.level)));
    card->setProperty("diagLevel", int(f.level));
    card->setProperty("diagSearch", f.title + QLatin1Char('\n') + f.detail + QLatin1Char('\n') + f.action + QLatin1Char('\n') + f.fixLabel);
    auto *col = new QVBoxLayout(card);
    col->setContentsMargins(12, 8, 12, 8);
    col->setSpacing(4);
    auto *head = new QHBoxLayout;
    auto *title = text(f.title);
    QFont bold = title->font();
    bold.setBold(true);
    title->setFont(bold);
    head->addWidget(title, 1);
    auto *badge = new QLabel(diag::levelName(f.level));
    badge->setStyleSheet(QStringLiteral("color:%1;font-weight:600").arg(color(f.level)));
    head->addWidget(badge, 0, Qt::AlignTop);
    col->addLayout(head);
    if (!f.detail.isEmpty()) col->addWidget(text(f.detail, "dim"));
    if (!f.action.isEmpty()) col->addWidget(text(QStringLiteral("→ ") + f.action));
    // Technical terms in the finding: named on one quiet line, explained on hover.
    const QString terms = toolcatalog::explainTerms(f.title + QLatin1Char(' ') + f.detail + QLatin1Char(' ') + f.action);
    if (!terms.isEmpty()) {
        QStringList names;
        for (const QString &line : terms.split(QLatin1Char('\n'))) names << line.section(QStringLiteral(" — "), 0, 0);
        auto *glossary = text(QCoreApplication::translate("diag", "ⓘ Terms: %1").arg(names.join(QStringLiteral(" · "))), "muted");
        glossary->setToolTip(terms);
        col->addWidget(glossary);
    }
    if (!f.fix.isEmpty()) {
        auto *fix = new QPushButton(f.fixLabel);
        fix->setToolTip(QCoreApplication::translate("diag", "Makes this change now (asks for the administrator password)."));
        QObject::connect(fix, &QPushButton::clicked, view, [view, f] {
            sysadmin::act(view, f.fixLabel, QCoreApplication::translate("diag", "%1\n\n%2?").arg(f.title, f.fixLabel), f.fix,
                          [view](bool ok) { if (ok) Q_EMIT view->fixApplied(); });
        });
        col->addWidget(fix, 0, Qt::AlignLeft);
    }
    return card;
}

// A presentation key only; duplicate table/row keys are deliberately ambiguous.
QString tableKey(const diag::Table &t) {
    return QString::fromUtf8(QJsonDocument(QJsonArray{t.viewKind,t.title,QJsonArray::fromStringList(t.headers)}).toJson(QJsonDocument::Compact));
}
QString rowKey(const QTreeWidgetItem *item) {
    if (item->treeWidget() && item->treeWidget()->property("stableDeviceRows").toBool())
        return item->data(0, Qt::UserRole).toString();
    QJsonArray row;
    for (int c = 0; c < item->columnCount(); ++c) row.append(item->text(c));
    return QString::fromUtf8(QJsonDocument(row).toJson(QJsonDocument::Compact));
}
struct TableViewState {
    QJsonObject header;
    QSet<QString> selected;
    QString current;
    bool hasCurrent = false;
    int scroll = 0;
};
TableViewState captureTable(QTreeWidget *tree) {
    TableViewState state;
    state.header = workspace::captureHeader(tree);
    state.scroll = tree->verticalScrollBar()->value();
    QMap<QString,int> counts;
    for (int i=0;i<tree->topLevelItemCount();++i) ++counts[rowKey(tree->topLevelItem(i))];
    for (const auto *item : tree->selectedItems()) {
        const auto key = rowKey(item); if (!key.isEmpty() && counts.value(key)==1 && !item->isHidden()) state.selected.insert(key);
    }
    if (const auto *item = tree->currentItem(); item && !item->isHidden()) {
        state.current = rowKey(item); state.hasCurrent = !state.current.isEmpty() && counts.value(state.current)==1;
    }
    return state;
}
void restoreTable(QTreeWidget *tree,const TableViewState &state) {
    workspace::restoreHeader(tree,state.header,true);
    QMap<QString,int> counts;
    for (int i=0;i<tree->topLevelItemCount();++i) ++counts[rowKey(tree->topLevelItem(i))];
    for (int i=0;i<tree->topLevelItemCount();++i) {
        auto *item = tree->topLevelItem(i); const auto key = rowKey(item);
        if (key.isEmpty() || counts.value(key)!=1 || item->isHidden()) continue;
        if (state.hasCurrent && state.current==key) tree->setCurrentItem(item,0,QItemSelectionModel::NoUpdate);
        item->setSelected(state.selected.contains(key));
    }
    tree->verticalScrollBar()->setValue(state.scroll);
}

// This dialog operates exclusively on a captured report. The model creates no
// per-row widgets and the filter never invokes a diagnostic or hardware read.
void exploreHardware(QWidget *owner, diag::Table table, std::shared_ptr<QJsonObject> state, QString requestedKey) {
    // The frozen table and state outlive a report replacement in the nested event loop.
    QDialog dlg; dlg.setWindowTitle(table.title); dlg.resize(960, 640);
    QObject::connect(owner, &QObject::destroyed, &dlg, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dlg);
    auto *search = new QLineEdit;
    search->setPlaceholderText(QCoreApplication::translate("diag", "Filter hierarchy…"));
    layout->addWidget(search);
    auto *tree = new QTreeView;
    auto *model = new HierarchyViewModel(table, tree);
    auto *proxy = new QSortFilterProxyModel(tree);
    proxy->setSourceModel(model); proxy->setRecursiveFilteringEnabled(true);
    proxy->setFilterKeyColumn(-1); proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    tree->setModel(proxy); tree->setUniformRowHeights(true); tree->setAlternatingRowColors(true);
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers); tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree->header()->setStretchLastSection(true);
    layout->addWidget(tree, 1);
    auto *details = new QPlainTextEdit; details->setReadOnly(true);
    details->setMaximumHeight(dlg.fontMetrics().height() * 10);
    layout->addWidget(details);
    layout->addWidget(text(table.note, "muted"));
    const auto keyOf = [proxy](const QModelIndex &index) { return proxy->mapToSource(index.siblingAtColumn(0)).data(HierarchyViewModel::KeyRole).toString(); };
    QSet<QString> expanded;
    for (const auto &key : state->value(QStringLiteral("expanded")).toArray())
        if (model->byKey(key.toString()).isValid()) expanded.insert(key.toString());
    QString wanted = requestedKey.isEmpty() ? state->value(QStringLiteral("selected")).toString() : requestedKey;
    if (!model->byKey(wanted).isValid()) wanted.clear();
    bool updating = true;
    const auto restore = [&] {
        tree->collapseAll();
        for (const QString &key : expanded) {
            auto index = proxy->mapFromSource(model->byKey(key));
            for (auto p = index; p.isValid(); p = p.parent()) tree->expand(p);
        }
        const auto target = proxy->mapFromSource(model->byKey(wanted));
        if (target.isValid()) {
            for (auto p = target.parent(); p.isValid(); p = p.parent()) tree->expand(p);
            tree->setCurrentIndex(target); tree->scrollTo(target);
        } else { tree->selectionModel()->clear(); details->clear(); }
    };
    QObject::connect(tree, &QTreeView::expanded, &dlg, [&](const QModelIndex &index) {
        if (!updating && search->text().isEmpty()) { const auto key = keyOf(index); if (!key.isEmpty()) expanded.insert(key); }
    });
    QObject::connect(tree, &QTreeView::collapsed, &dlg, [&](const QModelIndex &index) {
        if (!updating && search->text().isEmpty()) expanded.remove(keyOf(index));
    });
    QObject::connect(tree->selectionModel(), &QItemSelectionModel::currentChanged, &dlg, [&](const QModelIndex &index) {
        QStringList lines;
        if (index.isValid()) {
            const auto source = proxy->mapToSource(index.siblingAtColumn(0));
            for (int c = 0; c < model->columnCount(); ++c)
                lines << table.headers.value(c) + QStringLiteral(": ") + source.siblingAtColumn(c).data().toString();
        }
        details->setPlainText(lines.join(QLatin1Char('\n')));
        if (!updating) wanted = keyOf(index);
    });
    if (!state->contains(QStringLiteral("expanded"))) {
        tree->expandToDepth(0);
        for (int i = 0; i < model->nodeCount(); ++i) {
            const auto index = proxy->mapFromSource(model->atNode(i));
            if (tree->isExpanded(index) && !keyOf(index).isEmpty()) expanded.insert(keyOf(index));
        }
    } else restore();
    if (!wanted.isEmpty()) restore();
    updating = false;
    QObject::connect(search, &QLineEdit::textChanged, &dlg, [&](const QString &query) {
        updating = true;
        proxy->setFilterFixedString(query);
        if (query.isEmpty()) restore();
        else {
            tree->expandAll();
            const auto target = proxy->mapFromSource(model->byKey(wanted));
            if (target.isValid()) tree->setCurrentIndex(target);
            else { tree->selectionModel()->clear(); details->clear(); }
        }
        updating = false;
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);
    dlg.exec();
    QObject::disconnect(tree, nullptr, &dlg, nullptr);
    QObject::disconnect(tree->selectionModel(), nullptr, &dlg, nullptr);
    QObject::disconnect(search, nullptr, &dlg, nullptr);
    QJsonArray keys;
    // Preserve the unfiltered expansion state; never persist ambiguous/unknown IDs.
    for (const auto &key : expanded) keys.append(key);
    *state = QJsonObject{{"expanded", keys}, {"selected", wanted}};
}

// An explicit, read-only subscription to the already implemented ACPI generic
// netlink family. History is bounded to this viewing session; no firmware
// methods, helper commands or extra diagnostic scans are invoked.
void inspectAcpiNotifications(QWidget *owner, const diag::Table &inventory, std::shared_ptr<QJsonObject> state) {
    auto tr=[](const char *s){return QCoreApplication::translate("diag",s);};
    QDialog dlg; dlg.setWindowTitle(tr("Live ACPI notifications"));dlg.resize(960,640);
    QObject::connect(owner,&QObject::destroyed,&dlg,&QDialog::reject);
    auto *layout=new QVBoxLayout(&dlg);
    layout->addWidget(text(tr("Events are received only while this viewer is open. Device links use exact identifiers in the captured inventory; they are not proof of historical binding or cause."),"muted"));
    auto *status=text(QString());layout->addWidget(status);
    auto *events=new QTreeWidget;events->setRootIsDecorated(false);events->setUniformRowHeights(true);
    events->setHeaderLabels({tr("Received (UTC)"),tr("Device class"),tr("Bus ID"),tr("Type (hex)"),tr("Data (hex)"),tr("Inventory match"),tr("Kernel sequence")});
    events->header()->setSectionResizeMode(QHeaderView::ResizeToContents);events->header()->setStretchLastSection(true);layout->addWidget(events,1);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Close);layout->addWidget(buttons);
    auto *jump=buttons->addButton(tr("Inspect matched device…"),QDialogButtonBox::ActionRole);jump->setEnabled(false);
    auto *clear=buttons->addButton(tr("Clear visible notifications"),QDialogButtonBox::ActionRole);
    AcpiEvents receiver;
    quint64 gaps=0,evicted=0,received=0;
    auto updateStatus=[&]{status->setText(receiver.active()?tr("Received: %1 · older rows omitted: %2 · observed capture gaps: %3").arg(received).arg(evicted).arg(gaps):tr("ACPI netlink subscription unavailable (unsupported or inaccessible)."));};
    updateStatus();
    QHash<QString,QStringList> keys;
    for(qsizetype i=0;i<inventory.rows.size();++i){const auto id=inventory.rows.at(i).value(0);if(!id.isEmpty())keys[id]<<inventory.rowKeys.value(i);}
    QObject::connect(&receiver,&AcpiEvents::captureGap,&dlg,[&]{++gaps;updateStatus();});
    QObject::connect(&receiver,&AcpiEvents::notification,&dlg,[&](const QString &cls,const QString &id,uint type,uint data,uint sequence){
        ++received;
        const auto candidates=keys.value(id);
        const QString key=candidates.size()==1?candidates.first():QString();
        auto *row=new QTreeWidgetItem({QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs),cls,id,
            QStringLiteral("0x%1").arg(type,0,16),QStringLiteral("0x%1").arg(data,0,16),
            !key.isEmpty()?tr("Exact identifier candidate"):candidates.size()>1?tr("Ambiguous identifier"):tr("No verified inventory link"),QString::number(sequence)});
        row->setData(0,Qt::UserRole,key);events->addTopLevelItem(row);
        while(events->topLevelItemCount()>512){delete events->takeTopLevelItem(0);++evicted;}
        updateStatus();
    });
    QObject::connect(events,&QTreeWidget::itemSelectionChanged,&dlg,[&]{jump->setEnabled(events->currentItem()&&!events->currentItem()->data(0,Qt::UserRole).toString().isEmpty()&&events->currentItem()->isSelected());});
    QObject::connect(jump,&QPushButton::clicked,&dlg,[&]{if(auto *item=events->currentItem();item&&item->isSelected()){const auto key=item->data(0,Qt::UserRole).toString();if(!key.isEmpty())exploreHardware(&dlg,inventory,state,key);}});
    QObject::connect(clear,&QPushButton::clicked,&dlg,[&]{evicted+=quint64(events->topLevelItemCount());events->clear();updateStatus();});
    QObject::connect(buttons,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);dlg.exec();
    QObject::disconnect(&receiver,nullptr,&dlg,nullptr);
    QObject::disconnect(events,nullptr,&dlg,nullptr);
}

QWidget *tableWidget(const diag::Table &t, const std::shared_ptr<QJsonObject> &hardwareState) {
    auto *box = new QWidget;
    auto *col = new QVBoxLayout(box);
    col->setContentsMargins(0, 4, 0, 0);
    col->setSpacing(3);
    auto *title = text(t.title);
    title->setWordWrap(false);  // a wrapping label in a scroll area reserves height it does not use
    QFont bold = title->font();
    bold.setBold(true);
    title->setFont(bold);
    col->addWidget(title);
    if (t.rows.isEmpty()) {
        col->addWidget(text(QCoreApplication::translate("diag", "(nothing to list)"), "muted"));
    } else {
        auto *tree = new QTreeWidget;
        int columns = qMax(1, int(t.headers.size()));
        for (const QStringList &row : t.rows) columns = qMax(columns, int(row.size()));
        tree->setColumnCount(columns);
        tree->setProperty("workspaceTableKey",tableKey(t));
        tree->setProperty("stableDeviceRows", (t.viewKind == QStringLiteral("hardware-device-tree") || t.viewKind == QStringLiteral("acpi-wmi-tree")));
        tree->setHeaderLabels(t.headers);
        tree->setHeaderHidden(t.headers.isEmpty());
        tree->setRootIsDecorated(false);
        tree->setAlternatingRowColors(true);
        tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
        tree->setUniformRowHeights(true);
        tree->setWordWrap(false);
        tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
        tree->setSortingEnabled(false);
        for (int i = 0; i < t.rows.size(); ++i) {
            auto *it = new QTreeWidgetItem(t.rows[i]);
            if ((t.viewKind == QStringLiteral("hardware-device-tree") || t.viewKind == QStringLiteral("acpi-wmi-tree"))) it->setData(0, Qt::UserRole, t.rowKeys.value(i));
            for (int c = 0; c < t.rows[i].size(); ++c) it->setToolTip(c, t.rows[i][c]);
            const Level l = i < t.levels.size() ? t.levels[i] : Level::Info;
            if (l == Level::Attention || l == Level::Review || l == Level::Ok) it->setForeground(0, QColor(color(l)));
            tree->addTopLevelItem(it);
        }
        for (int c = 0; c < tree->columnCount(); ++c) {
            tree->resizeColumnToContents(c);
            tree->setColumnWidth(c, qMin(tree->columnWidth(c), tree->fontMetrics().horizontalAdvance(QString(80, QLatin1Char('M')))));
        }
        tree->header()->setStretchLastSection(true);
        // Preserve diagnostic order until a user explicitly requests sorting.
        tree->header()->setSectionsClickable(true);
        QObject::connect(tree->header(), &QHeaderView::sectionClicked, tree, [tree](int column) {
            if (!tree->isSortingEnabled()) {
                tree->setSortingEnabled(true);
                tree->sortByColumn(column, Qt::AscendingOrder);
            }
        });
        // Sized to its rows (up to 14) so the page scrolls as one, not a stack of small scroll boxes.
        const int rowH = tree->sizeHintForRow(0) > 0 ? tree->sizeHintForRow(0) : tree->fontMetrics().height() + 6;
        const int shown = qMin(int(t.rows.size()), 14);
        // Room for a horizontal scroll bar: wide tables (many columns, long values) get one, and it must not cover the last row.
        tree->setFixedHeight(rowH * shown + (t.headers.isEmpty() ? 0 : tree->header()->sizeHint().height()) + 2 * tree->frameWidth() + 4 +
                             tree->horizontalScrollBar()->sizeHint().height());
        col->addWidget(tree);
    }
    if (!t.rows.isEmpty() && t.parents.size() == t.rows.size()) {
        auto *explore = new QPushButton(QCoreApplication::translate("diag", "Explore hierarchy…"));
        col->addWidget(explore, 0, Qt::AlignLeft);
        if ((t.viewKind == QStringLiteral("hardware-device-tree") || t.viewKind == QStringLiteral("acpi-wmi-tree"))) {
            QObject::connect(explore, &QPushButton::clicked, explore, [explore, box, t, hardwareState] {
                QString key;
                if (auto *flat = box->findChild<QTreeWidget *>(); flat && flat->currentItem() && flat->currentItem()->isSelected())
                    key = flat->currentItem()->data(0, Qt::UserRole).toString();
                exploreHardware(explore, t, hardwareState, key);
            });
        } else QObject::connect(explore, &QPushButton::clicked, explore, [explore, t] {
            QDialog dlg(explore->window()); dlg.setWindowTitle(t.title); dlg.resize(900, 600);
            auto *layout = new QVBoxLayout(&dlg);
            auto *search = new QLineEdit; search->setPlaceholderText(QCoreApplication::translate("diag", "Filter hierarchy…"));
            layout->addWidget(search);
            auto *tree = new QTreeWidget; tree->setColumnCount(qMax(1, int(t.headers.size())));
            tree->setHeaderLabels(t.headers); tree->setUniformRowHeights(true);
            tree->setAlternatingRowColors(true); tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
            QList<QTreeWidgetItem *> items;
            for (int i = 0; i < t.rows.size(); ++i) {
                auto *item = new QTreeWidgetItem(t.rows.at(i));
                const int parent = t.parents.at(i);
                if (parent >= 0 && parent < i) items.at(parent)->addChild(item);
                else tree->addTopLevelItem(item);
                for (int c = 0; c < t.rows.at(i).size(); ++c) item->setToolTip(c, t.rows.at(i).at(c));
                items << item;
            }
            tree->expandToDepth(1);
            tree->header()->setStretchLastSection(true);
            tree->resizeColumnToContents(0);
            layout->addWidget(tree, 1);
            auto *details = new QPlainTextEdit; details->setReadOnly(true); details->setMaximumHeight(dlg.fontMetrics().height() * 8);
            layout->addWidget(details);
            QObject::connect(tree, &QTreeWidget::currentItemChanged, &dlg, [details, t](QTreeWidgetItem *item) {
                QStringList lines;
                if (item) for (int c = 0; c < t.headers.size(); ++c) lines << t.headers.at(c) + QStringLiteral(": ") + item->text(c);
                details->setPlainText(lines.join(QLatin1Char('\n')));
            });
            QObject::connect(search, &QLineEdit::textChanged, &dlg, [tree, items](const QString &query) {
                // Reverse insertion order: a matching descendant keeps its ancestors visible.
                for (auto it = items.crbegin(); it != items.crend(); ++it) {
                    auto *item = *it; bool match = query.isEmpty();
                    for (int c = 0; c < tree->columnCount() && !match; ++c) match = item->text(c).contains(query, Qt::CaseInsensitive);
                    for (int c = 0; c < item->childCount() && !match; ++c) match = !item->child(c)->isHidden();
                    item->setHidden(!match);
                    if (match && !query.isEmpty()) item->setExpanded(true);
                }
            });
            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
            QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
            layout->addWidget(buttons); dlg.exec();
        });
    }
    if (!t.note.isEmpty()) col->addWidget(text(t.note, "muted"));
    return box;
}

// Read-only, common Hz axis. The band is only an explicit decoded VRR range;
// DTD marks are advertised progressive modes, not current compositor state.
class EdidTimingPlot final : public QWidget {
public:
    explicit EdidTimingPlot(QWidget *parent = nullptr) : QWidget(parent) { setMinimumHeight(86); }
    void setData(std::vector<edidfields::Timing> timings, int minimum, int maximum) {
        timings_ = std::move(timings); minimum_ = minimum; maximum_ = maximum; selected_ = -1; update();
    }
    void selectOffset(int offset) { selected_ = offset; update(); }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        const QColor fg(theme::FG), accent(theme::ACCENT), info(theme::INFO);
        p.setPen(fg);
        if (timings_.empty() && maximum_ <= 0) {
            p.drawText(rect().adjusted(8, 4, -8, -4), Qt::AlignCenter | Qt::TextWordWrap,
                       QCoreApplication::translate("diag", "No verified progressive timing or explicit VRR range to plot."));
            return;
        }
        double high = std::max(60.0, double(maximum_));
        for (const auto &t : timings_) high = std::max(high, t.refreshHz());
        high = std::ceil(high / 10.0) * 10.0;
        const double left = 12, right = width() - 12;
        if (right <= left) return;
        const auto x = [&](double hz) { return left + hz / high * (right-left); };
        const int axis = height()/2;
        p.setPen(QPen(QColor(theme::BORDER), 2)); p.drawLine(QPointF(left,axis),QPointF(right,axis));
        if (minimum_ > 0 && maximum_ >= minimum_) {
            p.setPen(QPen(info, 7, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(x(minimum_),axis-13),QPointF(x(maximum_),axis-13));
        }
        for (const auto &t : timings_) {
            const auto hz = x(t.refreshHz());
            const bool selected = selected_ >= int(t.offset) && selected_ < int(t.offset+18);
            p.setPen(QPen(selected ? accent : fg, selected ? 3 : 1));
            p.drawLine(QPointF(hz,axis-4),QPointF(hz,axis+(selected?15:8)));
        }
        p.setPen(fg); p.drawText(QRectF(left,axis+20,right-left,22),Qt::AlignLeft,QStringLiteral("0 Hz"));
        p.drawText(QRectF(left,axis+20,right-left,22),Qt::AlignRight,QStringLiteral("%1 Hz").arg(high,0,'f',0));
    }
private:
    std::vector<edidfields::Timing> timings_;
    int minimum_ = 0, maximum_ = 0, selected_ = -1;
};

// Inspect the frozen report, never re-open a connector or change an EDID override.
void inspectEdid(QWidget *parent, const diag::Table &bytes, const diag::Table &fields,
                 const QString &connector, int selectedOffset) {
    auto tr = [](const char *s) { return QCoreApplication::translate("diag", s); };
    QMap<QString, QByteArray> captures;
    QSet<QString> invalidConnectors;
    for (const auto &row : bytes.rows) {
        const QString name = row.value(0);
        if (invalidConnectors.contains(name)) continue;
        bool ok = false; const auto offset = row.value(2).toInt(&ok,16);
        auto &capture = captures[name];
        const QByteArray hex = row.value(3).toLatin1().toLower();
        const auto chunk = QByteArray::fromHex(hex);
        if (name.isEmpty() || !ok || offset != capture.size() || offset < 0 || offset >= 4096
            || chunk.isEmpty() || chunk.size() > 16 || offset + chunk.size() > 4096 || chunk.toHex(' ') != hex) {
            captures.remove(name);
            invalidConnectors.insert(name);
            continue;
        }
        capture.append(chunk);
    }
    if (captures.isEmpty() || (!connector.isEmpty() && (!captures.contains(connector)
        || selectedOffset < 0 || selectedOffset >= captures.value(connector).size()))) {
        QMessageBox::information(parent, tr("EDID Byte Inspector"), tr("The requested EDID bytes are missing or invalid."));
        return;
    }
    QDialog dlg(parent); dlg.setWindowTitle(tr("EDID Byte Inspector")); dlg.resize(860, 720);
    auto *layout = new QVBoxLayout(&dlg);
    layout->addWidget(text(tr("Captured bytes only. The connector may have changed since the report was collected.")));
    if (!invalidConnectors.isEmpty())
        layout->addWidget(text(tr("Some connector captures contain invalid byte rows and were omitted.")));
    auto *selector = new QComboBox; selector->addItems(captures.keys()); layout->addWidget(selector);
    auto *summary = text(QString()); layout->addWidget(summary);
    auto *plot = new EdidTimingPlot; layout->addWidget(plot);
    auto *timingTree = new QTreeWidget; timingTree->setRootIsDecorated(false);
    timingTree->setHeaderLabels({tr("Offset"), tr("Scan mode"), tr("Active"), tr("Total"), tr("Pixel Clock (MHz)"),
        tr("Frame rate (Hz)"), tr("Field rate (Hz)")});
    timingTree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    auto *split = new QSplitter(Qt::Vertical); split->addWidget(timingTree); layout->addWidget(split, 1);
    auto *bytePanel = new QWidget; auto *byteLayout = new QVBoxLayout(bytePanel); byteLayout->setContentsMargins(0,0,0,0);
    auto *offset = new QSpinBox; offset->setDisplayIntegerBase(16); offset->setPrefix(QStringLiteral("0x"));
    offset->setAccessibleName(tr("Selected byte offset")); byteLayout->addWidget(offset);
    auto *detail = text(QString()); byteLayout->addWidget(detail);
    auto *raw = new QPlainTextEdit; raw->setReadOnly(true); raw->setLineWrapMode(QPlainTextEdit::NoWrap);
    raw->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont)); byteLayout->addWidget(raw, 1);
    split->addWidget(bytePanel); split->setSizes({170,300});
    auto update = [&] {
        const auto capture = captures.value(selector->currentText()); const int pos = offset->value();
        if (pos < 0 || pos >= capture.size()) return;
        const auto inspection = edidfields::inspect({reinterpret_cast<const std::uint8_t *>(capture.constData()), std::size_t(capture.size())});
        QString info = tr("Byte %1 · block %2 · value %3").arg(pos).arg(pos/128).arg(QStringLiteral("0x%1").arg(quint8(capture.at(pos)),2,16,QLatin1Char('0')));
        for (const auto &row : fields.rows) {
            if (row.value(0) != selector->currentText()) continue;
            bool startOk=false, lenOk=false; const int start=row.value(1).toInt(&startOk,16), len=row.value(2).toInt(&lenOk);
            if (startOk && lenOk && start>=0 && len>0 && pos>=start && pos-start<len) {
                info += QStringLiteral("\n%1 · %2 + %3").arg(row.value(3),row.value(1),row.value(2)); break;
            }
        }
        if (std::size_t(pos/128)<inspection.blocks.size()) {
            const auto &block=inspection.blocks[std::size_t(pos/128)];
            info += QStringLiteral("\n") + (block.checksum ? tr("Checksum matches") : tr("Checksum mismatch"));
            if (!block.declared || !block.structure) info += QStringLiteral(" · ") + tr("Invalid extension boundaries or unexpected block count.");
        }
        if(capture.size()>=4096)info+=QStringLiteral("\n")+tr("Display capped at 4096 bytes; completeness is not established by this view.");
        detail->setText(info); plot->selectOffset(pos);
        const QSignalBlocker rawBlock(raw), treeBlock(timingTree);
        auto cursor=raw->textCursor(); cursor.movePosition(QTextCursor::Start); cursor.movePosition(QTextCursor::Down,QTextCursor::MoveAnchor,pos/16);
        cursor.movePosition(QTextCursor::StartOfLine); cursor.movePosition(QTextCursor::Right,QTextCursor::MoveAnchor,8+(pos%16)*3);
        cursor.movePosition(QTextCursor::Right,QTextCursor::KeepAnchor,2); raw->setTextCursor(cursor); raw->ensureCursorVisible();
        timingTree->clearSelection();
        for (int i=0; i<timingTree->topLevelItemCount(); ++i) {
            auto *item=timingTree->topLevelItem(i); const int start=item->data(0,Qt::UserRole).toInt();
            if (pos>=start && pos-start<18) { timingTree->setCurrentItem(item); break; }
        }
    };
    auto switchConnector = [&] {
        const auto capture=captures.value(selector->currentText());
        const QSignalBlocker blocker(offset), rawBlock(raw), treeBlock(timingTree);
        offset->setRange(0,int(capture.size())-1); offset->setValue(0);
        QStringList lines; for (int i=0;i<capture.size();i+=16) lines << QStringLiteral("0x%1  %2").arg(i,4,16,QLatin1Char('0')).arg(QString::fromLatin1(capture.mid(i,16).toHex(' ')).toUpper());
        raw->setPlainText(lines.join(QLatin1Char('\n')));
        auto decoded=diagnostictools::decodeEdid(capture);
        auto timings=edidfields::detailedTimings({reinterpret_cast<const std::uint8_t *>(capture.constData()), std::size_t(capture.size())});
        // The source reads one extra byte to detect a cap. A frozen report stores
        // at most 4096 bytes, so it cannot prove completeness at the cap.
        if (capture.size()>=4096 || !decoded.structuralValid) { timings.clear(); decoded.vrrMin=decoded.vrrMax=0; }
        QString status=decoded.vrrMax>0 ? tr("Explicit VRR: %1–%2 Hz (%3)").arg(decoded.vrrMin).arg(decoded.vrrMax).arg(decoded.vrrSource)
            : tr("Explicit VRR range unavailable; a general refresh range is not VRR evidence.");
        status+=QStringLiteral("\n")+tr("The table lists verified base and CTA detailed timings. The plot shows progressive timings and the explicit VRR range.");
        summary->setText(status); plot->setAccessibleName(status);
        timingTree->clear();
        for (const auto &t: timings) {
            auto *item=new QTreeWidgetItem(timingTree,{QStringLiteral("0x%1").arg(qulonglong(t.offset),4,16,QLatin1Char('0')),
                t.interlaced ? tr("Interlaced") : tr("Progressive"),
                QStringLiteral("%1 × %2").arg(t.hActive).arg(t.frameActiveLines()),
                QStringLiteral("%1 × %2").arg(t.hActive+t.hBlank).arg(t.frameTotalLines()),
                QString::number(t.pixelClock10kHz/100.0,'f',2),QString::number(t.refreshHz(),'f',2),
                t.interlaced ? QString::number(2*t.refreshHz(),'f',2) : QStringLiteral("—")});
            item->setData(0,Qt::UserRole,int(t.offset));
        }
        std::erase_if(timings, [](const edidfields::Timing &timing) { return timing.interlaced; });
        plot->setData(std::move(timings),decoded.vrrMin,decoded.vrrMax);
        update();
    };
    QObject::connect(selector,&QComboBox::currentIndexChanged,&dlg,switchConnector);
    QObject::connect(offset,&QSpinBox::valueChanged,&dlg,update);
    QObject::connect(timingTree,&QTreeWidget::itemSelectionChanged,&dlg,[&]{
        const auto items=timingTree->selectedItems(); if (!items.isEmpty()) offset->setValue(items.first()->data(0,Qt::UserRole).toInt());
    });
    QObject::connect(raw,&QPlainTextEdit::cursorPositionChanged,&dlg,[&]{
        const auto cursor=raw->textCursor(); const int column=cursor.positionInBlock();
        if(column<8) return;
        const int pos=cursor.blockNumber()*16+(column-8)/3;
        if(pos>=0 && pos<=offset->maximum() && pos!=offset->value()) offset->setValue(pos);
    });
    if (!connector.isEmpty()) selector->setCurrentText(connector);
    switchConnector();
    if (!connector.isEmpty()) offset->setValue(selectedOffset);
    auto *close=new QDialogButtonBox(QDialogButtonBox::Close); layout->addWidget(close);
    auto *exportBinary=close->addButton(tr("Save captured EDID…"), QDialogButtonBox::ActionRole);
    QObject::connect(exportBinary,&QPushButton::clicked,&dlg,[&]{
        const auto captured=captures.value(selector->currentText());
        if (captured.isEmpty()) return;
        if (QMessageBox::warning(&dlg,tr("Export EDID"),
            tr("The captured bytes may contain a display serial number and may be incomplete or damaged. This saves a diagnostic copy, not a validated override. Continue?"),
            QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes) return;
        const auto path=QFileDialog::getSaveFileName(&dlg,tr("Export EDID"),QStringLiteral("edid.bin"),tr("Binary files (*.bin)"));
        if(path.isEmpty()) return;
        // Never open the source connector or invoke a hardware helper. No
        // direct-write fallback: export must be an ordinary atomic file save.
        QSaveFile f(path); f.setDirectWriteFallback(false);
        if(!f.open(QIODevice::WriteOnly) || !f.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner) ||
            f.write(captured)!=captured.size() || !f.commit()) QMessageBox::warning(&dlg,tr("Export EDID"),f.errorString());
    });
    QObject::connect(close,&QDialogButtonBox::rejected,&dlg,&QDialog::reject); dlg.exec();
}

// The visual view consumes the frozen report only. Source capabilities cannot
// identify the negotiated contract or associate an unrelated supply with a port.
void inspectPd(QWidget *parent,const diag::Table &ports,const diag::Table &profiles,const diag::Table &supplies) {
    auto tr=[](const char *s){return QCoreApplication::translate("diag",s);};
    QDialog dlg(parent); dlg.setWindowTitle(tr("USB-C PD Profile View")); dlg.resize(860,680);
    auto *layout=new QVBoxLayout(&dlg);
    layout->addWidget(text(tr("Captured partner capabilities only. Negotiated contract is unknown; the supply measurements below are not attributed to the selected port.")));
    auto *selector=new QComboBox; for(const auto &row:ports.rows) selector->addItem(row.value(0)); layout->addWidget(selector);
    auto *summary=text(QString()); layout->addWidget(summary);
    auto *chart=new BarChart; auto *chartScroll=new QScrollArea;
    chartScroll->setWidget(chart);chartScroll->setWidgetResizable(true); chartScroll->setMaximumHeight(210);layout->addWidget(chartScroll);
    auto *table=new QTreeWidget;table->setRootIsDecorated(false);table->setHeaderLabels(profiles.headers.mid(1));
    table->header()->setSectionResizeMode(QHeaderView::ResizeToContents); layout->addWidget(table,1);
    layout->addWidget(text(tr("Independent supply measurements and reported limits")));
    auto *supplyTable=new QTreeWidget; supplyTable->setRootIsDecorated(false);supplyTable->setHeaderLabels(supplies.headers);
    supplyTable->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    for(const auto &row:supplies.rows) new QTreeWidgetItem(supplyTable,row);
    layout->addWidget(supplyTable,1);
    auto update=[&]{
        table->clear(); QVector<BarChart::Bar> bars; const int selected=selector->currentIndex();
        if(selected<0 || selected>=ports.rows.size())return;
        const auto row=ports.rows.at(selected);
        QString note=tr("Port %1 · role %2 · partner %3 · reported maximum %4").arg(row.value(0),row.value(1),row.value(2),row.value(4));
        note+=QStringLiteral("\n")+tr("Power operation mode: %1").arg(row.value(5));
        int plotted=0;
        for(qsizetype i=0;i<profiles.rows.size();++i){
            const auto &profile=profiles.rows.at(i);if(profile.value(0)!=row.value(0))continue;
            new QTreeWidgetItem(table,profile.mid(1));
            bool ok=false;const double capacity=profile.value(5).toDouble(&ok);
            if(profiles.levels.value(i,Level::Unknown)!=Level::Info || !ok || !std::isfinite(capacity) || capacity<=0)continue;
            ++plotted;if(bars.size()<32) bars.append({profile.value(1),capacity,QStringLiteral("%1 W").arg(capacity,0,'g',8)});
        }
        if(bars.isEmpty())note+=QStringLiteral("\n")+tr("No verified positive source capacity in this capture.");
        else if(plotted>bars.size())note+=QStringLiteral("\n")+tr("First 32 verified profiles plotted; the table contains all captured profiles.");
        summary->setText(note);chart->setAccessibleName(note);chart->setBars(bars);chartScroll->setVisible(!bars.isEmpty());
    };
    QObject::connect(selector,&QComboBox::currentIndexChanged,&dlg,update);update();
    auto *close=new QDialogButtonBox(QDialogButtonBox::Close);layout->addWidget(close);QObject::connect(close,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);dlg.exec();
}

}  // namespace

DiagReportView::DiagReportView(QWidget *parent) : QWidget(parent) {
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);
    auto *top = new QHBoxLayout;
    verdict_ = new QLabel;
    verdict_->setStyleSheet(QStringLiteral("font-weight:700"));
    headline_ = new QLabel;
    headline_->setWordWrap(true);
    headline_->setTextFormat(Qt::PlainText);
    top->addWidget(verdict_);
    top->addWidget(headline_, 1);
    copy_ = new QPushButton(tr("Copy report"));
    save_ = new QPushButton(tr("Save report…"));
    copy_->setEnabled(false);
    save_->setEnabled(false);
    v->addLayout(top);
    auto *actions = new QHBoxLayout;
    snapshot_ = text(QString(), "muted");
    actions->addWidget(snapshot_, 1);
    preview_ = new QPushButton(tr("Export preview…"));
    actions->addWidget(preview_);
    pin_ = new QPushButton(tr("Pin Snapshot")); compare_ = new QPushButton(tr("Compare Snapshot…"));
    pin_->setEnabled(false); compare_->setEnabled(false);
    actions->addWidget(pin_); actions->addWidget(compare_);
    archive_ = new QPushButton(tr("History…")); archive_->setEnabled(false);
    actions->addWidget(archive_); connect(archive_, &QPushButton::clicked, this, &DiagReportView::archiveWorkspace);
    auto *crossBoot = new QPushButton(tr("Cross-Boot History…"));
    actions->addWidget(crossBoot); connect(crossBoot,&QPushButton::clicked,this,[this]{diagnostichistory::open(this);});
    evidence_ = new QPushButton(tr("Evidence…")); evidence_->setEnabled(false);
    actions->addWidget(evidence_); connect(evidence_, &QPushButton::clicked, this, &DiagReportView::evidenceWorkspace);
    connect(pin_, &QPushButton::clicked, this, [this] {
        if (!hasReport_) return;
        baselineCode_ = report_.code; baselineText_ = report_.toText(); baselineTime_ = snapshot_->text();
        compare_->setEnabled(true);
    });
    connect(compare_, &QPushButton::clicked, this, [this] {
        if (!hasReport_ || baselineCode_ != report_.code || baselineText_.isEmpty()) return;
        QDialog dlg(this); dlg.setWindowTitle(tr("Diagnostic Snapshot Comparison")); dlg.resize(1000, 650);
        auto *layout = new QVBoxLayout(&dlg);
        const auto current = report_.toText();
        layout->addWidget(text(current == baselineText_ ? tr("Reports are textually identical.") : tr("Reports differ. Differences are not proof of a hardware change.")));
        auto *split = new QSplitter(Qt::Horizontal);
        for (const auto &entry : {qMakePair(baselineTime_,baselineText_),qMakePair(snapshot_->text(),current)}) {
            auto *panel = new QWidget; auto *v = new QVBoxLayout(panel); v->addWidget(text(entry.first));
            auto *raw = new QPlainTextEdit(entry.second); raw->setReadOnly(true); raw->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont)); v->addWidget(raw); split->addWidget(panel);
        }
        layout->addWidget(split,1); auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close); connect(buttons,&QDialogButtonBox::rejected,&dlg,&QDialog::reject); layout->addWidget(buttons); dlg.exec();
    });
    actions->addWidget(copy_);
    actions->addWidget(save_);
    v->addLayout(actions);
    connect(preview_, &QPushButton::clicked, this, &DiagReportView::previewExport);
    about_ = text(QString(), "muted");
    v->addWidget(about_);
    auto *filters = new QHBoxLayout;
    search_ = new QLineEdit;
    search_->setPlaceholderText(tr("Search findings and readings…"));
    search_->setClearButtonEnabled(true);
    search_->setToolTip(tr("Filters findings and table rows only. The verdict, limits and full export are unchanged."));
    filters->addWidget(search_, 1);
    severity_ = new QComboBox;
    severity_->addItem(tr("All finding levels"), -1);
    for (Level l : {Level::Attention, Level::Review, Level::Unknown, Level::Info, Level::Ok})
        severity_->addItem(diag::levelName(l), int(l));
    filters->addWidget(severity_);
    sections_ = new QComboBox;
    sections_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    sections_->setMinimumContentsLength(12);
    filters->addWidget(sections_, 1);
    v->addLayout(filters);
    filterInfo_ = text(QString(), "muted");
    v->addWidget(filterInfo_);
    scroll_ = new QScrollArea;
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    body_ = new QWidget;
    bodyLayout_ = new QVBoxLayout(body_);
    bodyLayout_->setContentsMargins(0, 2, 6, 6);
    bodyLayout_->setSpacing(8);
    scroll_->setWidget(body_);
    v->addWidget(scroll_, 1);
    connect(search_, &QLineEdit::textChanged, this, &DiagReportView::filterReport);
    connect(severity_, &QComboBox::currentIndexChanged, this, &DiagReportView::filterReport);
    connect(sections_, &QComboBox::activated, this, [this](int i) {
        if (i > 0 && i <= sectionWidgets_.size()) {
            QWidget *target = sectionWidgets_.at(i - 1);
            if (findingWidgets_.contains(target) && target->isHidden()) {
                for (QWidget *w : findingWidgets_) if (!w->isHidden()) { target = w; break; }
            }
            if (!target->isHidden()) scroll_->ensureWidgetVisible(target, 0, 8);
        }
    });

    // All external exports use the same frozen review path; binary and raw data
    // require explicit selection instead of bypassing review via the main buttons.
    connect(copy_, &QPushButton::clicked, this, &DiagReportView::previewExport);
    connect(save_, &QPushButton::clicked, this, &DiagReportView::previewExport);
    setMessage(QString());
}

void DiagReportView::clearBody() {
    findingWidgets_.clear();
    sectionWidgets_.clear();
    tables_.clear();
    sections_->clear();
    while (QLayoutItem *item = bodyLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

void DiagReportView::setMessage(const QString &s) {
    hasReport_ = false;
    report_ = {};
    clearBody();
    search_->clear();
    severity_->setCurrentIndex(0);
    for (QWidget *w : {static_cast<QWidget *>(search_), static_cast<QWidget *>(severity_), static_cast<QWidget *>(sections_)}) w->setEnabled(false);
    snapshot_->clear();
    filterInfo_->clear();
    preview_->setEnabled(false); pin_->setEnabled(false); compare_->setEnabled(false); archive_->setEnabled(false); evidence_->setEnabled(false);
    verdict_->clear();
    verdict_->setVisible(false);
    headline_->setText(s);
    about_->setVisible(false);
    copy_->setEnabled(false);
    save_->setEnabled(false);
    bodyLayout_->addStretch();
}

void DiagReportView::setReport(const diag::Report &r) {
    // Preserve position and filters only for a replacement of the same visible report.
    const bool same = hasReport_ && report_.code == r.code;
    const int oldScroll = same ? scroll_->verticalScrollBar()->value() : 0;
    QMap<QString,TableViewState> previousTables;
    QMap<QString,int> oldCounts;
    if (same) {
        for (auto *tree : tables_) ++oldCounts[tree->property("workspaceTableKey").toString()];
        for (auto *tree : tables_) {
            const auto key = tree->property("workspaceTableKey").toString();
            if (oldCounts.value(key)==1) previousTables.insert(key,captureTable(tree));
        }
    }
    const diag::Report replacement = r;  // caller may pass report() itself
    clearBody();
    report_ = replacement;
    const diag::Report &current = report_;
    hasReport_ = true;
    if (!same) {
        const QSignalBlocker b1(search_), b2(severity_);
        search_->clear();
        severity_->setCurrentIndex(0);
    }
    search_->setEnabled(true);
    severity_->setEnabled(true);
    sections_->setEnabled(true);
    preview_->setEnabled(true); pin_->setEnabled(true); compare_->setEnabled(baselineCode_ == report_.code && !baselineText_.isEmpty());
    capturedAt_ = QDateTime::currentDateTimeUtc().toString(Qt::ISODate); captureBoot_ = diagsrc::bootId();
    archive_->setEnabled(!current.code.isEmpty()); evidence_->setEnabled(true);
    snapshot_->setText(tr("Report displayed: %1 · snapshot, not live data").arg(QDateTime::currentDateTime().toString(Qt::ISODate)));
    sections_->addItem(tr("Jump to section…"));
    auto addSection = [this](const QString &name, QWidget *w) {
        sections_->addItem(name);
        sectionWidgets_ << w;
    };
    const Level v = current.verdict();
    verdict_->setText(diag::levelName(v));
    verdict_->setStyleSheet(QStringLiteral("font-weight:700;color:%1").arg(color(v)));
    verdict_->setVisible(true);
    headline_->setText(QStringLiteral("· ") + current.headline());
    about_->setText(current.about);
    about_->setVisible(!current.about.isEmpty());
    copy_->setEnabled(true);
    save_->setEnabled(true);

    // Most serious first, the order within a level as the diagnostic wrote it.
    QList<diag::Finding> findings = current.findings;
    std::stable_sort(findings.begin(), findings.end(), [](const diag::Finding &a, const diag::Finding &b) { return a.level > b.level; });
    for (const diag::Finding &f : findings) {
        auto *card = findingCard(f, this);
        if (findingWidgets_.isEmpty()) addSection(tr("Findings"), card);
        findingWidgets_ << card;
        bodyLayout_->addWidget(card);
    }
    diag::Table edidBytes, edidFields;
    for (const auto &table : current.tables) {
        if (table.viewKind == QStringLiteral("edid-bytes")) edidBytes = table;
        if (table.viewKind == QStringLiteral("edid-fields")) edidFields = table;
    }
    for (const diag::Table &t : current.tables) {
        auto *w = tableWidget(t, t.viewKind == QStringLiteral("acpi-wmi-tree") ? acpiTreeState_ : hardwareTreeState_);
        auto *tree = w->findChild<QTreeWidget *>();
        if (tree) tables_ << tree;
        const bool rawEdid = t.viewKind == QStringLiteral("edid-bytes");
        if (tree && !edidBytes.rows.isEmpty() && (rawEdid || t.viewKind == QStringLiteral("edid-fields")
                                                  || t.viewKind == QStringLiteral("edid-timings"))) {
            auto *inspect = new QPushButton(rawEdid ? tr("Inspect captured EDID bytes…") : tr("Inspect selected byte offset…"));
            w->layout()->addWidget(inspect);
            const auto openEdid = [w, bytes = edidBytes, fields = edidFields, rawEdid](QTreeWidgetItem *item) {
                if (!item) {
                    if (rawEdid) inspectEdid(w, bytes, fields, {}, 0);
                    return;
                }
                bool valid = false;
                const int offset = item->text(rawEdid ? 2 : 1).toInt(&valid, 16);
                if (valid) inspectEdid(w, bytes, fields, item->text(0), offset);
            };
            connect(inspect, &QPushButton::clicked, w, [tree, openEdid] { openEdid(tree->currentItem()); });
            connect(tree, &QTreeWidget::itemDoubleClicked, w, [openEdid](QTreeWidgetItem *item, int) { openEdid(item); });
            if (!rawEdid) {
                const auto update = [tree, inspect] { inspect->setEnabled(tree->currentItem() != nullptr); };
                connect(tree, &QTreeWidget::currentItemChanged, w, update);
                update();
            }
        }
        if(t.viewKind==QStringLiteral("acpi-wmi-tree")) {
            auto *listen=new QPushButton(tr("Live ACPI notifications…"));w->layout()->addWidget(listen);
            connect(listen,&QPushButton::clicked,w,[w,t,state=acpiTreeState_]{inspectAcpiNotifications(w,t,state);});
        }
        if(t.viewKind==QStringLiteral("pd-ports")) {
            diag::Table profiles,supplies;
            for(const auto &candidate:current.tables){if(candidate.viewKind==QStringLiteral("pd-profiles"))profiles=candidate;else if(candidate.viewKind==QStringLiteral("pd-supplies"))supplies=candidate;}
            auto *inspect=new QPushButton(tr("Visualize captured PD profiles…"));w->layout()->addWidget(inspect);
            connect(inspect,&QPushButton::clicked,w,[w,t,profiles,supplies]{inspectPd(w,t,profiles,supplies);});
        }
        addSection(t.title, w);
        bodyLayout_->addWidget(w);
    }
    if (!current.limits.isEmpty()) {
        auto *lim = text(tr("Limits of this check:") + QStringLiteral("\n• ") + current.limits.join(QStringLiteral("\n• ")), "muted");
        addSection(tr("Limits of this check:"), lim);
        bodyLayout_->addWidget(lim);
    }
    if (!current.raw.trimmed().isEmpty()) {
        auto *toggle = new QPushButton(tr("Show technical details"));
        toggle->setCheckable(true);
        auto *raw = new QPlainTextEdit(current.raw.trimmed());
        raw->setReadOnly(true);
        raw->setLineWrapMode(QPlainTextEdit::NoWrap);
        raw->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        raw->setMinimumHeight(220);
        raw->setVisible(false);
        connect(toggle, &QPushButton::toggled, this, [toggle, raw](bool on) {
            raw->setVisible(on);
            toggle->setText(on ? tr("Hide technical details") : tr("Show technical details"));
        });
        // The choice is remembered for every report: who reads the raw output once usually wants it again.
        connect(toggle, &QPushButton::clicked, this, [](bool on) { guiSettings().setValue(QStringLiteral("diag/technical"), on); });
        toggle->setChecked(guiSettings().value(QStringLiteral("diag/technical"), false).toBool());
        addSection(tr("Technical details"), toggle);
        bodyLayout_->addWidget(toggle, 0, Qt::AlignLeft);
        bodyLayout_->addWidget(raw);
    }
    bodyLayout_->addStretch();
    filterReport();
    bodyLayout_->activate();
    QMap<QString,int> newCounts;
    for (auto *tree : tables_) ++newCounts[tree->property("workspaceTableKey").toString()];
    for (auto *tree : tables_) {
        const auto key = tree->property("workspaceTableKey").toString();
        if (newCounts.value(key)==1 && previousTables.contains(key)) restoreTable(tree,previousTables.value(key));
    }
    scroll_->verticalScrollBar()->setValue(oldScroll);
}

void DiagReportView::filterReport() {
    if (!hasReport_) return;
    const QString needle = search_->text().trimmed();
    const int level = severity_->currentData().toInt();
    int visibleFindings = 0, visibleRows = 0, allRows = 0;
    for (QWidget *w : findingWidgets_) {
        const bool match = (level < 0 || w->property("diagLevel").toInt() == level)
            && w->property("diagSearch").toString().contains(needle, Qt::CaseInsensitive);
        w->setVisible(match);
        visibleFindings += match;
    }
    // Keep tables, notes and limits available even when all findings are filtered out.
    for (QTreeWidget *tree : tables_) {
        int visible = 0;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            auto *item = tree->topLevelItem(i);
            bool match = needle.isEmpty();
            for (int c = 0; c < tree->columnCount() && !match; ++c)
                match = item->text(c).contains(needle, Qt::CaseInsensitive);
            item->setHidden(!match);
            visibleRows += match;
            visible += match;
            ++allRows;
        }
        const int rowH = tree->fontMetrics().height() + 8;
        tree->setFixedHeight(rowH * qBound(1, visible, 14) + (tree->isHeaderHidden() ? 0 : tree->header()->sizeHint().height())
                             + 2 * tree->frameWidth() + 4 + tree->horizontalScrollBar()->sizeHint().height());
    }
    filterInfo_->setText(tr("Showing %1/%2 findings and %3/%4 table rows. Export includes the full report.")
        .arg(visibleFindings).arg(findingWidgets_.size()).arg(visibleRows).arg(allRows));
}

void DiagReportView::previewExport() {
    if (!hasReport_) return;
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Export preview"));
    auto *layout = new QVBoxLayout(&dlg);
    layout->addWidget(text(tr("Reports may contain private data. Automatic redaction is best effort only. Review and edit the preview before sharing.")));
    const diag::Report captured = report_;
    auto *raw = new QPlainTextEdit;
    const QString captureTime = capturedAt_, captureBoot = captureBoot_;
    auto *selection = new QTreeWidget; selection->setHeaderHidden(true);
    selection->setMaximumHeight(fontMetrics().height() * 9);
    auto add = [](QTreeWidgetItem *parent, const QString &label) {
        auto *item = new QTreeWidgetItem(parent, {label});
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, Qt::Checked); return item;
    };
    auto *findings = new QTreeWidgetItem(selection, {tr("Findings")});
    findings->setFlags(findings->flags() | Qt::ItemIsAutoTristate | Qt::ItemIsUserCheckable);
    for (const auto &f : captured.findings) {
        auto *item=add(findings,f.title);
        item->setFlags(item->flags()|Qt::ItemIsAutoTristate);
        add(item,tr("Finding title (required with detail or action)"));
        if(!f.detail.isEmpty()) add(item,tr("Finding detail"));
        if(!f.action.isEmpty()) add(item,tr("Suggested action"));
    }
    auto *tables = new QTreeWidgetItem(selection, {tr("Tables")});
    tables->setFlags(tables->flags() | Qt::ItemIsAutoTristate | Qt::ItemIsUserCheckable);
    findings->setCheckState(0, Qt::Checked);
    for (const auto &t : captured.tables) {
        auto *item=add(tables,t.title);
        item->setFlags(item->flags()|Qt::ItemIsAutoTristate);
        auto *columns=add(item,tr("Columns"));columns->setFlags(columns->flags()|Qt::ItemIsAutoTristate);
        int width=int(t.headers.size());for(const auto &row:t.rows)width=qMax(width,int(row.size()));
        for(int c=0;c<width;++c)add(columns,t.headers.value(c,tr("Column %1").arg(c+1)));
        auto *rows=add(item,tr("Rows"));rows->setFlags(rows->flags()|Qt::ItemIsAutoTristate);
        for(int r=0;r<t.rows.size();++r)add(rows,tr("Row %1: %2").arg(r+1).arg(t.rows.at(r).join(QStringLiteral(" · ")).left(160)));
        if (!t.note.isEmpty()) add(item,tr("Table note"));
        if (t.privateBinary) {item->setCheckState(0,Qt::Unchecked);item->setToolTip(0,tr("Contains binary identifiers; redaction cannot guarantee privacy."));}
    }
    auto *context = new QTreeWidgetItem(selection,{tr("Report context and limits")});
    context->setFlags(context->flags()|Qt::ItemIsAutoTristate|Qt::ItemIsUserCheckable);
    auto *about = add(context,tr("Report description"));
    if(captured.about.isEmpty()) about->setDisabled(true);
    auto *limits = add(context,tr("Source limitations")); limits->setFlags(limits->flags()|Qt::ItemIsAutoTristate);
    for(const auto &limit:captured.limits) add(limits,limit);
    context->setCheckState(0,Qt::Checked);
    layout->addWidget(selection);
    auto *includeRaw = new QCheckBox(tr("Include technical details")); includeRaw->setChecked(false);
    auto *redact = new QCheckBox(tr("Redact detected private data")); redact->setChecked(true);
    layout->addWidget(includeRaw); layout->addWidget(redact);
    auto *binary = new QCheckBox(tr("Allow selected private binary tables")); layout->addWidget(binary);
    auto *metadata=new QCheckBox(tr("Include capture time"));layout->addWidget(metadata);
    auto *bootMetadata=new QCheckBox(tr("Include capture boot identity"));layout->addWidget(bootMetadata);
    auto *privacyWarning=text(QString(),"muted"); layout->addWidget(privacyWarning);
    auto updatePrivacyWarning=[=]{
        QStringList warnings;
        if(includeRaw->isChecked()) warnings << tr("Technical details can contain unrecognized identifiers. Inspect them manually before sharing.");
        if(binary->isChecked()) warnings << tr("Binary tables may expose identifiers even when automatic redaction is enabled.");
        if(!redact->isChecked()) warnings << tr("Automatic redaction is disabled.");
        privacyWarning->setText(warnings.join(QLatin1Char('\n'))); privacyWarning->setVisible(!warnings.isEmpty());
    };
    connect(includeRaw,&QCheckBox::toggled,&dlg,updatePrivacyWarning);
    connect(binary,&QCheckBox::toggled,&dlg,updatePrivacyWarning);
    connect(redact,&QCheckBox::toggled,&dlg,updatePrivacyWarning); updatePrivacyWarning();
    auto *hashLabel=text(QString(),"muted"); layout->addWidget(hashLabel);
    auto *regenerate = new QPushButton(tr("Regenerate preview (replaces edits)")); layout->addWidget(regenerate);
    auto refresh = [=] {
        auto selected = captured;
        selected.findings.clear(); selected.tables.clear(); selected.limits.clear();
        if(about->checkState(0)!=Qt::Checked) selected.about.clear();
        for(int i=0;i<limits->childCount();++i) if(limits->child(i)->checkState(0)==Qt::Checked) selected.limits<<captured.limits.at(i);
        int excludedFindingFields=0;
        for (int i = 0; i < findings->childCount(); ++i) {
            const auto *item=findings->child(i);
            if(item->checkState(0)==Qt::Unchecked) continue;
            auto f=captured.findings.at(i);
            int c=1;
            // Title is the context for selected detail/action; never export an
            // orphan finding. The title checkbox only controls an otherwise empty finding.
            const bool titleSelected=item->child(0)->checkState(0)==Qt::Checked;
            if(!f.detail.isEmpty() && item->child(c++)->checkState(0)!=Qt::Checked){f.detail.clear();++excludedFindingFields;}
            if(!f.action.isEmpty() && item->child(c)->checkState(0)!=Qt::Checked){f.action.clear();++excludedFindingFields;}
            if(titleSelected || !f.detail.isEmpty() || !f.action.isEmpty()) selected.findings<<f;
        }
        int excludedRows=0,excludedColumns=0,excludedTables=0,excludedNotes=0;
        for (int i=0;i<tables->childCount();++i) {
            const auto *item=tables->child(i);const auto &source=captured.tables.at(i);
            if(item->checkState(0)==Qt::Unchecked || (source.privateBinary && !binary->isChecked())){++excludedTables;continue;}
            const auto *cols=item->child(0),*rows=item->child(1);
            diag::Table t=source;t.headers.clear();t.rows.clear();t.levels.clear();t.parents.clear();t.rowKeys.clear();
            if(item->childCount()>2 && item->child(2)->checkState(0)!=Qt::Checked){t.note.clear();++excludedNotes;}
            QList<int> included;
            for(int c=0;c<cols->childCount();++c){if(cols->child(c)->checkState(0)==Qt::Checked){included<<c;t.headers<<source.headers.value(c);}else ++excludedColumns;}
            if(included.isEmpty()){++excludedTables;continue;}
            for(int r=0;r<rows->childCount();++r){if(rows->child(r)->checkState(0)!=Qt::Checked){++excludedRows;continue;}QStringList cells;for(int c:included)cells<<source.rows.at(r).value(c);t.add(cells,source.levels.value(r,Level::Info));}
            selected.tables<<t;
        }
        if (!includeRaw->isChecked()) selected.raw.clear();
        if (redact->isChecked()) diag::redactPrivateFields(selected);
        QString result = tr("Verdict refers to the complete original report; selected sections follow.") + QStringLiteral("\n")
            + tr("Excluded: %1 findings, %2 tables, %3 rows, %4 columns.").arg(captured.findings.size()-selected.findings.size()).arg(excludedTables).arg(excludedRows).arg(excludedColumns)+QStringLiteral("\n");
        result+=tr("Excluded: %1 finding details/actions, %2 table notes, %3 source limitations.").arg(excludedFindingFields).arg(excludedNotes).arg(captured.limits.size()-selected.limits.size())+QStringLiteral("\n");
        if(metadata->isChecked())result+=tr("Captured: %1").arg(captureTime)+QStringLiteral("\n");
        if(bootMetadata->isChecked())result+=tr("Capture boot: %1").arg(captureBoot)+QStringLiteral("\n");
        result+=QStringLiteral("\n")+selected.toTextWithVerdict(captured);
        if (redact->isChecked()) {
            // The known full capture ID can occur without its field label.
            if(QRegularExpression(QStringLiteral("^[0-9a-fA-F]{8}-(?:[0-9a-fA-F]{4}-){3}[0-9a-fA-F]{12}$")).match(captureBoot).hasMatch())
                result.replace(captureBoot,QStringLiteral("[redacted]"),Qt::CaseInsensitive);
            result = diag::redactPrivateText(result);
        }
        raw->setPlainText(result);
    };
    connect(regenerate, &QPushButton::clicked, &dlg, refresh);

    connect(raw,&QPlainTextEdit::textChanged,&dlg,[=]{hashLabel->setText(tr("Preview SHA-256: %1").arg(QString::fromLatin1(QCryptographicHash::hash(raw->toPlainText().toUtf8(),QCryptographicHash::Sha256).toHex())));});
    refresh();
    raw->setReadOnly(false);
    raw->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    layout->addWidget(raw, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *copy = buttons->addButton(tr("Copy preview"), QDialogButtonBox::ActionRole);
    connect(copy, &QPushButton::clicked, &dlg, [raw] { QApplication::clipboard()->setText(raw->toPlainText()); });
    auto *save = buttons->addButton(tr("Save preview…"), QDialogButtonBox::ActionRole);
    auto *pending = text(QString(), "muted"); layout->addWidget(pending);
    auto markPending = [=]{pending->setText(tr("Selection changed. Regenerate preview before exporting."));copy->setEnabled(false);save->setEnabled(false);};
    connect(selection, &QTreeWidget::itemChanged, &dlg, markPending);
    connect(includeRaw, &QCheckBox::toggled, &dlg, markPending);
    connect(redact, &QCheckBox::toggled, &dlg, markPending);
    connect(metadata, &QCheckBox::toggled, &dlg, markPending);
    connect(bootMetadata, &QCheckBox::toggled, &dlg, markPending);
    connect(binary, &QCheckBox::toggled, &dlg, markPending);
    connect(regenerate, &QPushButton::clicked, &dlg, [=]{pending->clear();copy->setEnabled(true);save->setEnabled(true);});
    connect(save, &QPushButton::clicked, &dlg, [&dlg, raw] {
        const auto bytes = raw->toPlainText().toUtf8();
        const auto path = QFileDialog::getSaveFileName(&dlg, tr("Save preview"), {}, tr("Text files (*.txt)")); if (path.isEmpty()) return;
        QSaveFile f(path); if (!f.open(QIODevice::WriteOnly) || !f.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner) || f.write(bytes) != bytes.size() || !f.commit()) QMessageBox::warning(&dlg, tr("Save preview"), f.errorString());
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);
    dlg.resize(850, 600);
    dlg.exec();
}

void DiagReportView::archiveWorkspace() {
    if (!hasReport_ || report_.code.isEmpty()) return;
    const auto captured = diag::archiveSnapshot(report_);
    const QString capturedAt = capturedAt_, boot = captureBoot_, locale = i18n::effective();
    const QString dir = diagsrc::dataDir() + QStringLiteral("/diagnostic-history");
    const QString path = dir + QLatin1Char('/') + QString::fromLatin1(QCryptographicHash::hash(report_.code.toUtf8(), QCryptographicHash::Sha256).toHex()) + QStringLiteral(".json");
    if (!QDir().mkpath(dir)) { QMessageBox::warning(this, tr("Diagnostic History"), tr("Could not create history directory.")); return; }
    auto read = [&]() -> std::optional<QJsonArray> {
        if (!QFileInfo::exists(path)) return QJsonArray{};
        const auto o = diagsrc::loadJson(path);
        if (o.value(QStringLiteral("version")).toInt() != 1 || !o.value(QStringLiteral("entries")).isArray()) return std::nullopt;
        const auto entries = o.value(QStringLiteral("entries")).toArray();
        if (entries.size() > 20) return std::nullopt;
        QSet<QString> ids;
        for (const auto &v : entries) {
            const auto e = v.toObject();
            if (e.value(QStringLiteral("id")).toString().isEmpty() || !e.value(QStringLiteral("snapshot")).isObject()) return std::nullopt;
            const QString id = e.value(QStringLiteral("id")).toString();
            if (ids.contains(id)) return std::nullopt;
            ids.insert(id);
            const auto snap = e.value(QStringLiteral("snapshot")).toObject();
            if (snap.value(QStringLiteral("version")).toInt() != 1 || snap.value(QStringLiteral("code")) != captured.value(QStringLiteral("code")) || !snap.value(QStringLiteral("records")).isArray() || !diag::archiveDiff(snap, snap).isEmpty()) return std::nullopt;
        }
        return entries;
    };
    auto entries = read();
    if (!entries) { QMessageBox::warning(this, tr("Diagnostic History"), tr("History is damaged or uses an unsupported schema; it has not been overwritten.")); return; }
    QDialog dlg(this); dlg.setWindowTitle(tr("Diagnostic History")); dlg.resize(950, 600);
    auto *layout = new QVBoxLayout(&dlg);
    layout->addWidget(text(tr("Local, manually saved snapshots. Raw text and executable fixes are omitted; redaction is best effort. Up to 20 entries per report, 1 MiB total per report. Capture boot is not evidence boot.")));
    auto *list = new QComboBox; layout->addWidget(list);
    auto *output = new QPlainTextEdit; output->setReadOnly(true); layout->addWidget(output, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *save = buttons->addButton(tr("Archive current snapshot"), QDialogButtonBox::ActionRole);
    auto *remove = buttons->addButton(tr("Delete selected snapshot"), QDialogButtonBox::DestructiveRole);
    auto *editNote = buttons->addButton(tr("Edit snapshot note…"), QDialogButtonBox::ActionRole);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto render = [&] {
        const int i = list->currentIndex(); remove->setEnabled(i >= 0); editNote->setEnabled(i>=0);
        if (i < 0 || i >= entries->size()) { output->setPlainText(tr("No saved snapshots.")); return; }
        const auto entry = entries->at(i).toObject();
        if (entry.value(QStringLiteral("locale")).toString() != locale) { output->setPlainText(tr("Snapshot locale differs; translated field names cannot be compared reliably.")); return; }
        const auto diff = diag::archiveDiff(entry.value(QStringLiteral("snapshot")).toObject(), captured);
        output->setPlainText(tr("Stored capture: %1 · boot %2\nCurrent capture: %3 · boot %4\nRows are compared without order; changed rows appear as removed and added. A difference is not evidence of causation.\n\n")
            .arg(entry.value(QStringLiteral("time")).toString(), entry.value(QStringLiteral("boot")).toString(), capturedAt, boot)
            + (diff.isEmpty() ? tr("No structured differences.") : diff.join(QLatin1Char('\n')))
            + QStringLiteral("\n\n") + tr("Note: %1").arg(entry.value("note").toString()));
    };
    auto refill = [&] {
        const QSignalBlocker block(list); list->clear();
        for (const auto &v : *entries) { const auto e = v.toObject(); list->addItem(e.value(QStringLiteral("time")).toString() + QStringLiteral(" · ") + e.value(QStringLiteral("boot")).toString(), e.value(QStringLiteral("id")).toString()); }
        list->setCurrentIndex(list->count() - 1); render();
    };
    connect(list, &QComboBox::currentIndexChanged, &dlg, [&](int) { render(); });
    auto change = [&](bool adding) {
        const QString selected = list->currentData().toString();
        if (!adding && (selected.isEmpty() || QMessageBox::question(&dlg, tr("Diagnostic History"), tr("Delete only this archived snapshot?")) != QMessageBox::Yes)) return;
        QLockFile lock(path + QStringLiteral(".lock"));
        if (!lock.tryLock(0)) { QMessageBox::warning(&dlg, tr("Diagnostic History"), tr("History is currently in use.")); return; }
        auto latest = read();
        if (!latest) { QMessageBox::warning(&dlg, tr("Diagnostic History"), tr("History changed or is damaged.")); return; }
        if (adding) {
            for(const auto &v:*latest) {
                const auto e=v.toObject();
                if(e.value("time").toString()==capturedAt && e.value("boot").toString()==boot && e.value("locale").toString()==locale && e.value("snapshot").toObject()==captured) {
                    QMessageBox::information(&dlg,tr("Diagnostic History"),tr("This capture is already archived."));return;
                }
            }
            if (latest->size() >= 20) { QMessageBox::information(&dlg, tr("Diagnostic History"), tr("History is full. Delete an old snapshot first.")); return; }
            latest->append(QJsonObject{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"time", capturedAt}, {"boot", boot}, {"locale", locale}, {"snapshot", captured}});
        } else {
            for (qsizetype i = latest->size(); i > 0; --i) if (latest->at(i-1).toObject().value(QStringLiteral("id")).toString() == selected) latest->removeAt(i-1);
        }
        const QString error = diagsrc::saveJson(path, QJsonObject{{"version", 1}, {"entries", *latest}});
        lock.unlock();
        if (!error.isEmpty()) { QMessageBox::warning(&dlg, tr("Diagnostic History"), error); return; }
        entries = latest; refill();
    };
    connect(save, &QPushButton::clicked, &dlg, [&] { change(true); });
    connect(remove, &QPushButton::clicked, &dlg, [&] { change(false); });
    connect(editNote,&QPushButton::clicked,&dlg,[&]{
        const auto id=list->currentData().toString(); if(id.isEmpty()) return;
        const auto old=entries->at(list->currentIndex()).toObject().value("note").toString();
        bool ok=false;const auto note=QInputDialog::getMultiLineText(&dlg,tr("Diagnostic History"),tr("Local snapshot note (redaction is best effort)"),old,&ok);
        if(!ok) return;
        if(note.size()>4096) {QMessageBox::warning(&dlg,tr("Diagnostic History"),tr("Note exceeds 4096 characters."));return;}
        QLockFile lock(path+QStringLiteral(".lock"));
        if(!lock.tryLock(0)) {QMessageBox::warning(&dlg,tr("Diagnostic History"),tr("History is currently in use."));return;}
        auto latest=read();if(!latest) {QMessageBox::warning(&dlg,tr("Diagnostic History"),tr("History changed or is damaged."));return;}
        for(qsizetype i=0;i<latest->size();++i) {
            auto e=latest->at(i).toObject(); if(e.value("id").toString()!=id) continue;
            if(e.value("note").toString()!=old) {QMessageBox::warning(&dlg,tr("Diagnostic History"),tr("History changed or is damaged."));return;}
            e.insert("note",diag::redactPrivateText(note));latest->replace(i,e);
            const auto error=diagsrc::saveJson(path,QJsonObject{{"version",1},{"entries",*latest}});
            if(!error.isEmpty()) {QMessageBox::warning(&dlg,tr("Diagnostic History"),error);return;}
            entries=latest;refill();list->setCurrentIndex(list->findData(id));return;
        }
        QMessageBox::warning(&dlg,tr("Diagnostic History"),tr("History changed or is damaged."));
    });
    refill(); dlg.exec();
}

void DiagReportView::evidenceWorkspace() {
    if (!hasReport_) return;
    // Only explicit containment and parent edges from this report. No correlation is inferred.
    auto captured = report_;
    diag::redactPrivateFields(captured);
    QDialog dlg(this); dlg.setWindowTitle(tr("Evidence Relationships")); dlg.resize(880, 580);
    auto *layout = new QVBoxLayout(&dlg);
    layout->addWidget(text(tr("Direct references in this captured report, not causal relationships. No new hardware scan is performed.")));
    auto *filter = new QLineEdit; filter->setPlaceholderText(tr("Filter evidence…")); layout->addWidget(filter);
    auto *split = new QSplitter(Qt::Horizontal);
    auto *tree = new QTreeWidget; tree->setHeaderLabels({tr("Evidence"), tr("Relationship")});
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch); tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    auto *detail = new QPlainTextEdit; detail->setReadOnly(true);
    split->addWidget(tree); split->addWidget(detail); split->setStretchFactor(0, 3); split->setStretchFactor(1, 2); layout->addWidget(split, 1);
    auto *root = new QTreeWidgetItem(tree, {captured.code + QStringLiteral(" · ") + captured.title, tr("Captured report")});
    root->setData(0, Qt::UserRole, diag::redactPrivateText(captured.about) + QStringLiteral("\n") + capturedAt_ + QStringLiteral("\n") + captureBoot_);
    int nodes = 1;
    constexpr int MaxNodes = 400;
    auto node = [&](QTreeWidgetItem *parent, const QString &label, const QString &edge, const QString &content) -> QTreeWidgetItem * {
        if (nodes >= MaxNodes) return nullptr;
        ++nodes; auto *item = new QTreeWidgetItem(parent, {diag::redactPrivateText(label).left(300), edge});
        item->setData(0, Qt::UserRole, diag::redactPrivateText(content).left(8192)); return item;
    };
    for (const auto &f : captured.findings) {
        if (!node(root, f.title, tr("Contains finding"), diag::levelName(f.level) + QStringLiteral("\n") + f.detail + QStringLiteral("\n") + f.action)) break;
    }
    for (const auto &t : captured.tables) {
        if (t.privateBinary) continue;
        auto *table = node(root, t.title, tr("Contains readings"), t.note);
        if (!table) break;
        QList<QTreeWidgetItem *> items;
        for (qsizetype i = 0; i < t.rows.size(); ++i) {
            const auto &cells = t.rows.at(i);
            const int parent = t.parents.value(i, -1);
            const bool linked = parent >= 0 && parent < items.size();
            QStringList fields;
            for (qsizetype c = 0; c < cells.size(); ++c) fields << t.headers.value(c) + QStringLiteral(": ") + cells.at(c);
            auto *entry = node(linked ? items.at(parent) : table, cells.value(0), linked ? tr("Reported parent") : tr("Contains row"), fields.join(QLatin1Char('\n')));
            if (!entry) break;
            items.append(entry);
        }
    }
    for (const auto &limit : captured.limits) if (!node(root, limit, tr("Evidence limit"), limit)) break;
    if (nodes >= MaxNodes) layout->addWidget(text(tr("Evidence display limited to 400 nodes. The full report is unchanged.")));
    connect(tree, &QTreeWidget::currentItemChanged, detail, [detail](QTreeWidgetItem *item, QTreeWidgetItem *) { detail->setPlainText(item ? item->data(0, Qt::UserRole).toString() : QString()); });
    connect(filter, &QLineEdit::textChanged, tree, [root](const QString &query) {
        const auto visit = [&](auto &&self, QTreeWidgetItem *item) -> bool {
            bool match = query.isEmpty() || item->text(0).contains(query, Qt::CaseInsensitive) || item->text(1).contains(query, Qt::CaseInsensitive) || item->data(0, Qt::UserRole).toString().contains(query, Qt::CaseInsensitive);
            for (int i = 0; i < item->childCount(); ++i) match |= self(self, item->child(i));
            item->setHidden(!match); if (match && !query.isEmpty()) item->setExpanded(true); return match;
        }; visit(visit, root);
    });
    root->setExpanded(true); tree->setCurrentItem(root);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close); connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject); layout->addWidget(buttons); dlg.exec();
}
