#pragma once
// Ryzen Curve Optimizer search, all-core or per-core, at three test levels (costrategy.h decides what to
// try; centurion-cpustress --load co tests it in 60 s cycles). Every candidate is temporary: the curve goes
// back to stock when the search ends, fails or is cancelled, and the result is saved as a Ryzen Curve
// profile, never applied. The search can run under a scene of its own (the power level the CPU will
// really be used at). A journal written before every trial lets a search
// that froze or rebooted the machine resume afterwards, the trial in flight counting as failed.
#include "coevidence.h"
#include "costrategy.h"

#include <QDialog>
#include <QHash>
#include <QSet>
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
    void applyTuning();
    cosearch::Tuning tuning() const;
    void loadSettings();
    void saveSettings() const;
    bool writeJournal() const;
    void refreshTable();
    void reloadPresets();
    void watchTick();
    void gatherEvidence(const QJsonObject &journal, const cosearch::Search &search, const cosearch::Trial &trial,
                        std::function<void(const coevidence::Verdict &)> then);
    void updateResultButtons();
    void saveResultAs();
    void loadResultIntoStarts();
    void applyPreset(const QString &name);
    bool saveProfile(const QString &name) const;

    QComboBox *mode_, *level_, *scene_, *preset_;
    QSpinBox *limit_, *allStart_, *step_;
    QPushButton *applyTuning_;
    QLabel *estimate_, *status_;
    QTableWidget *table_;
    QPlainTextEdit *log_;
    QPushButton *go_, *stop_, *saveResult_, *loadResult_;
    QProcess *test_ = nullptr;
    QTimer *guard_, *watch_;
    QString testProgram_;

    std::optional<cosearch::Search> search_;
    std::optional<cosearch::Trial> trial_;
    int testIndex_ = 0;
    cosearch::Outcome outcome_;
    QByteArray testBuf_;
    QJsonObject testEnd_;
    QString expectedCurve_;              // CurvePage::currentState("cpu") while the trial's offsets are applied
    int trialNo_ = 0;
    int hardReboots_ = 0;              // suspected reboots in this search, shown when resuming

    bool running_ = false, abort_ = false, applying_ = false, resetting_ = false, finished_ = false;
    bool sceneApplying_ = false, quitting_ = false;
    qint64 testStart_ = 0, testDeadline_ = 0;
    quint64 kmsgSeq_ = 0;
    QHash<QString, QString> mceBefore_;
    QString startScene_, priorScene_, bootId_;
    QJsonArray attempts_;
    QSet<int> excluded_;               // per-core: cores the user left out (coreKey(ccd, slot))
    QHash<int, int> starts_;           // per-core: each core's start (coreKey -> offset), typed or from a preset
    // Crash watcher (coevidence.h) of the trial in flight: what it saw, committed to the journal at once.
    QJsonArray evidence_;
    QHash<QString, QString> mceWatch_;
    quint64 watchSeq_ = 0;
    bool watchBusy_ = false;
    int activeCpu_ = -1;               // the CPU centurion-cpustress last moved its single-core load to
};
