#include "uinavigator.h"
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
    auto *hint = new QLabel(QObject::tr("Search pages, Health tools and visible controls. Enter navigates; it never applies a setting."));
    hint->setObjectName(QStringLiteral("expPaletteHint"));
    layout->addWidget(hint);
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
        destinations.append(dest);
        if (auto *health = qobject_cast<HealthTab *>(tabs->widget(i))) {
            for (const auto &entry : health->navigationEntries()) {
                Destination tool;
                tool.tab = i;
                tool.page = tabs->widget(i);
                tool.health = health;
                tool.healthTarget = entry.target;
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
    // Only *already visible* controls are indexed. Building unopened specialist
    // pages here could wake a sleeping dGPU or trigger a privileged probe.
    if (QWidget *page = tabs->currentWidget()) {
        const QString prefix = tabs->tabText(tabs->currentIndex());
        int visibleControls = 0;
        for (QWidget *w : page->findChildren<QWidget *>()) {
            if (!w->isVisibleTo(page) || !w->isEnabled()) continue;
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
            const int score = !d.settingKey.isEmpty() && filter.compare(d.settingKey, Qt::CaseInsensitive) == 0
                ? 12000 : toolcatalog::navigationScore(filter, d.title, {d.search});
            if (score >= 0) ranked.append(qMakePair(score, i));
        }
        std::stable_sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        for (int n = 0; n < ranked.size() && n < 100; ++n) {
            const int i = ranked.at(n).second;
            const Destination &d = destinations.at(i);
            auto *item = new QListWidgetItem(icons.value(d.tab), d.title, results);
            item->setData(Qt::UserRole, i);
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
