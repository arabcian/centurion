#pragma once
// Warm in the bag: the lid is closed, the machine is still awake, and nothing but the lid is a display, for
// longer than the limit on the Sleep page (sleep/bag_warn_min in gui.ini, minutes, 0 = off, default 10).
// Then one notification with the CPU temperature and the fastest fan, so a laptop that is cooking in a bag
// gets noticed. Event-driven: UPower's LidIsClosed property signal starts one single-shot timer, and the lid
// opening stops it. Nothing is polled; the window does not need to be open.
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class QTimer;

class LidWatch : public QObject {
    Q_OBJECT
public:
    explicit LidWatch(QObject *parent);

Q_SIGNALS:
    /// The lid has been shut for too long while the machine stayed awake; `text` names the temperatures.
    void warm(const QString &text);

private Q_SLOTS:
    void onPropertiesChanged(const QString &iface, const QVariantMap &changed, const QStringList &invalidated);

private:
    void setClosed(bool closed);
    void fire();

    QTimer *timer_;
    bool closed_ = false;
    int minutes_ = 0;  // the limit read when the lid closed
};
