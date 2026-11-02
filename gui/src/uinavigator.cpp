#include "uinavigator.h"

#include <QAbstractButton>
#include <QDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QScrollArea>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVector>

namespace uinavigator {
namespace {
struct Destination {
    int tab = -1;
    QPointer<QWidget> target;
    QPointer<QTabWidget> inner;
    int innerIndex = -1;
    QPointer<QTreeWidget> tree;
    QTreeWidgetItem *treeItem = nullptr;
    QString title;
    QString search;
};
}

void open(QTabWidget *tabs, QWidget *parent) {
    if (!tabs) return;
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Quick navigation — Centurion"));
    dialog.resize(560, 490);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(8);
    auto *heading = new QLabel(QObject::tr("Jump to a section"));
    heading->setProperty("role", "title");
    layout->addWidget(heading);
    auto *hint = new QLabel(QObject::tr("Search tabs and visible controls. Enter navigates; it never applies a setting."));
    hint->setProperty("role", "muted");
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *search = new QLineEdit;
    search->setObjectName(QStringLiteral("experimentalQuickSearch"));
    search->setPlaceholderText(QObject::tr("CPU, fan, Health, Scenes, tuning…"));
    search->setClearButtonEnabled(true);
    layout->addWidget(search);
    auto *results = new QListWidget;
    results->setObjectName(QStringLiteral("experimentalQuickResults"));
    results->setUniformItemSizes(true);
    results->setAlternatingRowColors(false);
    layout->addWidget(results, 1);

    QVector<Destination> destinations;
    for (int i = 0; i < tabs->count(); ++i) {
        const QString label = tabs->tabText(i);
        if (!tabs->isTabEnabled(i)) continue;
        const QString detail = tabs->tabToolTip(i);
        Destination dest;
        dest.tab = i;
        dest.title = QObject::tr("Page  /  %1").arg(label);
        dest.search = label + QLatin1Char(' ') + detail;
        destinations.append(dest);
    }
    // Only *already visible* controls are indexed. Building unopened specialist
    // pages here could wake a sleeping dGPU or trigger a privileged probe.
    if (QWidget *page = tabs->currentWidget()) {
        const QString prefix = tabs->tabText(tabs->currentIndex());
        for (QWidget *w : page->findChildren<QWidget *>()) {
            if (!w->isVisibleTo(page) || !w->isEnabled()) continue;
            QString label;
            if (auto *group = qobject_cast<QGroupBox *>(w)) label = group->title();
            else if (auto *button = qobject_cast<QAbstractButton *>(w)) label = button->text();
            else continue;
            label.remove(QLatin1Char('&'));
            if (label.trimmed().isEmpty() || label.size() > 100) continue;
            if (destinations.size() >= 220) break;
            Destination dest;
            dest.tab = tabs->currentIndex();
            dest.target = w;
            dest.title = QObject::tr("%1  /  %2").arg(prefix, label);
            dest.search = prefix + QLatin1Char(' ') + label + QLatin1Char(' ') + w->toolTip();
            destinations.append(dest);
        }
        // Discover existing sub-tab names without activating them. This keeps
        // Optimize's CPU/Storage/Network/Boot tools searchable without forcing
        // the LazyWidget content to initialize (or making extra device reads).
        for (QTabWidget *inner : page->findChildren<QTabWidget *>()) {
            if (!inner->isVisibleTo(page)) continue;
            for (int j = 0; j < inner->count() && destinations.size() < 250; ++j) {
                if (!inner->isTabEnabled(j)) continue;
                Destination dest;
                dest.tab = tabs->currentIndex();
                dest.inner = inner;
                dest.innerIndex = j;
                dest.title = QObject::tr("%1  /  %2").arg(prefix, inner->tabText(j));
                dest.search = dest.title + QLatin1Char(' ') + inner->tabToolTip(j);
                destinations.append(dest);
            }
        }
        // Health Center's one-level tree is also a first-class navigation
        // target. Skip headers, hidden groups and the inner tool directory.
        for (QTreeWidget *tree : page->findChildren<QTreeWidget *>()) {
            if (!tree->isVisibleTo(page) || tree->objectName() == QLatin1String("experimentalQuickResults")) continue;
            for (int r = 0; r < tree->topLevelItemCount(); ++r) {
                QTreeWidgetItem *root = tree->topLevelItem(r);
                for (int j = 0; j < root->childCount() && destinations.size() < 300; ++j) {
                    QTreeWidgetItem *item = root->child(j);
                    if (item->isHidden() || !(item->flags() & Qt::ItemIsSelectable)) continue;
                    Destination dest;
                    dest.tab = tabs->currentIndex();
                    dest.tree = tree;
                    dest.treeItem = item;
                    dest.title = QObject::tr("%1  /  %2").arg(prefix, item->text(0));
                    dest.search = prefix + QLatin1Char(' ') + root->text(0) + QLatin1Char(' ') + item->text(0);
                    destinations.append(dest);
                }
            }
        }
    }
    auto refill = [&] {
        results->clear();
        const QString filter = search->text().trimmed();
        for (int i = 0; i < destinations.size(); ++i) {
            const Destination &d = destinations.at(i);
            if (!filter.isEmpty() && !d.search.contains(filter, Qt::CaseInsensitive)) continue;
            auto *item = new QListWidgetItem(d.title, results);
            item->setData(Qt::UserRole, i);
        }
        if (results->count()) results->setCurrentRow(0);
    };
    QObject::connect(search, &QLineEdit::textChanged, &dialog, refill);
    auto select = [&] {
        QListWidgetItem *item = results->currentItem();
        if (!item) return;
        const int i = item->data(Qt::UserRole).toInt();
        if (i < 0 || i >= destinations.size()) return;
        const Destination d = destinations.at(i);
        if (d.tab < 0 || d.tab >= tabs->count()) return;
        tabs->setCurrentIndex(d.tab);
        if (d.inner && d.innerIndex >= 0 && d.innerIndex < d.inner->count())
            d.inner->setCurrentIndex(d.innerIndex);
        if (d.tree && d.treeItem) d.tree->setCurrentItem(d.treeItem);
        if (d.target) {
            // Find the nearest scroll area. Focus is purely navigational and
            // never clicks the widget, adjusts a value or writes hardware.
            for (QWidget *p = d.target->parentWidget(); p; p = p->parentWidget()) {
                if (auto *scroll = qobject_cast<QScrollArea *>(p)) {
                    scroll->ensureWidgetVisible(d.target); break;
                }
            }
            if (d.target->focusPolicy() != Qt::NoFocus) d.target->setFocus(Qt::OtherFocusReason);
        }
        dialog.accept();
    };
    QObject::connect(results, &QListWidget::itemActivated, &dialog, [&](QListWidgetItem *) { select(); });
    QObject::connect(search, &QLineEdit::returnPressed, &dialog, select);
    refill();
    search->setFocus();
    dialog.exec();
}
}  // namespace uinavigator
