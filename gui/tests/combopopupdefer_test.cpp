// ComboPopupDefer: a combo box on a page that is never shown gets no popup under a style sheet, and a combo
// shown later is styled and measured exactly like one that was visible all along.
// Plain asserts (no QtTest dependency); runs on the offscreen platform.
#include "../src/combopopupdefer.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static bool hasPopup(const QComboBox *c) {
    for (const QObject *child : c->children())
        if (child->inherits("QComboBoxPrivateContainer")) return true;
    return false;
}

static QComboBox *combo() {
    auto *c = new QComboBox;
    c->addItems({QStringLiteral("Balanced"), QStringLiteral("Performance (long entry)")});
    return c;
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.installEventFilter(new ComboPopupDefer(&app));

    // Page 0 visible with combo `shown` (Qt's own handling: the reference), page 1 never shown with combo
    // `hidden`; the window is laid out (both size hints cached) before the style sheet arrives, as in main().
    QWidget window;
    auto *stack = new QStackedWidget;
    (new QVBoxLayout(&window))->addWidget(stack);
    auto *page0 = new QWidget, *page1 = new QWidget;
    QComboBox *shown = combo(), *hidden = combo();
    (new QVBoxLayout(page0))->addWidget(shown);
    (new QVBoxLayout(page1))->addWidget(hidden);
    stack->addWidget(page0);
    stack->addWidget(page1);
    window.show();
    QCoreApplication::processEvents();
    const QSize plain = hidden->sizeHint();

    app.setStyleSheet(QStringLiteral("QComboBox { padding: 9px 40px; font-size: 15pt; }"));
    QCoreApplication::processEvents();
    CHECK(!hasPopup(hidden));                    // the point of it: no popup for a combo nobody has seen
    CHECK(hasPopup(shown));                      // a visible combo is handled by Qt as before
    CHECK(shown->sizeHint() != plain);           // the sheet does change the size (the test can fail)

    // Shown: the held-back events arrive first, so it matches the combo that was visible all along.
    stack->setCurrentWidget(page1);
    QCoreApplication::processEvents();
    CHECK(hasPopup(hidden));
    CHECK(hidden->sizeHint() == shown->sizeHint());
    CHECK(hidden->minimumSizeHint() == shown->minimumSizeHint());
    CHECK(hidden->font().pointSize() == shown->font().pointSize());
    CHECK(hidden->size() == shown->size());

    // Once shown, later changes go straight through, whether or not it is on screen now.
    stack->setCurrentWidget(page0);
    app.setStyleSheet(QStringLiteral("QComboBox { padding: 2px 4px; font-size: 8pt; }"));
    QCoreApplication::processEvents();
    CHECK(hidden->sizeHint() == shown->sizeHint());
    CHECK(hidden->font().pointSize() == 8);

    // The popup still opens with every item.
    stack->setCurrentWidget(page1);
    QCoreApplication::processEvents();
    hidden->showPopup();
    QCoreApplication::processEvents();
    CHECK(hidden->view()->isVisible());
    CHECK(hidden->view()->model()->rowCount() == 2);
    hidden->hidePopup();

    // A combo created while a sheet is active, on a page that is never shown, gets no popup either.
    auto *later = combo();
    (new QVBoxLayout(new QWidget(page0)))->addWidget(later);
    later->parentWidget()->hide();
    QCoreApplication::processEvents();
    CHECK(!hasPopup(later));

    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::puts("combopopupdefer: all checks passed");
    return failures ? 1 : 0;
}
