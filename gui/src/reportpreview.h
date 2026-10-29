#pragma once
// The diagnostic report before it is saved: every file of the archive with its status, and the text of
// the file selected. The text is the sanitized text: what is shown is what is written.
#include <QDialog>
#include <QJsonObject>

class QListWidget;
class QPlainTextEdit;

class ReportPreview : public QDialog {
    Q_OBJECT
public:
    /// `preview` is the reply of the helper's diag_report op without "out".
    explicit ReportPreview(const QJsonObject &preview, QWidget *parent = nullptr);

private:
    static QString statusText(const QString &status);
    QListWidget *list_;
    QPlainTextEdit *text_;
};
