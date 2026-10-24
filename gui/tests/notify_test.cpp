// Notifier (notify.h) against a fake org.freedesktop.Notifications on a private session bus.
// Run under `dbus-run-session --config-file=tests/dbus-session.conf` (ctest does): that bus has no
// activatable services, so the "no service" case cannot start a real notification daemon.
#include "../src/notify.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QElapsedTimer>
#include <QVariantMap>
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

struct Call { QString app; uint replaces; QString icon, summary, body; QStringList actions; QVariantMap hints; int timeout; };

class FakeServer : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public:
    QList<Call> calls;
    uint next = 41;
public Q_SLOTS:
    uint Notify(const QString &app, uint replaces, const QString &icon, const QString &summary, const QString &body,
                const QStringList &actions, const QVariantMap &hints, int timeout) {
        calls.append({app, replaces, icon, summary, body, actions, hints, timeout});
        return replaces ? replaces : ++next;
    }
};

static bool waitFor(const std::function<bool()> &cond) {
    QElapsedTimer t;
    t.start();
    while (!cond() && t.elapsed() < 3000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return cond();
}

/// Lets replies and signals already on the wire reach the client.
static void settle() {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 200) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

static void emitSignal(QDBusConnection &c, const char *name, const QVariantList &args) {
    QDBusMessage s = QDBusMessage::createSignal(QStringLiteral("/org/freedesktop/Notifications"),
                                                QStringLiteral("org.freedesktop.Notifications"), QLatin1String(name));
    s.setArguments(args);
    c.send(s);
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (!QDBusConnection::sessionBus().isConnected()) { std::fputs("notify: no session bus (run under dbus-run-session)\n", stderr); return 1; }
    Notifier n;

    // 1. No service on the bus → the fallback runs.
    bool fellBack = false;
    n.notify("t", "Title", "Body", {}, 2500, [&] { fellBack = true; });
    CHECK(waitFor([&] { return fellBack; }));

    // The fake server on its own connection, so calls really travel through the bus.
    QDBusConnection srv = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("fake-notifications"));
    FakeServer fake;
    CHECK(srv.registerObject(QStringLiteral("/org/freedesktop/Notifications"), &fake, QDBusConnection::ExportAllSlots));
    CHECK(srv.registerService(QStringLiteral("org.freedesktop.Notifications")));

    // 2. Arguments of a plain notification.
    fellBack = false;
    n.notify("Fans", "Fans", "All fans to maximum…", {}, 2500, [&] { fellBack = true; });
    CHECK(waitFor([&] { return fake.calls.size() == 1; }));
    if (fake.calls.size() == 1) {
        const Call &c = fake.calls[0];
        CHECK(c.app == QLatin1String("Centurion"));
        CHECK(c.replaces == 0);
        CHECK(c.icon == QLatin1String("centurion"));
        CHECK(c.summary == QLatin1String("Fans"));
        CHECK(c.body == QString::fromUtf8("All fans to maximum…"));
        CHECK(c.actions.isEmpty());
        CHECK(c.hints.value("desktop-entry").toString() == QLatin1String("centurion"));
        CHECK(c.timeout == 2500);
    }

    settle();  // the reply (id) must be back before the same topic is used again

    // 3. Same topic again → replaces the live one.
    n.notify("Fans", "Fans", "All fans back to Auto…", {}, 2500, [&] { fellBack = true; });
    CHECK(waitFor([&] { return fake.calls.size() == 2; }));
    if (fake.calls.size() == 2) CHECK(fake.calls[1].replaces == 42);

    // 4. Actions: key/label pairs, server-default timeout; ActionInvoked runs the right one, only for our id.
    int undone = 0, other = 0;
    n.notify("GPU curve guard", "GPU curve guard", "Reset.", {{"Undo", [&] { ++undone; }}, {"Other", [&] { ++other; }}}, 2500,
             [&] { fellBack = true; });
    CHECK(waitFor([&] { return fake.calls.size() == 3; }));
    uint id = 0;
    if (fake.calls.size() == 3) {
        CHECK(fake.calls[2].actions == QStringList({"0", "Undo", "1", "Other"}));
        CHECK(fake.calls[2].timeout == -1);
        CHECK(fake.calls[2].replaces == 0);
        id = fake.next;
    }
    settle();
    emitSignal(srv, "ActionInvoked", {QVariant::fromValue(id + 100), QStringLiteral("0")});  // someone else's
    emitSignal(srv, "ActionInvoked", {QVariant::fromValue(id), QStringLiteral("0")});
    CHECK(waitFor([&] { return undone == 1; }));
    CHECK(other == 0);

    // 5. Closed → the topic starts a new notification, and its old buttons are gone.
    emitSignal(srv, "NotificationClosed", {QVariant::fromValue(id), QVariant::fromValue(2u)});
    settle();
    emitSignal(srv, "ActionInvoked", {QVariant::fromValue(id), QStringLiteral("0")});
    n.notify("GPU curve guard", "GPU curve guard", "Again.", {}, 2500, [&] { fellBack = true; });
    CHECK(waitFor([&] { return fake.calls.size() == 4; }));
    if (fake.calls.size() == 4) CHECK(fake.calls[3].replaces == 0);
    CHECK(undone == 1);
    CHECK(!fellBack);

    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::puts("notify: all checks passed");
    return failures ? 1 : 0;
}

#include "notify_test.moc"
