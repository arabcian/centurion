#pragma once
// Desktop notifications through org.freedesktop.Notifications (KDE, GNOME, most panels), with
// action buttons. One notification per topic: a new message on the same topic replaces the
// previous one (replaces_id) instead of stacking. When the service is missing or the call fails,
// `fallback` runs (the tray's plain showMessage), so nothing is lost on a bare WM.
//
// Actions only do their work; they never raise the window (the "Details" action does that itself).
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <functional>

struct NotifyAction {
    QString label;
    std::function<void()> run;
};

class Notifier : public QObject {
    Q_OBJECT
public:
    explicit Notifier(QObject *parent = nullptr);
    void notify(const QString &topic, const QString &title, const QString &body, const QList<NotifyAction> &actions,
                int timeoutMs, std::function<void()> fallback);

private Q_SLOTS:
    void onActionInvoked(uint id, const QString &key);
    void onClosed(uint id, uint reason);

private:
    QHash<QString, uint> topicId_;                // topic → id of its live notification
    QHash<uint, QList<NotifyAction>> actions_;    // id → its buttons
};
