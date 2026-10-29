#pragma once
// Health → Boot history: every boot Centurion has seen — how it ended (clean / unclean / running),
// its kernel events, the CPU / GPU curve states in effect — plus the pstore records a crash left
// behind and the last saved kernel-log lines of an unclean boot.
//
// All the assembling is done by `health-reader {"op":"boots"}` (crates/centurion-helpers/src/boothist.rs,
// read-only, silent polkit action); this page only shows it. One call when the page is first shown,
// on Refresh, and once at start-up for HealthTab's "previous session ended unexpectedly" band —
// no periodic process spawns. *Archive and clear pstore…* runs `tune-helper {"op":"pstore_clear"}`
// after a confirmation.
#include <QElapsedTimer>
#include <QJsonObject>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;

class BootPage : public QWidget {
    Q_OBJECT
public:
    explicit BootPage(QWidget *parent = nullptr);
    void reload();

Q_SIGNALS:
    /// After every load: the boot before this one ended unexpectedly (empty id: it did not).
    void previousUnclean(const QString &bootId, qint64 endTime);

protected:
    void showEvent(QShowEvent *e) override;

private:
    void apply(const QJsonObject &r);
    void showDetail();
    void clearPstore();
    static QString logPath();
    static QStringList kernelLogTail(const QString &boot8, int lines);

    QLabel *info_;
    QTreeWidget *list_;
    QPlainTextEdit *detail_;
    QPushButton *bClear_;
    QJsonObject data_;
    QElapsedTimer age_;
    bool busy_ = false;
};
