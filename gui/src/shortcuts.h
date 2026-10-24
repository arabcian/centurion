#pragma once
// Global keyboard shortcuts for scenes through the desktop's
// org.freedesktop.portal.GlobalShortcuts (KDE Plasma 5.27+, GNOME 48+,
// Hyprland, …). Each scene with a "shortcut" gets one portal shortcut, id
// "scene:<name>", with the scene's key as the preferred trigger; pressing it
// applies the scene like the tray menu does. The desktop owns the final key:
// it may show a confirmation dialog when shortcuts are bound, and the user can
// change them in its own shortcut settings — trigger() reports what it chose.
//
// Nothing is sent to the portal while no scene has a shortcut, so a login
// without shortcuts never shows a dialog.
#include <QDBusContext>
#include <QHash>
#include <QKeySequence>
#include <QObject>
#include <QStringList>
#include <QVariantMap>
#include <functional>

/// XDG shortcuts-spec form of the first key combination of `k` ("CTRL+ALT+1", "LOGO+F5"), "" if none:
/// modifiers CTRL / ALT / SHIFT / LOGO, the key as its xkbcommon keysym name.
inline QString portalTrigger(const QKeySequence &k) {
    if (k.isEmpty()) return {};
    const QKeyCombination c = k[0];
    QStringList parts;
    const Qt::KeyboardModifiers m = c.keyboardModifiers();
    if (m & Qt::ControlModifier) parts << QStringLiteral("CTRL");
    if (m & Qt::AltModifier) parts << QStringLiteral("ALT");
    if (m & Qt::ShiftModifier) parts << QStringLiteral("SHIFT");
    if (m & Qt::MetaModifier) parts << QStringLiteral("LOGO");
    // Qt's portable names that differ from the keysym names; single letters are lower-case keysyms.
    static const QHash<QString, QString> keysym{
        {"Esc", "Escape"}, {"Del", "Delete"}, {"Ins", "Insert"}, {"PgUp", "Prior"}, {"PgDown", "Next"},
        {"Backspace", "BackSpace"}, {"Space", "space"}, {",", "comma"}, {".", "period"}, {"/", "slash"},
        {";", "semicolon"}, {"'", "apostrophe"}, {"[", "bracketleft"}, {"]", "bracketright"}, {"-", "minus"},
        {"=", "equal"}, {"`", "grave"}, {"\\", "backslash"}};
    const QString key = QKeySequence(c.key()).toString(QKeySequence::PortableText);
    parts << keysym.value(key, key.size() == 1 ? key.toLower() : key);
    return parts.join('+');
}

class SceneEngine;
class QDBusMessage;

class GlobalShortcuts : public QObject, protected QDBusContext {
    Q_OBJECT
public:
    explicit GlobalShortcuts(SceneEngine *eng, QObject *parent = nullptr);

    /// Binds the shortcuts of the saved scenes again when they changed (after save, delete, import).
    void sync();
    /// false once the portal turned out to be missing (no xdg-desktop-portal, or no GlobalShortcuts in it).
    bool available() const { return state_ != State::Unavailable; }
    /// The desktop's description of the key bound to `scene` ("" = none / not known yet).
    QString trigger(const QString &scene) const { return triggers_.value(idFor(scene)); }
    /// The portal can open the desktop's own dialog for our shortcuts (GlobalShortcuts version ≥ 2,
    /// once a session exists, i.e. some scene has a shortcut).
    bool canConfigure() const { return state_ == State::Ready && version_ >= 2; }
    /// Opens that dialog (ConfigureShortcuts); a changed key comes back through ShortcutsChanged.
    void configure();

    static QString idFor(const QString &scene) { return QStringLiteral("scene:") + scene; }

Q_SIGNALS:
    /// Availability or the bound triggers changed.
    void changed();

private Q_SLOTS:
    void onResponse(uint code, const QVariantMap &results);
    void onActivated(const QDBusMessage &m);
    void onShortcutsChanged(const QDBusMessage &m);

private:
    enum class State { Idle, Starting, Ready, Unavailable };
    using Reply = std::function<void(uint code, const QVariantMap &results)>;
    void start();
    void request(const QString &method, QVariantList args, QVariantMap options, Reply then);
    void list();
    void bindIfNeeded();
    void storeTriggers(const QVariant &shortcuts);
    void readVersion();

    SceneEngine *eng_;
    State state_ = State::Idle;
    QString session_;
    QHash<QString, Reply> pending_;      // request object path → continuation
    QHash<QString, QString> triggers_;   // shortcut id → trigger description, as the portal reports it
    QHash<QString, QString> asked_;      // shortcut id → preferred trigger last bound by us
    bool listed_ = false;
    int serial_ = 0;
    uint version_ = 0;                   // portal interface version (property "version")
};
