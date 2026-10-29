#pragma once
// Health tab — hardware/driver faults since boot: NVIDIA Xid (with the root
// cause of an Xid 154), GSP timeouts, machine checks, PCIe AER, lockups, and
// disk / file system / iGPU / Wi-Fi / thermal / OOM / kernel-bug events.
//
// Source: `tune-helper {"op":"health","since":N}`, run unprivileged (reads
// /dev/kmsg). Event-driven: the tab keeps /dev/kmsg open with a socket
// notifier and starts the helper only after a kernel line that can matter
// (NVRM, MCE, AER, lockup, amdgpu) — no periodic process spawns. Only when
// the kernel log is restricted (dmesg_restrict=1) it falls back to a slow
// pkexec poll. New critical events reach the tray as a notification.
// Above the list, a 30-day chart of the saved events per day and kind; a click shows that day only.
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QVariantMap>
#include <QWidget>

class BackupPage;
class DayBars;
class BootPage;
class CoolingPage;
class CurvePage;
class KlogPage;
class OverviewPage;
class QFrame;
class QLabel;
class QTabWidget;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

class HealthTab : public QWidget {
    Q_OBJECT
public:
    explicit HealthTab(QWidget *parent = nullptr);
    ~HealthTab() override;
    BackupPage *backup() const { return backup_; }
    /// Worst level of the Overview cards (-1 no data, 0 ok, 1 watch, 2 attention).
    int worstLevel() const;
    /// The Overview cards as last computed (empty until the Overview has run).
    QList<QVariantMap> overviewCards() const;
    /// This session's events with a seq above `seq`, oldest first (a repeated event is one row, under its first seq).
    QList<QVariantMap> eventsSince(quint64 seq) const;
    /// The Cooling page (the main window gives it the Home tab's fan controls).
    CoolingPage *cooling() const { return cooling_; }
    /// Show the dGPU power page (the Health tab must already be on screen).
    void openDgpuPage();

Q_SIGNALS:
    /// The dGPU stayed awake on battery for `minutes` (BatteryPage::dgpuAwake); the tray tells the user.
    void dgpuAwake(qint64 minutes, const QString &holders);
    /// The Overview's worst level changed (Home badge, tray).
    void levelChanged(int worst);
    /// A desktop alert: `category` is a notifyprefs key, `severity` "error" / "critical" for kernel
    /// alerts (empty otherwise). Kernel alerts are new error/critical events (not the start-up history).
    void alert(const QString &category, const QString &severity, const QString &title, const QString &message);
    void curveReset(const QString &group, const QString &profile, const QString &message);  // CurvePage::curveReset
    /// Every new classified kernel event of this session (not the history found at start-up).
    void fault(const QJsonObject &event);
    /// A main-window tab by its English name (MainWindow::showTab), for Monitor's actions.
    void openTab(const QString &label);

private:
    void scan();
    void drainKmsg();
    void onReply(const QJsonObject &r, bool viaRoot);
    void addEvent(const QJsonObject &e, const QString &bootId);
    void eventMenu(const QPoint &pos);
    void showInKlog(const QJsonObject &e);
    void updateSummary();
    void persist(const QJsonArray &ev, const QString &bootId);
    void updateLogInfo();
    static QString logPath();
    /// The 30-day chart from the helper (event_days); again after new events and after a saved-log clear.
    void refreshDays();
    void filterDay(const QString &date);
    void applyDayFilter();

    /// Mark all seen: the events shown so far stop counting as new (gui.ini, see seenMs_).
    void markSeen();
    /// Select `page` in its group (or in the top level), so it shows wherever it sits. Null does nothing.
    void showPage(QWidget *page);

    /// The newest row of one event that repeats: same boot, kind, code and title, within GROUP_US.
    struct Group { QTreeWidgetItem *row = nullptr; QString base; double firstUs = 0, lastUs = 0; int n = 0; };

    QLabel *summary_, *source_, *aer_, *logInfo_, *dayInfo_;
    DayBars *days_ = nullptr;
    QString dayFilter_;          // the day the list shows (empty: all)
    quint64 savedSeq_ = 0;      // highest seq of this boot already in the log file
    QString savedBoot_;         // boot_id savedSeq_ belongs to
    QTreeWidget *list_;
    QTimer *timer_, *debounce_;
    int kmsgFd_ = -1;
    quint64 lastSeq_ = 0;
    bool first_ = true, needsRoot_ = false, busy_ = false;
    int xid_ = 0, gsp_ = 0, mce_ = 0, aerN_ = 0, lockup_ = 0, other_ = 0;
    int nvme_ = 0, fs_ = 0, igpu_ = 0, wifi_ = 0, thermal_ = 0, oom_ = 0, oops_ = 0;
    QString lastRootXid_;  // title of the Xid before an Xid 154
    QHash<QString, Group> groups_;
    qint64 seenMs_ = 0;    // Mark all seen: events up to this wall-clock time (ms since epoch) are not new; 0 = never
    BackupPage *backup_ = nullptr;
    BootPage *boot_ = nullptr;
    QTabWidget *sub_ = nullptr;  // the Health sub-tabs (Monitor's actions switch to them)
    KlogPage *klog_ = nullptr;
    CurvePage *curves_ = nullptr;
    CoolingPage *cooling_ = nullptr;
    OverviewPage *overview_ = nullptr;
    QFrame *band_ = nullptr;     // "the previous session ended unexpectedly"
    QLabel *bandText_ = nullptr;
    QString bandBoot_;           // boot the band is about (what Dismiss remembers)
};
