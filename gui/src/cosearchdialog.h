#pragma once
// Opt-in conservative all-core Ryzen CO search. Every candidate is temporary:
// it is tested with centurion-cpustress then reset to stock on completion/failure.
#include <QDialog>
#include <QJsonObject>
#include <QJsonArray>
class QLabel;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QSpinBox;
class QTimer;
class QCloseEvent;
class CoSearchDialog : public QDialog {
    Q_OBJECT
public:
    explicit CoSearchDialog(QWidget *parent = nullptr);
protected:
    void closeEvent(QCloseEvent *event) override;
    void reject() override;
private:
    void start();
    void next();
    void cpuTest();
    void testFinished(int code);
    void validateFaultLog();
    void cancel();
    void reset(const QString &reason);
    void done(bool resetOk, const QString &reason);
    void note(const QString &line);
    double temperature() const;
    QLabel *status_;
    QPlainTextEdit *log_;
    QPushButton *go_, *stop_;
    QSpinBox *limit_;
    QProcess *test_ = nullptr;
    QTimer *guard_;
    QString testProgram_;
    int candidate_ = 0, best_ = 0;
    bool running_ = false, abort_ = false, applying_ = false, resetting_ = false, exhausted_ = false;
    qint64 testStart_ = 0;
    QString startScene_, bootId_;
    QJsonArray attempts_;
};
