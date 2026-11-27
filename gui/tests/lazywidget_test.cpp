// LazyWidget: a page is built on first show or first content() call, exactly once, and never while hidden.
// Plain asserts (no QtTest dependency); runs on the offscreen platform.
#include "../src/lazywidget.h"

#include <QApplication>
#include <QLabel>
#include <QStackedWidget>
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    // A page that stays behind another one in a stack is never built.
    int builtA = 0, builtB = 0;
    QStackedWidget stack;
    auto *first = new QLabel(QStringLiteral("first"));
    auto *a = new LazyWidget([&] { ++builtA; return new QLabel(QStringLiteral("a")); });
    auto *b = new LazyWidget([&] { ++builtB; return new QLabel(QStringLiteral("b")); });
    stack.addWidget(first);
    stack.addWidget(a);
    stack.addWidget(b);
    stack.show();
    QCoreApplication::processEvents();
    CHECK(builtA == 0 && builtB == 0);
    CHECK(a->findChildren<QLabel *>().isEmpty());

    // Showing it builds it once; showing it again does not build a second copy.
    stack.setCurrentWidget(a);
    QCoreApplication::processEvents();
    CHECK(builtA == 1);
    CHECK(a->findChildren<QLabel *>().size() == 1);
    stack.setCurrentWidget(first);
    stack.setCurrentWidget(a);
    QCoreApplication::processEvents();
    CHECK(builtA == 1);
    CHECK(a->findChildren<QLabel *>().size() == 1);

    // content() builds a page that was never shown, returns the same widget afterwards, and showing it later
    // does not build it again.
    QWidget *content = b->content();
    CHECK(builtB == 1);
    CHECK(content && content->parent() == b);
    CHECK(qobject_cast<QLabel *>(content) && static_cast<QLabel *>(content)->text() == QStringLiteral("b"));
    CHECK(b->content() == content);
    stack.setCurrentWidget(b);
    QCoreApplication::processEvents();
    CHECK(builtB == 1);
    CHECK(content->isVisible());

    // content() after a show returns what the show built.
    auto *c = new LazyWidget([] { return new QLabel(QStringLiteral("c")); });
    stack.addWidget(c);
    stack.setCurrentWidget(c);
    QCoreApplication::processEvents();
    CHECK(c->findChildren<QLabel *>().size() == 1 && c->content() == c->findChild<QLabel *>());

    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::puts("lazywidget: all checks passed");
    return failures ? 1 : 0;
}
