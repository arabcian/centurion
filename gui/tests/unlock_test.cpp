#include "../src/privileged.h"
#include "../src/unlock.h"

#include <QApplication>
#include <QInputDialog>
#include <QTimer>
#include <QWidget>
#include <utility>

namespace {
QString pendingOp;
privileged::Callback pendingReply;

void reply(QJsonObject json) {
    std::exchange(pendingReply, {})({true, std::move(json), {}});
}
}  // namespace

namespace privileged {
// Keep the helper reply pending until the test releases it, with no root calls.
QString helperPath(const QString &name) { return name; }
QString Result::message() const { return error; }
void run(const QString &, const QJsonObject &payload, QObject *, Callback cb, int) {
    pendingOp = payload.value(QStringLiteral("op")).toString();
    pendingReply = std::move(cb);
}
void runUntilExit(const QString &helper, const QJsonObject &payload, QObject *ctx, Callback cb, int timeout,
                  std::function<void()>, std::uint64_t) {
    run(helper, payload, ctx, std::move(cb), timeout);
}
}  // namespace privileged

int main(int argc, char **argv) {
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
    return manager.unlocked() || pendingReply ? 8 : 0;
}
