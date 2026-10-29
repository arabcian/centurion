#pragma once
// System tray icon + menu — port of tray.py.
#include "notify.h"
#include <QStringList>
#include <QSystemTrayIcon>

class MainWindow;
class QMenu;

QIcon appIcon(int size = 64);

class Tray : public QSystemTrayIcon {
    Q_OBJECT
public:
    explicit Tray(MainWindow *win);
    void toggleWindow();

private:
    void rebuild();
    bool claimCooldown();
    void notify(const QString &title, const QString &msg, const QList<NotifyAction> &actions = {});
    /// notify() through the Health → Notifications settings (notifyprefs): hidden, held while a game runs, or shown.
    void gatedNotify(const QString &category, const QString &severity, const QString &title, const QString &msg,
                     const QList<NotifyAction> &actions = {});
    /// Sends the alerts held during a game as one summary, once no game session is left.
    void flushDeferred();
    /// Health → Cooling: one notification when the newest clean cooling test is overdue (at most once a week).
    void coolingReminder();

    MainWindow *win_;
    QMenu *menu_;
    Notifier *notifier_;
    bool cooldown_ = false;
    QStringList deferred_;  // one line per alert held while a game ran
};
