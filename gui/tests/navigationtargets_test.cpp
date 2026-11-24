#include "../src/navigationtargets.h"
#include "../src/lazywidget.h"
#include <QApplication>
#include <QPushButton>
#include <QVBoxLayout>
#include <cstdio>
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); ++failures; } } while (0)
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QWidget page;
    auto *layout = new QVBoxLayout(&page);
    auto *tabs = new QTabWidget;
    layout->addWidget(tabs);
    auto *a = new QWidget;
    auto *b = new QWidget;
    tabs->addTab(a, "First"); tabs->addTab(b, "Second");
    int constructs = 0, writes = 0;
    auto *lazy = new LazyWidget([&] { ++constructs; return new QWidget; });
    tabs->addTab(lazy, "Lazy");
    auto *bl = new QVBoxLayout(b);
    auto *button = new QPushButton("Do not click"); bl->addWidget(button);
    QObject::connect(button, &QPushButton::clicked, &page, [&] { ++writes; });
    navigationtargets::mark(button, "fw/device/attr", "Firmware setting", "güç");
    page.show(); app.processEvents();
    const auto initial = navigationtargets::entries(&page);
    CHECK(initial.size() == 1 && initial.first().id == "fw/device/attr");
    CHECK(constructs == 0 && writes == 0 && tabs->currentIndex() == 0);
    CHECK(navigationtargets::reveal(&page, "fw/device/attr"));
    CHECK(tabs->currentIndex() == 1 && constructs == 0 && writes == 0);
    navigationtargets::forget(button);
    button->deleteLater();
    button = new QPushButton("Replacement"); bl->addWidget(button);
    navigationtargets::mark(button, "fw/device/attr", "Firmware setting");
    CHECK(navigationtargets::resolve(&page, "fw/device/attr") == button);
    auto *duplicate = new QPushButton("Duplicate"); bl->addWidget(duplicate);
    navigationtargets::mark(duplicate, "fw/device/attr", "Ambiguous");
    CHECK(navigationtargets::entries(&page).isEmpty());
    CHECK(!navigationtargets::reveal(&page, "fw/device/attr"));
    delete duplicate;
    button->hide(); CHECK(!navigationtargets::reveal(&page, "fw/device/attr"));
    button->show(); tabs->setCurrentIndex(0); tabs->setTabEnabled(1, false);
    CHECK(!navigationtargets::reveal(&page, "fw/device/attr"));
    CHECK(tabs->currentIndex() == 0 && constructs == 0 && writes == 0);
    QSet<QString> ids;
    for (const auto &e : navigationtargets::amdEntries) {
        const QString id = QString::fromLatin1(e.id);
        CHECK(!ids.contains(id)); ids.insert(id);
        QWidget candidate;
        navigationtargets::markAmd(&candidate, QString::fromLatin1(e.title));
        CHECK(candidate.property(navigationtargets::Id).toString() == id);
    }
    return failures ? 1 : 0;
}
