#pragma once
// Ryzen Curve Optimizer search, all-core or per-core, at three test levels (costrategy.h decides what to
// try; centurion-cpustress --load co tests it in 60 s cycles). Every candidate is temporary: the curve goes
// back to stock when the search ends, fails or is cancelled, and the result is saved as a Ryzen Curve
// profile, never applied. The search can run under a scene of its own (the power level the CPU will
// really be used at). A journal written before every trial lets a search
// that froze or rebooted the machine resume afterwards, the trial in flight counting as failed.
#include "costrategy.h"

#include <QDialog>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <functional>
#include <optional>

class QCloseEvent;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;

class CoSearchDialog : public QDialog {
    Q_OBJECT
public:
    explicit CoSearchDialog(QWidget *parent = nullptr);
    ~CoSearchDialog() override;
protected:
    void closeEvent(QCloseEvent *event) override;
    void reject() override;
private:
    void start();
    void offerResume();
    void launch();
    void step();
    void runTest();
    void testOutput();
    void testFinished(int code, bool crashed);
    void validateFaultLog(cosearch::Outcome o);
    void finishTrial(const cosearch::Outcome &o);
    void scanKernel(std::function<void(bool, const QJsonArray &, quint64)> then);
    void cancel();
    void reset(const QString &reason);
    void done(bool resetOk, const QString &reason);
    void note(const QString &line);
    bool toolsPresent();
    void updateEstimate();
    void loadSettings();
    void saveSettings() const;
    bool writeJournal() const;
    void refreshTable();
    QString saveProfile() const;

    QComboBox *mode_, *level_, *scene_;
    QSpinBox *limit_;
    QLabel *estimate_, *status_;
    QTableWidget *table_;
    QPlainTextEdit *log_;
    QPushButton *go_, *stop_;
    QProcess *test_ = nullptr;
    QTimer *guard_;
    QString testProgram_;

    std::optional<cosearch::Search> search_;
    std::optional<cosearch::Trial> trial_;
    int testIndex_ = 0;
    cosearch::Outcome outcome_;
    QByteArray testBuf_;
    QJsonObject testEnd_;
    QString expectedCurve_;              // CurvePage::currentState("cpu") while the trial's offsets are applied
    int trialNo_ = 0;
    int hardReboots_ = 0;              // max two suspected reboots per search session

    bool running_ = false, abort_ = false, applying_ = false, resetting_ = false, finished_ = false;
    bool sceneApplying_ = false, quitting_ = false;
    qint64 testStart_ = 0, testDeadline_ = 0;
    quint64 kmsgSeq_ = 0;
    QHash<QString, QString> mceBefore_;
    QString startScene_, priorScene_, bootId_;
    QJsonArray attempts_;
};
