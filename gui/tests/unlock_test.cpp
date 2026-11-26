#include "../src/kernelparamspage.h"
#include "../src/privileged.h"
#include "../src/unlock.h"

#include <QApplication>
#include <QComboBox>
#include <QEventLoop>
#include <QInputDialog>
#include <QJsonArray>
#include <QKeyEvent>
#include <QPushButton>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>
#include <utility>

namespace {
QString pendingOp;
privileged::Callback pendingReply, pendingSnapshot;

void reply(QJsonObject json) {
    std::exchange(pendingReply, {})({true, std::move(json), {}});
}
}  // namespace

namespace theme {
bool experimentalEnabled() { return false; }
}

namespace privileged {
// Keep the helper reply pending until the test releases it, with no root calls.
QString helperPath(const QString &) { return QCoreApplication::applicationFilePath(); }
QString Result::message() const { return error; }
void run(const QString &, const QJsonObject &payload, QObject *, Callback cb, int) {
    const auto op = payload.value(QStringLiteral("op")).toString();
    if (op == QStringLiteral("ksnap")) {
        pendingSnapshot = std::move(cb);
        return;
    }
    pendingOp = op;
    pendingReply = std::move(cb);
}
void runUntilExit(const QString &helper, const QJsonObject &payload, QObject *ctx, Callback cb, int timeout,
                  std::function<void()>, std::uint64_t) {
    run(helper, payload, ctx, std::move(cb), timeout);
}
}  // namespace privileged

int main(int argc, char **argv) {
    QTemporaryDir settings;
    if (!settings.isValid()) return 25;
    qputenv("XDG_CONFIG_HOME", settings.path().toUtf8());
    QApplication app(argc, argv);
    QWidget window;
    window.show();
    auto &manager = unlock::Manager::instance();

    auto enterPin = [&] {
        manager.requestUnlock(&window);
        if (pendingOp != QStringLiteral("pin_status") || !pendingReply) return false;
        QTimer::singleShot(0, &window, [] {
            if (auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
                dialog->setTextValue(QStringLiteral("1234"));
                dialog->accept();
            }
        });
        // Even if the helper claims a full grant exists, an explicit unlock must verify the PIN.
        reply({{"ok", true}, {"pin_set", true}, {"unlocked", true}});
        return pendingOp == QStringLiteral("unlock") && bool(pendingReply);
    };

    if (!enterPin()) return 1;
    reply({{"ok", true}, {"unlocked", true}});
    if (!manager.unlocked()) return 2;
    manager.lock(true);
    if (manager.unlocked() || pendingOp != QStringLiteral("lock") || !pendingReply) return 3;
    reply({{"ok", true}, {"locked", true}, {"scenes_only", true}});
    if (manager.unlocked() || pendingReply) return 4;

    if (!enterPin()) return 5;
    manager.lock(true);
    reply({{"ok", true}, {"unlocked", true}});
    if (manager.unlocked() || pendingOp != QStringLiteral("lock") || !pendingReply) return 6;
    reply({{"ok", true}, {"locked", true}, {"scenes_only", true}});
    if (manager.unlocked() || pendingReply) return 7;

    manager.requestUnlock(&window);
    manager.lock(true);
    reply({{"ok", true}, {"pin_set", true}});
    if (manager.unlocked() || pendingReply) return 8;

    auto *timer = manager.findChild<QTimer *>();
    if (!timer || timer->interval() != 600000 || timer->isActive()) return 9;
    if (!enterPin()) return 10;
    reply({{"ok", true}, {"unlocked", true}});
    if (!manager.unlocked() || !timer->isActive()) return 11;

    timer->setInterval(25);
    const int timerId = timer->timerId();
    QKeyEvent syntheticInput(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
    QApplication::sendEvent(&window, &syntheticInput);
    if (timer->timerId() != timerId) return 12;
    QEventLoop waitForLock;
    QObject::connect(timer, &QTimer::timeout, &waitForLock, &QEventLoop::quit);
    QTimer::singleShot(1000, &waitForLock, &QEventLoop::quit);
    waitForLock.exec();
    if (manager.unlocked() || timer->isActive() || pendingOp != QStringLiteral("lock") || !pendingReply) return 13;
    reply({{"ok", true}, {"locked", true}, {"scenes_only", true}});
    if (manager.unlocked() || pendingReply) return 14;

    timer->setInterval(600000);
    KernelParamsPage page;
    auto *scope = page.findChild<QComboBox *>();
    auto *table = page.findChild<QTableView *>();
    QPushButton *read = nullptr, *save = nullptr;
    for (auto *button : page.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("Read")) read = button;
        if (button->text() == QStringLiteral("Export and Save As…")) save = button;
    }
    if (!scope || !table || !read || !save || scope->isEnabled() || read->isEnabled() || save->isEnabled()) return 15;
    // The read path also checks the lock if triggered directly through scope activation.
    scope->setCurrentIndex(scope->findData(QStringLiteral("/sys")));
    if (!QMetaObject::invokeMethod(scope, "activated", Qt::DirectConnection, Q_ARG(int, scope->currentIndex())) || pendingSnapshot)
        return 16;

    if (!enterPin()) return 17;
    reply({{"ok", true}, {"unlocked", true}});
    if (!read->isEnabled() || !scope->isEnabled()) return 18;
    const auto deliverSnapshot = [] {
        std::exchange(pendingSnapshot, {})({true, {{"ok", true}, {"entries", QJsonArray{
            QJsonObject{{"p", "/sys/test"}, {"v", "value"}, {"w", false}}}}}, {}});
    };
    read->click();
    if (!pendingSnapshot) return 19;
    deliverSnapshot();
    if (table->model()->rowCount() != 1 || !save->isEnabled()) return 20;

    read->click();
    if (!pendingSnapshot) return 21;
    manager.lock(false);
    if (table->model()->rowCount() != 0 || save->isEnabled() || read->isEnabled() || scope->isEnabled()) return 22;
    reply({{"ok", true}, {"locked", true}});
    if (!enterPin()) return 23;
    reply({{"ok", true}, {"unlocked", true}});
    deliverSnapshot();
    if (table->model()->rowCount() != 0 || save->isEnabled() || !scope->isEnabled()) return 24;
    return 0;
}
