#pragma once
// Health → Kernel log: every kernel message of level warning or worse, saved
// across boots. Source: `tune-helper {"op":"klog","since":N}` (reads /dev/kmsg).
//
// Capture is event-driven: while the app runs, /dev/kmsg is watched with a
// socket notifier and the helper is started only after a warning-or-worse
// record appeared (debounced) — no periodic process spawns. If the kernel log
// is restricted (dmesg_restrict=1) it is read through pkexec only when the
// page is shown or "Refresh" is pressed, never in the background.
// The log file survives reboots until cleared; the ring buffer does not.
// View options (level, repeats, only new, muted messages) are shown over the saved lines; the
// pure parts live in klogview.h. Muted messages (right-click → Mute this message) are kept in
// gui.ini under klog/muted.
#include <QHash>
#include <QSet>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QTextBrowser;
class QTimer;

class KlogPage : public QWidget {
    Q_OBJECT
public:
    explicit KlogPage(QWidget *parent = nullptr);
    ~KlogPage() override;
    /// Monitor's "show in kernel log": the saved lines around one kernel record (20 before and after,
    /// all boots, the record highlighted). Changing the scope or the filter shows everything again.
    void showAt(const QString &bootId, quint64 seq);

protected:
    void showEvent(QShowEvent *e) override;

private:
    void capture();
    void drainKmsg();
    void onReply(const QJsonObject &r, bool viaRoot);
    void persist(const QJsonArray &records, const QString &bootId);
    void render();
    void updateInfo();
    void contextMenu(const QPoint &pos);
    void setMuted(const QString &key, bool on);
    static QString logPath();

    QComboBox *scope_;
    QComboBox *level_;
    QCheckBox *group_, *onlyNew_, *showMuted_;
    QLineEdit *filter_;
    QTextBrowser *text_;
    QLabel *info_;
    QTimer *debounce_;
    int kmsgFd_ = -1;
    quint64 lastSeq_ = 0, savedSeq_ = 0;
    QString bootId_;
    QString focusBoot_;          // boot tag (8 chars) of the record showAt() was given; empty = off
    quint64 focusSeq_ = 0;
    bool needsRoot_ = false, busy_ = false, shownOnce_ = false;
    QSet<QString> muted_;                 // normalized messages hidden by the user (gui.ini klog/muted)
    QHash<QString, QString> rowKey_;      // text of each rendered row → its message key (for the menu)
    int hiddenMuted_ = 0;                 // rows left out by the mute list in the last render
};
