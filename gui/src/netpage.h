#pragma once
// Health → Network: live TCP/UDP connections with their processes, an IP
// blacklist, the Wine/.exe guard (a non-whitelisted Windows program that
// opens an internet connection is cut off and logged by the centurion-netguard
// daemon — see crates/centurion-helpers/src/netguard.rs), and the switchable
// connection log (every new outbound and inbound connection, written by the
// same daemon) with an on-demand whois / reverse-DNS lookup of an address.
//
// Cost: the connection list is read by the unprivileged netguard-helper only
// while this page is on screen (every 3 s), and so is the connection log
// (new lines only, while its tab is shown); the block log is watched with
// inotify, so a new block reaches the tray without any polling. Rule changes
// go through pkexec and take effect in the running daemon at once.
// The Wi-Fi tab (signal, link rate, retries) appears only on a machine with a
// wireless interface and is read by the same unprivileged helper, every 3 s while shown.
// Traffic (bytes per program per day) is the netguard ledger file, read directly like the connection log.
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QDateTime>
#include <QJsonObject>
#include <QPointer>
#include <QWidget>
#include <functional>

#include "blocklistsubs.h"
#include "traffic.h"

class QCheckBox;
class QFileSystemWatcher;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidget;
class QMenu;
class QNetworkAccessManager;
class QTabWidget;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

class NetPage : public QWidget {
    Q_OBJECT
public:
    explicit NetPage(QWidget *parent = nullptr);

Q_SIGNALS:
    /// A program was blocked just now (not the history found at start-up).
    void alert(const QString &title, const QString &message);

protected:
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;

private:
    void runUser(const QJsonObject &req, std::function<void(const QJsonObject &)> cb, int timeoutMs = 10000);
    void runRoot(const QJsonObject &req, bool isStatus = true);
    void applyStatus(const QJsonObject &s);
    void refreshConns();
    void showConns();
    void readLog(bool announce);
    void readConnLog();
    void filterConnLog();
    void ipInfo(const QString &ip);
    void connDetails(const QString &ip, int pid, const QString &exe, qint64 uid, const QString &state);
    void whois(const QString &ip, std::function<void(const QJsonObject &)> cb);
    void addIpActions(QMenu &m, const QString &ip);
    void refreshWifi();
    void applyWifi(const QJsonObject &w);
    void subscriptions();
    void fillTraffic();
    bool loadTraffic();
    void fillSubscriptions();
    void updateSubscription(int row);
    void changeSubscription(const QString &name, const std::function<void(BlocklistSub &)> &f);
    void blacklistNetwork(const QString &ip);
    void bulkBlacklist();
    void removeBlacklisted(bool all);
    void watchLog();
    void poll();
    QJsonObject curConn() const;
    void changeList(const char *op, const char *verb, const QString &entry);

    QLabel *status_, *connInfo_;
    QCheckBox *guard_, *lan_, *logConns_, *accounting_, *listening_;
    QTabWidget *tabs_;
    QLabel *wifiText_;                        // Wi-Fi tab: link and station counters
    QTimer *wifiTimer_;
    int wifiTab_ = -1;
    QPointer<QListWidget> subList_;           // open Subscriptions window
    QNetworkAccessManager *nam_ = nullptr;
    QLineEdit *filter_, *logFilter_, *wlEdit_, *blEdit_;
    QLabel *logInfo_;
    QTreeWidget *conns_, *blocked_, *connLog_, *bl_;  // bl_: groups (entries banned together) with their entries
    QTreeWidget *trafficTree_;
    QLabel *trafficInfo_;
    int trafficTab_ = -1;
    TrafficDays trafficDays_;                 // Traffic ledger, reread when the file changed
    QDateTime trafficSeen_;
    bool trafficReadable_ = false;
    QListWidget *wl_;
    QStringList blacklist_;
    QTimer *timer_;
    QFileSystemWatcher *watch_;
    QJsonArray connData_;
    QJsonObject state_;                       // last full status (kernel checks included)
    QHash<QString, QTreeWidgetItem *> rows_;  // block log: program|proto|dst|port → row
    QHash<QString, QJsonObject> whois_;       // address → registry answer, for this session
    qint64 logPos_ = 0, connPos_ = 0;
    QElapsedTimer lastAlert_;
    bool busy_ = false, viaRoot_ = false;
};
