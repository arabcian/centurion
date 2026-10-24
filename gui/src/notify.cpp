#include "notify.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QVariantMap>

static const QString SERVICE = QStringLiteral("org.freedesktop.Notifications");
static const QString PATH = QStringLiteral("/org/freedesktop/Notifications");

Notifier::Notifier(QObject *parent) : QObject(parent) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(SERVICE, PATH, SERVICE, QStringLiteral("ActionInvoked"), this, SLOT(onActionInvoked(uint, QString)));
    bus.connect(SERVICE, PATH, SERVICE, QStringLiteral("NotificationClosed"), this, SLOT(onClosed(uint, uint)));
}

void Notifier::notify(const QString &topic, const QString &title, const QString &body, const QList<NotifyAction> &actions,
                      int timeoutMs, std::function<void()> fallback) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) { fallback(); return; }
    QStringList keys;
    for (int i = 0; i < actions.size(); ++i) keys << QString::number(i) << actions[i].label;
    QDBusMessage m = QDBusMessage::createMethodCall(SERVICE, PATH, SERVICE, QStringLiteral("Notify"));
    m << QStringLiteral("Centurion") << topicId_.value(topic, 0u) << QStringLiteral("centurion") << title << body << keys
      << QVariantMap{{QStringLiteral("desktop-entry"), QStringLiteral("centurion")}}
      << (actions.isEmpty() ? timeoutMs : -1);  // buttons need time to be clicked: server default
    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(m), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, topic, actions, fallback] {
        w->deleteLater();
        const QDBusPendingReply<uint> r = *w;
        if (r.isError()) { fallback(); return; }
        const uint id = r.value();
        topicId_.insert(topic, id);
        if (actions.isEmpty()) actions_.remove(id);
        else actions_.insert(id, actions);
    });
}

void Notifier::onActionInvoked(uint id, const QString &key) {
    const auto it = actions_.constFind(id);
    if (it == actions_.cend()) return;  // not ours (the signal is broadcast to every client)
    bool ok = false;
    const int i = key.toInt(&ok);
    if (ok && i >= 0 && i < it->size() && (*it)[i].run) (*it)[i].run();
}

void Notifier::onClosed(uint id, uint) {
    actions_.remove(id);
    for (auto it = topicId_.begin(); it != topicId_.end();)
        it = it.value() == id ? topicId_.erase(it) : std::next(it);
}
