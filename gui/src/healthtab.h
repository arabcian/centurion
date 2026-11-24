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
#include <QStringList>
#include <QJsonArray>
#include <QJsonObject>
#include <QVariantMap>
#include <QWidget>

#include <functional>

class AdvancedHealthPage;
class BackupPage;
class DayBars;
class BootPage;
class CoolingPage;
class CurvePage;
class KlogPage;
class OverviewPage;
class QFrame;
class QLabel;
class QStackedWidget;
class QLineEdit;
class QComboBox;
class QCheckBox;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class QTextBrowser;
class QPushButton;
class QScrollArea;
class QToolButton;

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
    struct NavigationEntry { QString target, label; QStringList keywords; };
    QList<NavigationEntry> navigationEntries() const;
    bool openNavigationTarget(const QString &target) { return navigateTo(target); }
    /// A page added to System tools after construction (the Optimizations tab's tuning pages). `key` is
    /// its untranslated name in the tool catalog (toolcatalog.h): area, search words, badges.
    void addToolPage(QWidget *page, const QString &label, const QString &key);

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
    void loadHistoryDay();
    void explainEvent(const QJsonObject &e);
    /// The event record a row stands for (live rows and saved-history rows alike); empty for none.
    QJsonObject eventOf(const QTreeWidgetItem *item) const;
    /// The explanation pane under the list follows the selected row.
    void showEventDetail();
    /// What can be done about an event on another page now (open dGPU power, reset the GPU curve …); empty for none.
    QList<QPair<QString, std::function<void()>>> eventActions(const QJsonObject &e);
    /// The links above a Health page to the System tools that cover the same ground (toolcatalog::relatedForPage).
    void updateRelated(const QString &pageKey);
    static QString friendlyEvent(const QJsonObject &e);

    /// Mark all seen: the events shown so far stop counting as new (gui.ini, see seenMs_).
    void markSeen();
    /// Select `page` in its group (or in the top level), so it shows wherever it sits. A System tool opens
    /// in the tool view. Null does nothing.
    void showPage(QWidget *page);

    // System tools: one catalog page (search, pinned, recent, tools by area) and one tool view with a path
    // back; no second navigation tree.
    struct Tool { QWidget *page = nullptr; QString key, label; };
    void addTool(QWidget *page, const QString &label, const QString &key);
    void openTool(QWidget *page);
    void showToolCatalog();
    void rebuildToolCatalog();
    QWidget *toolButton(const Tool &t, bool featured = false);
    const Tool *toolOf(QWidget *page) const;
    const Tool *toolByKey(const QString &key) const;
    QStringList toolList(const QString &setting) const;
    void togglePin();

    // "page:<key>" / "tool:<key>" targets: back / forward history and links from other pages.
    /// Opens a target; false when this system does not have it.
    bool navigateTo(const QString &target);
    /// Translated name of a target, empty when this system does not have it.
    QString targetLabel(const QString &target) const;
    QWidget *pageByKey(const QString &key) const;
    void record(const QString &target);
    void stepHistory(int delta);
    /// The rail's search: Health pages by their search words, and matching System tools in their own group.
    void filterNavigation(const QString &query);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:

    /// The newest row of one event that repeats: same boot, kind, code and title, within GROUP_US.
    struct Group { QTreeWidgetItem *row = nullptr; QString base; double firstUs = 0, lastUs = 0; int n = 0; };

    QLabel *summary_, *source_, *aer_, *logInfo_, *dayInfo_, *scanWarning_ = nullptr;
    DayBars *days_ = nullptr;
    QString dayFilter_;          // the day the list shows (empty: all)
    quint64 savedSeq_ = 0;      // highest seq of this boot already in the log file
    QString savedBoot_;         // boot_id savedSeq_ belongs to
    QTreeWidget *list_;
    QTextBrowser *eventDetail_ = nullptr;
    QWidget *eventActions_ = nullptr;   // buttons under the explanation of the selected event
    QWidget *relatedBar_ = nullptr;     // "Related:" links above the current page
    QTreeWidget *navigation_ = nullptr;
    QStackedWidget *pages_ = nullptr;
    QHash<QWidget *, QTreeWidgetItem *> pageItems_;
    QWidget *toolDirectory_ = nullptr;
    QStackedWidget *toolMode_ = nullptr;     // 0: catalog, 1: one tool
    QStackedWidget *toolPages_ = nullptr;
    QList<Tool> tools_;
    QLineEdit *toolSearch_ = nullptr;
    QLabel *toolTerms_ = nullptr, *toolPath_ = nullptr, *toolBadges_ = nullptr;
    QPushButton *toolPin_ = nullptr;
    QScrollArea *toolScroll_ = nullptr;
    QWidget *toolCatalog_ = nullptr;
    int toolColumns_ = 0;
    int toolFeaturedColumns_ = 0;
    int toolSymptomColumns_ = 0;
    QString toolFilter_ = QStringLiteral("all");  // all, safe, changes, recent, pinned
    QString toolAreaFilter_;  // empty: every area; an area name otherwise
    QTreeWidgetItem *toolResults_ = nullptr; // the rail's "System tools" search results
    QLineEdit *navSearch_ = nullptr;
    QToolButton *back_ = nullptr, *forward_ = nullptr;
    QStringList history_;
    int historyPos_ = -1;
    bool replaying_ = false;
    AdvancedHealthPage *investigations_ = nullptr;
    QLineEdit *eventSearch_ = nullptr;
    QComboBox *severityFilter_ = nullptr;
    QCheckBox *technicalEvents_ = nullptr;
    QList<QTreeWidgetItem *> historyRows_;
    QTimer *timer_, *debounce_;
    int kmsgFd_ = -1;
    quint64 lastSeq_ = 0;
    bool first_ = true, needsRoot_ = false, busy_ = false;
    int xid_ = 0, gsp_ = 0, mce_ = 0, aerN_ = 0, lockup_ = 0, other_ = 0;
    int nvme_ = 0, fs_ = 0, igpu_ = 0, wifi_ = 0, thermal_ = 0, oom_ = 0, oops_ = 0;
    QString lastRootXid_;  // latest earlier Xid, evidence only (never a proven cause)
    QString lastRootXidPci_, lastRootXidBoot_;
    double lastRootXidUs_ = 0;
    QHash<QString, Group> groups_;
    qint64 seenMs_ = 0;    // Mark all seen: events up to this wall-clock time (ms since epoch) are not new; 0 = never
    BackupPage *backup_ = nullptr;
    BootPage *boot_ = nullptr;
    KlogPage *klog_ = nullptr;
    CurvePage *curves_ = nullptr;
    CoolingPage *cooling_ = nullptr;
    OverviewPage *overview_ = nullptr;
    QFrame *band_ = nullptr;     // "the previous session ended unexpectedly"
    QLabel *bandText_ = nullptr;
    QString bandBoot_;           // boot the band is about (what Dismiss remembers)
};
