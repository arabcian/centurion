#pragma once
// Ryzen Curve Optimizer search, all-core or per-core, at three test levels (costrategy.h decides what to
// try; centurion-cpustress --load co tests it in 60 s cycles). Every candidate is temporary: the curve goes
// back to stock when the search ends, fails or is cancelled, and the result is saved as a Ryzen Curve
// profile, never applied. The search can run under a scene of its own (the power level the CPU will
// really be used at). A journal written before every trial lets a search
// that froze or rebooted the machine resume afterwards, the trial in flight counting as failed.
#include "coevidence.h"
#include "costrategy.h"
#include "ryzentab.h"

#include <QDialog>
#include <QElapsedTimer>
#include <QPointer>
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
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QGridLayout;
class QTimer;
class Notifier;

class CoSearchDialog : public QDialog {
    Q_OBJECT
public:
    explicit CoSearchDialog(QWidget *parent = nullptr);
    ~CoSearchDialog() override;
    bool operationActive() const { return running_ || applying_ || resetting_ || sceneApplying_ || preparing_ || recoveryBusy_ || !perfTest_.isNull(); }
protected:
    void closeEvent(QCloseEvent *event) override;
    void reject() override;
private:
    void start();
    void startPrepared();
    void verifyPrepared();
    /// Checks shared by start() and verifyProfile(); the detected layout when a search may begin.
    std::optional<ryzen::Layout> preflight(cosearch::Mode mode);
    /// Runs only the final verification on a saved Ryzen Curve profile (Search::startVerification).
    void verifyProfile();
    /// Lists the persistent crash guards (with the BIOS they were recorded under) and removes chosen ones.
    void manageCrashGuards();
    /// Single-core throughput on every searched core (centurion-cpustress percore --load sse): first CPU -> chunks,
    /// and the exit code (0 pass, 1 wrong result, other / -1 = no usable run).
    void measureThroughput(std::function<void(const QHash<int, int> &, int code)> then);
    /// After a finished search: the curve's run (curveRun_) against a stock run made right after the reset.
    void compareThroughput(std::function<void()> then);
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
    /// tone: a theme colour (theme::OK, DANGER, ...) for the log line and the status banner; nullptr = neutral.
    void note(const QString &line, const char *tone = nullptr);
    void probeTools(std::function<void(bool)> then);
    bool acAvailable();
    void updateEstimate();
    void updateForecast();
    void applyTuning();
    cosearch::Tuning tuning() const;
    void loadSettings();
    void saveSettings() const;
    bool writeJournal() const;
    void refreshTable();
    void showAttemptHistory();
    void showSessionArchive();
    void reloadPresets();
    void watchTick();
    void gatherEvidence(const QJsonObject &journal, const cosearch::Search &search, const cosearch::Trial &trial,
                        std::function<void(const coevidence::Verdict &)> then);
    void updateResultButtons();
    void saveResultAs();
    void loadResultIntoStarts();
    void applyPreset(const QString &name);
    bool saveProfile(const QString &name, int margin = 0) const;

    QComboBox *mode_, *level_, *scene_, *preset_;
    QSpinBox *limit_, *allStart_, *step_;
    QPushButton *applyTuning_;
    QLabel *estimate_, *status_;
    QProgressBar *coreProgress_;
    QTableWidget *table_;
    QGridLayout *coreMap_ = nullptr;
    QPlainTextEdit *log_;
    QPushButton *go_, *stop_, *saveResult_, *loadResult_, *verifyBtn_, *guardsBtn_;
    QProcess *test_ = nullptr;
    QPointer<QProcess> toolProbe_;
    bool preparing_ = false, probeCancelled_ = false, recoveryBusy_ = false;
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
    bool cleanExit_ = false;           // application quit with a confirmed stock reset: the pending trial is repeated, not failed
    std::optional<cosearch::Trial> replay_;  // resume after a clean exit: run this in-flight trial again instead of next()

    bool running_ = false, abort_ = false, applying_ = false, resetting_ = false, finished_ = false;
    bool sceneApplying_ = false, quitting_ = false;
    qint64 testStart_ = 0, testDeadline_ = 0;
    quint64 kmsgSeq_ = 0;
    QHash<QString, QString> mceBefore_;
    QString startScene_, priorScene_, bootId_;
    QJsonArray attempts_;
    QJsonObject trialEvidence_; // observation metadata only, never used to select a candidate
    QSet<int> excluded_;               // per-core: cores the user left out (coreKey(ccd, slot))
    QHash<int, int> starts_;           // per-core: each core's start (coreKey -> offset), typed or from a preset
    // Crash watcher (coevidence.h) of the trial in flight: what it saw, committed to the journal at once.
    QJsonArray evidence_;
    QHash<QString, QString> mceWatch_;
    quint64 watchSeq_ = 0;
    bool watchBusy_ = false;
    quint64 trialGeneration_ = 0; // invalidates late read-only callbacks across trials/resets/sessions
    bool currentTrial(quint64 generation) const {
        return generation == trialGeneration_ && running_ && !resetting_ && trial_.has_value();
    }
    int activeCpu_ = -1;               // the CPU centurion-cpustress last moved its single-core load to

    // Read-only insights (coinsights.h): shown with the results, never change what is tested or saved.
    QString verifiedProfile_;         // verification-only run of this saved profile (journal: "verify_profile")
    bool confirmPassed_ = false;       // the last trial was the final verification and passed: its curve is applied
    QHash<int, int> curveRun_;         // throughput with the found curve, first CPU -> chunks (until compared)
    QHash<int, double> perf_;          // core index -> throughput change against stock, in percent
    QHash<int, int> peakMhz_;          // core index -> highest clock seen while it was probed alone (journal: "peaks")
    QPointer<QProcess> perfTest_;
    QElapsedTimer searchElapsed_, testElapsed_; // display only; safety deadline remains independent
    qint64 measuredOverheadMs_ = 0;
    int measuredTests_ = 0;
    std::optional<cosearch::Search::WorkEstimate> passForecast_, failForecast_;
    Notifier *notifier_;
};
