#pragma once
// The diagnostic report before it is saved: every file of the archive with its status, and the text of
// the file selected. The text is the sanitized text: what is shown is what is written.
#include <QDialog>
#include <QHash>
#include <QJsonObject>
#include <QJsonArray>
#include <QSet>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QPlainTextEdit;

class ReportPreview : public QDialog {
    Q_OBJECT
public:
    /// `preview` is the reply of the helper's diag_report op without "out".
    explicit ReportPreview(const QJsonObject &preview, const QString &snapshotPath, QWidget *parent = nullptr);
    QJsonArray selection() const;

private:
    static QString statusText(const QString &status);
    void selectFile(QListWidgetItem *item);
    void showPage(qint64 page);
    void updatePageControls();
    void markPageShown(QListWidgetItem *item);
    qint64 nextUnopenedPage() const;

    QListWidget *list_;
    QPlainTextEdit *text_;
    QWidget *pager_;
    QPushButton *previousPage_;
    QPushButton *nextPage_;
    QPushButton *jumpPage_;
    QPushButton *unopenedPage_;
    QPushButton *retryPage_;
    QLabel *pageStatus_;
    const QString snapshotPath_;
    const qint64 pageBytes_;
    struct ReviewProgress {
        QSet<qint64> shown;
        qint64 lastPage = 0;
    };
    QHash<const QListWidgetItem *, ReviewProgress> reviews_;
    qint64 page_ = 0;
    qint64 pageCount_ = 1;
    qint64 failedPage_ = -1;
    quint64 generation_ = 0;
    bool loadingPage_ = false;
};
