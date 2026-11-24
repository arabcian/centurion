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
// The Wi-Fi tab (connection, adapter hardware, nearby networks) appears only on a machine with a
// wireless interface and is read by the same unprivileged helper: the link every 3 s while the tab is
// shown, the adapter and the nearby networks when their page is opened or refreshed.
// Traffic (bytes per program per day) is the netguard ledger file, read directly like the connection log.
// Firewall (default policies, port rules) and Lookup (whois, DNS, port check, ping) are their own widgets.
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

class FirewallTab;
class LookupTab;
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
    void refreshWifiHw();
    void applyWifiHw(const QJsonObject &h);
    void refreshWifiNets();
    void applyWifiNets(const QJsonObject &n);
    void rescanWifi();
    /// A Wi-Fi request for the adapter picked in the list (empty: the helper takes the first).
    QJsonObject wifiReq(const char *op) const;
    void subscriptions();
    void fillTraffic();
    /// Open ports: listening sockets from the last connection snapshot, with who can reach them.
    void showPorts();
    /// Open ports → open / lan / closed / default: one inbound port in the firewall.
    void setPortState(const QString &state);
    void inspectFirewall();
    /// Writes a list's visible rows (headers first) to a CSV file the user picks.
    void exportTree(QTreeWidget *tree, const QString &name);
    /// Whitelist and blacklist as one text file, and back (Import adds; nothing is removed).
    void exportRules();
    void importRules();
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
    class QComboBox *wifiIface_ = nullptr;    // adapter picker, shown only when there are several
    QLabel *wifiIfaceLabel_ = nullptr, *wifiHw_ = nullptr, *wifiNetInfo_ = nullptr;
    QTabWidget *wifiPages_ = nullptr;         // Connection / Hardware / Nearby networks
    QTreeWidget *wifiNets_ = nullptr;
    quint64 wifiScanRevision_ = 0;
    QJsonObject wifiScanSnapshot_;
    QDateTime wifiScanReceived_;
    class QPushButton *wifiRescan_ = nullptr;
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
    class QComboBox *trafficMode_ = nullptr; // per day / totals per program
    QTreeWidget *ports_ = nullptr;
    QLabel *portsInfo_ = nullptr;
    int portsTab_ = -1;
    FirewallTab *firewall_ = nullptr;
    LookupTab *lookup_ = nullptr;
    int firewallTab_ = -1;
    TrafficDays trafficDays_;                 // Traffic ledger, reread when the file changed
    QDateTime trafficSeen_;
    bool trafficReadable_ = false, trafficValid_ = false;
    QListWidget *wl_;
    QStringList blacklist_;
    QTimer *timer_;
    QFileSystemWatcher *watch_;
    QJsonArray connData_;
    QDateTime connSnapshotTime_;
    QString connSnapshotError_;
    QJsonObject state_;                       // last full status (kernel checks included)
    QHash<QString, QTreeWidgetItem *> rows_;  // block log: program|proto|dst|port → row
    QHash<QString, QJsonObject> whois_;       // address → registry answer, for this session
    qint64 logPos_ = 0, connPos_ = 0;
    QElapsedTimer lastAlert_;
    bool busy_ = false, viaRoot_ = false;
};
