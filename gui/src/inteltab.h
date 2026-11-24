#pragma once
#include <optional>
// Intel Undervolt tab — OC mailbox voltage offsets (5 planes), IccMax,
// TCC offset and package power limits, via intel-uv-helper (pkexec).
// Created only when the CPU vendor is GenuineIntel (see MainWindow).
#include <QJsonArray>
#include <QHash>
#include <QJsonObject>
#include <QWidget>
#include <functional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTimer;

namespace inteluv {
QString profilesDir();  // ~/.config/centurion/intel-uv-profiles
}

class TrialBar;
class IntelTab : public QWidget {
    Q_OBJECT
public:
    explicit IntelTab(QWidget *parent = nullptr);
    ~IntelTab() override;
    bool operationActive() const { return busy_; }

    QStringList savedProfileNames() const;
    /// Load + apply in one step (tray / game mode).
    bool applyNamedProfile(const QString &name);
    void applyReset();

protected:
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;

private:
    struct Row { QString key; QCheckBox *on; QDoubleSpinBox *val; QLabel *cur; };
    struct PlRow { QCheckBox *on; QDoubleSpinBox *w, *s; QLabel *cur; };

    QJsonObject currentProfile() const;
    void setProfile(const QJsonObject &p);
    bool validate(const QJsonObject &p);
    void readStatus();
    void showStatus(const QJsonObject &s);
    std::optional<bool> uvLock_;  // result of the last probe_uv_lock (session only)
    void runOp(const QJsonObject &req, std::function<void(const QJsonObject &)> then = {});
    void setBusy(bool b);
    void setPositive(bool on);
    void importThrottleStop();
    void saveBoot();
    void pollMonitor();          // legacy: one pkexec per sample (old helper without monitor_stream)
    void startMonitor();
    void stopMonitor();
    void startMonitorStream();   // one pkexec per session, one JSON line per sample
    void stopMonitorStream();
    void showMonitor(const QJsonObject &s);
    void updateLimits(const QJsonObject &limits);
    void resetLimits();
    QJsonArray hwpRules() const;
    void log(const QString &msg, const QString &level = "info");

    void reloadProfiles();
    void saveProfile();
    bool loadProfile(const QString &name);
    void deleteProfile();

    QList<Row> volt_, icc_;
    QCheckBox *link_ = nullptr, *tjOn_ = nullptr, *mchbar_ = nullptr;
    QLabel *noPlanes_ = nullptr;      // shown while no voltage plane has answered
    QList<QWidget *> planeInfo_;       // column heads and the undervolt note: hidden with the planes
    QSpinBox *tj_ = nullptr;
    QCheckBox *turboOn_ = nullptr;
    QSpinBox *turbo_ = nullptr;
    QLabel *turboCur_ = nullptr;
    QLabel *tjCur_ = nullptr, *info_ = nullptr, *bootLbl_ = nullptr, *plLock_ = nullptr;
    PlRow pl1_{}, pl2_{};
    QComboBox *profileCombo_ = nullptr;
    QPlainTextEdit *log_ = nullptr;
    QList<QPushButton *> buttons_;
    // extras (undervolt.py --force/--lock, throttled BDPROCHOT/cTDP)
    QCheckBox *positive_ = nullptr, *lock_ = nullptr;
    QComboBox *bdprochot_ = nullptr, *ctdp_ = nullptr;
    // ThrottleStop TPL/FIVR counterparts: clamp (tristate: partial = untouched), PL4/PP0 (powercap),
    // MMIO disable, turbo groups, intel_pstate perf %/EPP, uncore range, C1E, package C-state limit
    QCheckBox *clamp1_ = nullptr, *clamp2_ = nullptr, *pl4On_ = nullptr, *pp0On_ = nullptr, *mmioOff_ = nullptr;
    QDoubleSpinBox *pl4_ = nullptr, *pp0_ = nullptr;
    QLabel *pl4Cur_ = nullptr, *pp0Cur_ = nullptr;
    QCheckBox *groupsOn_ = nullptr;
    QList<QSpinBox *> groups_;
    QLabel *groupsCur_ = nullptr;
    QCheckBox *perfOn_ = nullptr, *uncOn_ = nullptr, *cstOn_ = nullptr;
    QSpinBox *perfMin_ = nullptr, *perfMax_ = nullptr, *uncMin_ = nullptr, *uncMax_ = nullptr, *cst_ = nullptr;
    QComboBox *epp_ = nullptr, *c1e_ = nullptr;
    QLabel *perfCur_ = nullptr, *uncCur_ = nullptr, *cstCur_ = nullptr;
    // boot / daemon (throttled AC/BATTERY + Update_Rate_s, intel-undervolt daemon/hwphint)
    QComboBox *bootTarget_ = nullptr;
    QSpinBox *interval_ = nullptr;
    QCheckBox *reapply_ = nullptr, *hwpOn_ = nullptr, *hwpMulti_ = nullptr;
    QComboBox *hwpMode_ = nullptr, *hwpAlgo_ = nullptr, *hwpLoadHint_ = nullptr, *hwpNormalHint_ = nullptr, *hwpDomain_ = nullptr, *hwpCmp_ = nullptr;
    QDoubleSpinBox *hwpThreshold_ = nullptr, *hwpWatts_ = nullptr;
    // daemon temperature alarm (ThrottleStop failsafe alarm): lower PL1/PL2 at a package temperature
    QCheckBox *alarmOn_ = nullptr;
    QSpinBox *alarmTemp_ = nullptr;
    QDoubleSpinBox *alarmPl1_ = nullptr, *alarmPl2_ = nullptr;
    // live monitor (throttled --monitor)
    QCheckBox *monOn_ = nullptr;
    QLabel *monLbl_ = nullptr;
    QTimer *monTimer_ = nullptr;
    QJsonObject monPrev_;
    // perf-limit-reason counters (MSR 0x64F / 0x6B0 / 0x6B1)
    QHash<QString, QLabel *> limCells_;   // "core:10" → cell
    QHash<QString, int> limCounts_;       // samples in which the reason was active
    QLabel *limInfo_ = nullptr;
    // per-CPU table (ThrottleStop main window): MHz, C0 %, °C, max °C since "Clear max"
    class QTableWidget *cpuTable_ = nullptr;
    QHash<int, int> maxTemp_;
    void updateCpuTable(const QJsonObject &s);
    // live-monitor history: trend chart (last 5 min) and an optional CSV log (ThrottleStop Log File)
    class LineChart *trend_ = nullptr;
    QComboBox *trendSel_ = nullptr;
    QCheckBox *csvOn_ = nullptr;
    class QFile *csv_ = nullptr;
    struct Sample { double t, pkgW, coreW, pkgC, vcore; };
    QVector<Sample> hist_;
    void recordSample(const QJsonObject &s, double pkgW, double coreW);
    void drawTrend();
    QPushButton *limRun_ = nullptr;
    int limSamples_ = 0;
    bool clearLogsNext_ = false;
    bool busy_ = false, readOnce_ = false, monInFlight_ = false;
    class QProcess *monStream_ = nullptr;  // parented to qApp (root child: exits on stdin EOF)
    bool monStreamFailed_ = false;         // helper predates monitor_stream → per-sample polling
    bool monStreamGotSample_ = false;
    TrialBar *trial_ = nullptr;
    void revertTrial();  // "Trial apply": auto-revert unless kept
};
