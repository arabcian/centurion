#include "uinavigator.h"
#include "navigationtargets.h"
#include <QMessageBox>
#include "experimentalui.h"
#include "healthtab.h"
#include "optimizetab.h"
#include "tuningnavigation.h"
#include "toolcatalog.h"
#include <QScreen>
#include <algorithm>

#include <QAbstractButton>
#include <QCoreApplication>
#include <QDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QHash>
#include <QSet>
#include <QScrollArea>
#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QVector>

namespace uinavigator {
namespace {
struct Destination {
    int tab = -1;
    QPointer<QWidget> page;
    QPointer<QWidget> target;
    QPointer<QTabWidget> inner;
    int innerIndex = -1;
    QPointer<HealthTab> health;
    QString healthTarget;
    QPointer<OptimizeTab> optimize;
    QString settingKey;
    QString stableTarget;
    QString targetTitle;
    QString navigationNote;
    QString favoriteId;
    QString title;
    QString search;
};
// The search field keeps the focus; Up/Down/PageUp/PageDown move the result selection.
class ListKeys : public QObject {
public:
    ListKeys(QListWidget *list, QObject *parent) : QObject(parent), list_(list) {}
protected:
    bool eventFilter(QObject *, QEvent *e) override {
        if (e->type() != QEvent::KeyPress) return false;
        const int key = static_cast<QKeyEvent *>(e)->key();
        if (key != Qt::Key_Up && key != Qt::Key_Down && key != Qt::Key_PageUp && key != Qt::Key_PageDown) return false;
        if (!list_->count()) return true;
        const int page = std::max(1, list_->viewport()->height() / std::max(1, list_->sizeHintForRow(0)) - 1);
        const int step = key == Qt::Key_Up ? -1 : key == Qt::Key_Down ? 1 : key == Qt::Key_PageUp ? -page : page;
        list_->setCurrentRow(std::clamp(list_->currentRow() + step, 0, list_->count() - 1));
        return true;
    }
private:
    QListWidget *list_;
};
}

void open(QTabWidget *tabs, QWidget *parent) {
    if (!tabs) return;
    // A command palette: no title bar, the search field on top, results below.
    QDialog dialog(parent, Qt::Dialog | Qt::FramelessWindowHint);
    dialog.setObjectName(QStringLiteral("expPalette"));
    dialog.setWindowTitle(QObject::tr("Quick Navigation — Centurion"));
    dialog.resize(parent ? QSize(620, 460).boundedTo(parent->screen()->availableGeometry().size() - QSize(32, 32)) : QSize(620, 460));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(1, 1, 1, 1);  // the palette's own hairline frame
    layout->setSpacing(0);
    auto *searchRow = new QHBoxLayout;
    searchRow->setContentsMargins(16, 6, 12, 6);
    searchRow->setSpacing(10);
    auto *searchIcon = new QLabel;
    searchIcon->setPixmap(experimentalui::searchIcon(theme::ACCENT).pixmap(16, 16));
    searchRow->addWidget(searchIcon);
    auto *search = new QLineEdit;
    search->setObjectName(QStringLiteral("experimentalQuickSearch"));
    search->setPlaceholderText(QObject::tr("CPU, fan, Health, Scenes, tuning…"));
    search->setClearButtonEnabled(true);
    search->setMaxLength(256);
    searchRow->addWidget(search, 1);
    auto *escape = new QLabel(QStringLiteral("Esc"));
    escape->setObjectName(QStringLiteral("expKeycap"));
    searchRow->addWidget(escape, 0, Qt::AlignVCenter);
    layout->addLayout(searchRow);
    auto *results = new QListWidget;
    results->setObjectName(QStringLiteral("experimentalQuickResults"));
    results->setUniformItemSizes(true);
    results->setAlternatingRowColors(false);
    results->setIconSize(QSize(18, 18));
    results->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(results, 1);
    auto *hint = new QLabel(QObject::tr("Search pages, tools and settings. Enter navigates; it never applies a setting."));
    hint->setObjectName(QStringLiteral("expPaletteHint"));
    auto *footer = new QHBoxLayout;
    footer->addWidget(hint, 1);
    auto *favorite = new QPushButton(QObject::tr("Add favorite"));
    favorite->setEnabled(false);
    footer->addWidget(favorite);
    layout->addLayout(footer);
    search->installEventFilter(new ListKeys(results, &dialog));

    QVector<Destination> destinations;
    for (int i = 0; i < tabs->count(); ++i) {
        const QString label = tabs->tabText(i);
        if (!tabs->isTabEnabled(i)) continue;
        const QString detail = tabs->tabToolTip(i);
        Destination dest;
        dest.tab = i;
        dest.page = tabs->widget(i);
        dest.title = QObject::tr("Page  /  %1").arg(label);
        dest.search = label + QLatin1Char(' ') + detail;
        const QString pageId = tabs->tabBar()->tabData(i).toString();
        if (!pageId.isEmpty()) dest.favoriteId = QStringLiteral("page/") + pageId;
        destinations.append(dest);
        if (auto *health = qobject_cast<HealthTab *>(tabs->widget(i))) {
            for (const auto &entry : health->navigationEntries()) {
                Destination tool;
                tool.tab = i;
                tool.page = tabs->widget(i);
                tool.health = health;
                tool.healthTarget = entry.target;
                tool.favoriteId = QStringLiteral("health/") + entry.target;
                if (entry.target.startsWith(QLatin1String("tool:"))) {
                    const auto info = toolcatalog::tool(entry.target.mid(5));
                    tool.navigationNote = toolcatalog::badges(info.flags).join(QStringLiteral(" · "));
                }
                tool.title = QObject::tr("%1  /  %2").arg(label, entry.label);
                tool.search = tool.title + QLatin1Char(' ') + entry.keywords.join(QLatin1Char(' '));
                destinations.append(tool);
            }
        }
    }
    // Static metadata is available even if describe has never run. Hardware support
    // remains unknown until the user actually opens a destination.
    OptimizeTab *optimize = nullptr; HealthTab *healthPage = nullptr;
    int optIndex = -1, healthIndex = -1;
    for (int i = 0; i < tabs->count(); ++i) {
        if (auto *o = qobject_cast<OptimizeTab *>(tabs->widget(i))) { optimize = o; optIndex = i; }
        if (auto *h = qobject_cast<HealthTab *>(tabs->widget(i))) { healthPage = h; healthIndex = i; }
    }
    if (optimize && optIndex >= 0 && tabs->isTabEnabled(optIndex)) {
        for (const auto &entry : tuningnavigation::entries) {
            Destination d;
            d.optimize = optimize; d.settingKey = QString::fromLatin1(entry.key);
            d.favoriteId = QStringLiteral("setting/") + d.settingKey;
            d.tab = optIndex; d.page = optimize;
            const QString group = QCoreApplication::translate("OptimizationGroups", entry.group);
            const QString label = QCoreApplication::translate("OptimizationLabels", entry.label);
            const QString home = QString::fromLatin1(entry.home);
            if (!home.isEmpty()) {
                if (!healthPage || !tabs->isTabEnabled(healthIndex)) continue;
                d.tab = healthIndex; d.page = healthPage; d.health = healthPage;
                d.healthTarget = QStringLiteral("tool:") + home.left(1).toUpper() + home.mid(1) + QStringLiteral(" tuning");
            }
            d.title = QObject::tr("%1 / %2 · %3").arg(tabs->tabText(d.tab), group, label);
            d.search = d.settingKey + QLatin1Char(' ') + QString::fromUtf8(entry.label) + QLatin1Char(' ') + group + QLatin1Char(' ') + label;
            destinations.append(d);
        }
    }
    // Registered targets are metadata on already constructed editors, including
    // firmware rows in unopened tabs. Never discover(), read, or show a page here.
    for (int i = 0; i < tabs->count(); ++i) {
        if (!tabs->isTabEnabled(i)) continue;
        QWidget *page = tabs->widget(i);
        QSet<QString> registered;
        auto add = [&](const navigationtargets::Entry &entry, bool capabilityUnknown) {
            if (entry.id.isEmpty() || registered.contains(entry.id)) return;
            registered.insert(entry.id);
            Destination d;
            d.tab = i; d.page = page; d.stableTarget = entry.id;
            const auto pageId = tabs->tabBar()->tabData(i).toString();
            if (!pageId.isEmpty()) d.favoriteId = QStringLiteral("control/") + pageId + QLatin1Char('/') + entry.id;
            const QString translated = QCoreApplication::translate("NavigationTargets", entry.title.toUtf8().constData());
            d.targetTitle = translated;
            d.title = QObject::tr("%1 / %2").arg(tabs->tabText(i), translated);
            if (capabilityUnknown) d.title += QObject::tr(" (capability not confirmed)");
            d.navigationNote = QObject::tr("Navigation only. Opening the page can run its existing device reads; availability may change.");
            if (pageId == QLatin1String("NVIDIA Curve") || pageId == QLatin1String("AMD GPU"))
                d.navigationNote += QLatin1Char(' ') + QObject::tr("Opening the GPU page may wake a device.");
            d.search = entry.id + QLatin1Char(' ') + entry.title + QLatin1Char(' ') + translated + QLatin1Char(' ') + entry.aliases;
            destinations.append(d);
        };
        for (const auto &entry : navigationtargets::entries(page)) add(entry, false);
        if (tabs->tabBar()->tabData(i).toString() == QLatin1String("AMD GPU")) {
            for (const auto &entry : navigationtargets::amdEntries)
                add({QString::fromLatin1(entry.id), QString::fromLatin1(entry.title), QString::fromUtf8(entry.aliases)}, true);
        }
    }
    // Only *already visible* controls are indexed. Building unopened specialist
    // pages here could wake a sleeping dGPU or trigger a privileged probe.
    if (QWidget *page = tabs->currentWidget()) {
        const QString prefix = tabs->tabText(tabs->currentIndex());
        int visibleControls = 0;
        for (QWidget *w : page->findChildren<QWidget *>()) {
            if (!w->isVisibleTo(page) || !w->isEnabled() || w->property(navigationtargets::Id).isValid()) continue;
            QString label;
            if (auto *group = qobject_cast<QGroupBox *>(w)) label = group->title();
            else if (auto *button = qobject_cast<QAbstractButton *>(w)) label = button->text();
            else continue;
            label.remove(QLatin1Char('&'));
            if (label.trimmed().isEmpty() || label.size() > 100) continue;
            if (++visibleControls > 150) break;
            Destination dest;
            dest.tab = tabs->currentIndex();
            dest.page = page;
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
            for (int j = 0; j < inner->count(); ++j) {
                if (!inner->isTabEnabled(j)) continue;
                Destination dest;
                dest.tab = tabs->currentIndex();
                dest.page = page;
                dest.inner = inner;
                dest.innerIndex = j;
                dest.title = QObject::tr("%1  /  %2").arg(prefix, inner->tabText(j));
                dest.search = dest.title + QLatin1Char(' ') + inner->tabToolTip(j);
                destinations.append(dest);
            }
        }
    }
    for (int i=0; i<tabs->count(); ++i) {
        QWidget *page = tabs->widget(i);
        if (i == tabs->currentIndex() || !tabs->isTabEnabled(i)) continue;
        for (QTabWidget *inner : page->findChildren<QTabWidget *>()) {
            for (int j=0; j<inner->count(); ++j) {
                if (!inner->isTabEnabled(j)) continue;
                Destination d; d.tab=i; d.page=page; d.inner=inner; d.innerIndex=j;
                d.title=QObject::tr("%1  /  %2").arg(tabs->tabText(i),inner->tabText(j));
                d.search=d.title+QLatin1Char(' ')+inner->tabToolTip(j); destinations.append(d);
            }
        }
    }
    QHash<QString,int> stableCounts;
    for (const auto &d : destinations) if (!d.stableTarget.isEmpty()) ++stableCounts[d.stableTarget];
    destinations.erase(std::remove_if(destinations.begin(), destinations.end(), [&](const Destination &d) {
        return !d.stableTarget.isEmpty() && stableCounts.value(d.stableTarget) != 1;
    }), destinations.end());
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("centurion"), QStringLiteral("gui"));
    const QString favoriteKey = QStringLiteral("ui/navigation_favorites_v1");
    const auto selectedFavorite = [&]() -> QString {
        const auto *item = results->currentItem();
        if (!item) return {};
        const int i = item->data(Qt::UserRole).toInt();
        return i >= 0 && i < destinations.size() ? destinations.at(i).favoriteId : QString();
    };
    const auto updateFavorite = [&] {
        const auto id = selectedFavorite();
        favorite->setEnabled(!id.isEmpty());
        favorite->setText(settings.value(favoriteKey).toStringList().contains(id) ? QObject::tr("Remove favorite") : QObject::tr("Add favorite"));
    };
    QObject::connect(results, &QListWidget::currentItemChanged, &dialog, [&] { updateFavorite(); });
    QObject::connect(favorite, &QPushButton::clicked, &dialog, [&] {
        const auto id = selectedFavorite(); if (id.isEmpty()) return;
        auto list = settings.value(favoriteKey).toStringList();
        if (list.contains(id)) list.removeAll(id);
        else if (list.size() < 512) list.append(id);
        else { QMessageBox::information(&dialog, QObject::tr("Navigation favorites"), QObject::tr("Favorite limit reached.")); return; }
        settings.setValue(favoriteKey,list); updateFavorite();
    });
    // Each result carries the icon of the page it leads to.
    QVector<QIcon> icons;
    for (int i = 0; i < tabs->count(); ++i)
        icons.append(experimentalui::navigationIcon(tabs->tabBar()->tabData(i).toString()));
    auto refill = [&] {
        results->clear();
        const QString filter = search->text().trimmed();
        QList<QPair<int, int>> ranked;
        for (int i = 0; i < destinations.size(); ++i) {
            const Destination &d = destinations.at(i);
            const bool exactKey = (!d.settingKey.isEmpty() && filter.compare(d.settingKey, Qt::CaseInsensitive) == 0)
                || (!d.stableTarget.isEmpty() && filter.compare(d.stableTarget, Qt::CaseInsensitive) == 0);
            const bool exactTitle = !d.targetTitle.isEmpty() && toolcatalog::normalize(filter) == toolcatalog::normalize(d.targetTitle);
            const int score = exactKey ? 12000 : exactTitle ? 11000 : toolcatalog::navigationScore(filter, d.title, {d.search});
            if (score >= 0) ranked.append(qMakePair(score, i));
        }
        std::stable_sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        for (int n = 0; n < ranked.size() && n < 100; ++n) {
            const int i = ranked.at(n).second;
            const Destination &d = destinations.at(i);
            auto *item = new QListWidgetItem(icons.value(d.tab), d.title, results);
            item->setData(Qt::UserRole, i);
            if (!d.navigationNote.isEmpty()) item->setToolTip(d.navigationNote);
        }
        if (results->count()) results->setCurrentRow(0);
    };
    QObject::connect(search, &QLineEdit::textChanged, &dialog, refill);
    int selectedDestination = -1;
    auto select = [&] {
        QListWidgetItem *item = results->currentItem();
        if (!item) return;
        const int i = item->data(Qt::UserRole).toInt();
        if (i < 0 || i >= destinations.size()) return;
        selectedDestination = i;
        dialog.accept();
    };
    auto navigate = [&](const Destination &d) {
        const int pageIndex = d.page ? tabs->indexOf(d.page) : -1;
        if (pageIndex < 0 || !tabs->isTabEnabled(pageIndex)) return;
        tabs->setCurrentIndex(pageIndex);
        if (!d.stableTarget.isEmpty()) {
            // Firmware showEvent may rebuild the entire row list. Resolve the ID
            // only now; never keep a pointer to a stale firmware editor.
            if (!d.page || !navigationtargets::reveal(d.page.data(), d.stableTarget))
                QMessageBox::information(parent, QObject::tr("Navigation target unavailable"),
                    QObject::tr("This control is not available in the current page state. No setting was applied."));
            return;
        }
        if (d.inner && d.innerIndex >= 0 && d.innerIndex < d.inner->count())
            d.inner->setCurrentIndex(d.innerIndex);
        if (d.health && !d.health->openNavigationTarget(d.healthTarget)) return;
        if (d.optimize && !d.settingKey.isEmpty()) d.optimize->navigateSetting(d.settingKey);
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
    };
    QObject::connect(results, &QListWidget::itemActivated, &dialog, [&](QListWidgetItem *) { select(); });
    QObject::connect(search, &QLineEdit::returnPressed, &dialog, select);
    refill();
    search->setFocus();
    // Upper third of the window, where the top bar's search field is.
    if (parent) {
        const QPoint origin = parent->mapToGlobal(QPoint(0, 0));
        dialog.move(origin.x() + (parent->width() - dialog.width()) / 2,
                    origin.y() + std::max(48, (parent->height() - dialog.height()) / 5));
    }
    if (dialog.exec() == QDialog::Accepted && selectedDestination >= 0)
        navigate(destinations.at(selectedDestination));
}
}  // namespace uinavigator
