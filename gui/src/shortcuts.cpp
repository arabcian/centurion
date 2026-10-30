#include "shortcuts.h"
#include "scenes.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <cstdio>

static const QString SERVICE = QStringLiteral("org.freedesktop.portal.Desktop");
static const QString OBJECT = QStringLiteral("/org/freedesktop/portal/desktop");
static const QString IFACE = QStringLiteral("org.freedesktop.portal.GlobalShortcuts");
static const QString REQUEST = QStringLiteral("org.freedesktop.portal.Request");
static const char *RESPONSE_SLOT = SLOT(onResponse(uint,QVariantMap));

/// One entry of the portal's a(sa{sv}) shortcut lists.
struct PortalShortcut {
    QString id;
    QVariantMap props;
};
using PortalShortcuts = QList<PortalShortcut>;
Q_DECLARE_METATYPE(PortalShortcut)

QDBusArgument &operator<<(QDBusArgument &a, const PortalShortcut &s) {
    a.beginStructure();
    a << s.id << s.props;
    a.endStructure();
    return a;
}
const QDBusArgument &operator>>(const QDBusArgument &a, PortalShortcut &s) {
    a.beginStructure();
    a >> s.id >> s.props;
    a.endStructure();
    return a;
}

/// Shortcut id → preferred trigger for every saved scene that has a shortcut.
static QHash<QString, QString> wanted() {
    QHash<QString, QString> out;
    for (const QString &n : scenes::names())
        if (const auto s = scenes::load(n); s && !s->shortcut.isEmpty())
            out.insert(GlobalShortcuts::idFor(n), portalTrigger(QKeySequence::fromString(s->shortcut, QKeySequence::PortableText)));
    return out;
}

GlobalShortcuts::GlobalShortcuts(SceneEngine *eng, QObject *parent) : QObject(parent), eng_(eng) {
    qDBusRegisterMetaType<PortalShortcut>();
    qDBusRegisterMetaType<PortalShortcuts>();
}

void GlobalShortcuts::sync() {
    switch (state_) {
    case State::Idle: if (!wanted().isEmpty()) start(); break;
    case State::Ready: list(); break;
    case State::Starting: break;  // lists and binds as soon as the session is up
    case State::Unavailable: break;
    }
}

void GlobalShortcuts::start() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) { state_ = State::Unavailable; Q_EMIT changed(); return; }
    state_ = State::Starting;
    // A host (non-Flatpak) app tells the portal its id first (xdg-desktop-portal 1.19+); older portals
    // have no registry and take the id from the desktop's app scope — the error is ignored.
    QDBusMessage reg = QDBusMessage::createMethodCall(SERVICE, OBJECT, QStringLiteral("org.freedesktop.host.portal.Registry"),
                                                      QStringLiteral("Register"));
    reg << QStringLiteral("centurion") << QVariantMap();
    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(reg), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        QDBusConnection bus = QDBusConnection::sessionBus();
        bus.connect(SERVICE, OBJECT, IFACE, QStringLiteral("Activated"), this, SLOT(onActivated(QDBusMessage)));
        bus.connect(SERVICE, OBJECT, IFACE, QStringLiteral("ShortcutsChanged"), this, SLOT(onShortcutsChanged(QDBusMessage)));
        request(QStringLiteral("CreateSession"), {}, {{QStringLiteral("session_handle_token"), QStringLiteral("centurion%1").arg(++serial_)}},
                [this](uint code, const QVariantMap &r) {
                    const QVariant h = r.value(QStringLiteral("session_handle"));
                    session_ = h.userType() == qMetaTypeId<QDBusObjectPath>() ? h.value<QDBusObjectPath>().path() : h.toString();
                    if (code != 0 || session_.isEmpty()) { state_ = State::Unavailable; Q_EMIT changed(); return; }
                    state_ = State::Ready;
                    list();
                    readVersion();
                });
    });
}

/// Portal request: the answer comes as a Response signal on a request object whose path is known in
/// advance, so the signal is connected before the call (no race with a fast portal).
void GlobalShortcuts::request(const QString &method, QVariantList args, QVariantMap options, Reply then) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    const QString token = QStringLiteral("centurion%1").arg(++serial_);
    options.insert(QStringLiteral("handle_token"), token);
    args << options;
    QString sender = bus.baseService().mid(1);
    sender.replace('.', '_');
    const QString path = OBJECT + QStringLiteral("/request/") + sender + '/' + token;
    pending_.insert(path, then);
    bus.connect(SERVICE, path, REQUEST, QStringLiteral("Response"), this, RESPONSE_SLOT);
    QDBusMessage m = QDBusMessage::createMethodCall(SERVICE, OBJECT, IFACE, method);
    m.setArguments(args);
    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(m), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, path, method](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (!w->isError()) return;
        std::fprintf(stderr, "centurion: global shortcuts: %s: %s\n", qPrintable(method), qPrintable(w->error().message()));
        QDBusConnection::sessionBus().disconnect(SERVICE, path, REQUEST, QStringLiteral("Response"), this, RESPONSE_SLOT);
        if (const Reply r = pending_.take(path)) r(~0u, {});
    });
}

void GlobalShortcuts::onResponse(uint code, const QVariantMap &results) {
    const QString path = message().path();
    QDBusConnection::sessionBus().disconnect(SERVICE, path, REQUEST, QStringLiteral("Response"), this, RESPONSE_SLOT);
    if (const Reply r = pending_.take(path)) r(code, results);
}

void GlobalShortcuts::list() {
    request(QStringLiteral("ListShortcuts"), {QVariant::fromValue(QDBusObjectPath(session_))}, {}, [this](uint code, const QVariantMap &r) {
        if (code != 0) return;
        storeTriggers(r.value(QStringLiteral("shortcuts")));
        bindIfNeeded();
    });
}

/// The portal's shortcut list for `set` (shortcut id → preferred trigger).
static PortalShortcuts portalShortcuts(const QHash<QString, QString> &set) {
    PortalShortcuts out;
    for (auto it = set.cbegin(); it != set.cend(); ++it) {
        QVariantMap p{{QStringLiteral("description"), QStringLiteral("Apply scene ") + it.key().mid(6)}};
        if (!it.value().isEmpty()) p.insert(QStringLiteral("preferred_trigger"), it.value());
        out << PortalShortcut{it.key(), p};
    }
    return out;
}

void GlobalShortcuts::bindIfNeeded() {
    const QHash<QString, QString> want = wanted();
    if (!listed_) {
        // Bound in an earlier run: the desktop keeps them (and whatever key the user gave them there).
        listed_ = true;
        for (auto it = want.cbegin(); it != want.cend(); ++it)
            if (triggers_.contains(it.key())) asked_.insert(it.key(), it.value());
    }
    // The desktop keeps the key of a shortcut it already has and ignores preferred_trigger for it, so an edited
    // key is bound again from scratch: the list without it first (the desktop forgets it), then with it.
    QSet<QString> changed;
    for (auto it = want.cbegin(); it != want.cend(); ++it)
        if (triggers_.contains(it.key()) && asked_.contains(it.key()) && asked_.value(it.key()) != it.value()) changed.insert(it.key());
    if (!changed.isEmpty()) {
        QHash<QString, QString> rest = want;
        for (const QString &id : std::as_const(changed)) { rest.remove(id); asked_.remove(id); }
        request(QStringLiteral("BindShortcuts"), {QVariant::fromValue(QDBusObjectPath(session_)), QVariant::fromValue(portalShortcuts(rest)), QString()}, {},
                [this](uint code, const QVariantMap &r) {
                    if (code == 0) storeTriggers(r.value(QStringLiteral("shortcuts")));
                    else asked_.clear();
                    bindIfNeeded();  // binds the changed shortcuts afresh
                });
        return;
    }
    if (QSet<QString>(want.keyBegin(), want.keyEnd()) == QSet<QString>(triggers_.keyBegin(), triggers_.keyEnd()) && asked_ == want) return;
    asked_ = want;
    request(QStringLiteral("BindShortcuts"), {QVariant::fromValue(QDBusObjectPath(session_)), QVariant::fromValue(portalShortcuts(want)), QString()}, {},
            [this](uint code, const QVariantMap &r) {
                if (code == 0) storeTriggers(r.value(QStringLiteral("shortcuts")));
                else asked_.clear();  // dismissed: asked again at the next save
            });
}

void GlobalShortcuts::storeTriggers(const QVariant &shortcuts) {
    triggers_.clear();
    for (const PortalShortcut &s : qdbus_cast<PortalShortcuts>(shortcuts))
        triggers_.insert(s.id, s.props.value(QStringLiteral("trigger_description")).toString());
    Q_EMIT changed();
}

void GlobalShortcuts::readVersion() {
    QDBusMessage m = QDBusMessage::createMethodCall(SERVICE, OBJECT, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    m << IFACE << QStringLiteral("version");
    auto *w = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(m), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QDBusVariant> r = *w;
        if (r.isError()) return;
        version_ = r.value().variant().toUInt();
        Q_EMIT changed();
    });
}

void GlobalShortcuts::configure() {
    if (!canConfigure()) return;
    QDBusMessage m = QDBusMessage::createMethodCall(SERVICE, OBJECT, IFACE, QStringLiteral("ConfigureShortcuts"));
    m << QVariant::fromValue(QDBusObjectPath(session_)) << QString() << QVariantMap();
    auto *w = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(m), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (w->isError()) std::fprintf(stderr, "centurion: global shortcuts: ConfigureShortcuts: %s\n", qPrintable(w->error().message()));
    });
}

void GlobalShortcuts::onActivated(const QDBusMessage &m) {
    const QVariantList a = m.arguments();
    if (a.size() < 2 || a.at(0).value<QDBusObjectPath>().path() != session_) return;
    const QString id = a.at(1).toString();
    if (!id.startsWith(QLatin1String("scene:"))) return;
    if (const QString name = id.mid(6); scenes::names().contains(name)) eng_->apply(name);
}

void GlobalShortcuts::onShortcutsChanged(const QDBusMessage &m) {
    const QVariantList a = m.arguments();
    if (a.size() >= 2 && a.at(0).value<QDBusObjectPath>().path() == session_) storeTriggers(a.at(1));
}
