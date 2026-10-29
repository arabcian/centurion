#pragma once
// Health → Backup: two kinds of dated archive (engine: backup-helper).
//
//  * Centurion configuration — everything Centurion stores (scenes,
//    presets, curves, lighting, boot profiles, network-guard rules, calibration)
//    plus an optional "system profile" (Portage config, world set, kernel
//    config, fstab, boot/module/sysctl configuration, package list). Made
//    without root; restoring the root-owned part asks for the password.
//  * System image — `tar --acls --xattrs -cpf - / | pigz` into a chosen folder,
//    and the matching restore into a chosen target directory. Root, the
//    administrator password every time.
//
// One job at a time; the helper streams progress lines. Cancel closes its
// stdin (a root child cannot be killed from here). Nothing runs in the
// background: the list is re-read when the page is shown and after a job.
#include <QDateTime>
#include <QJsonObject>
#include <QPointer>
#include <QWidget>
#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTreeWidget;
class QFileInfo;

class BackupPage : public QWidget {
    Q_OBJECT
public:
    explicit BackupPage(QWidget *parent = nullptr);
    ~BackupPage() override;
    /// *Back up now* of the Centurion configuration box (first-run wizard too); result in jobFinished.
    void configBackup();

Q_SIGNALS:
    /// A job finished while the page was not on screen (tray notification).
    void alert(const QString &title, const QString &message);
    /// Every job's final summary line.
    void jobFinished(const QString &summary, bool good);

protected:
    void showEvent(QShowEvent *e) override;

private:
    using Done = std::function<void(const QJsonObject &)>;
    void run(const QJsonObject &req, bool root, const QString &what, Done done);
    void onLine(const QByteArray &line);
    void cancel();
    void setBusy(bool busy, const QString &what = QString());
    void log(const QString &line, const char *color = nullptr);
    void finished(const QString &summary, bool good);

    void refreshList();
    void updateFree();
    void saveSettings();
    QString selected(QString *kind = nullptr) const;
    void select(const QString &path);
    QStringList selectedPaths() const;
    /// Asks for what opens an encrypted image (passphrase, or a pasted / loaded age secret key) and puts it
    /// into `req` as "passphrase" or "identity". Nothing is stored. false = cancelled.
    bool askSecret(const QStringList &archives, QJsonObject *req);
    /// Asks for a new passphrase twice (system image with "Passphrase" encryption). false = cancelled.
    bool askNewPassphrase(QString *pass);
    void newKeyPair();
    void updateEncryptionUi();
    void compareConfigs();
    void compareWithCurrent();
    void runDiff(const QJsonObject &req);
    void showDiff(const QJsonObject &r);
    // Verification results of this user's runs (state file), shown in the list's Notes.
    void rememberVerify(const QString &path, bool ok, const QString &error);
    QJsonObject verifyRecord(const QFileInfo &fi) const;

    void configRestore();
    void systemBackup();
    void systemRestore();
    void verifySelected();
    // Automatic configuration backup + system image reminder (the GUI lives in the tray, so no scheduler is needed).
    void autoTick();
    QDateTime newest(const QString &dir, bool image) const;
    void pruneAuto(const QString &dir, int keep);
    void updateAutoNote();
    QSpinBox *autoDays_ = nullptr, *autoKeep_ = nullptr, *remindDays_ = nullptr;
    QLabel *autoNote_ = nullptr, *remindNote_ = nullptr;
    class QTimer *autoTimer_ = nullptr;

    QLineEdit *cfgDir_, *sysDir_, *target_, *recipient_;
    QComboBox *encMode_ = nullptr;
    QPushButton *keyGen_ = nullptr;
    QCheckBox *cfgProfile_, *cfgLogs_, *low_, *verify_, *home_, *incr_;
    QComboBox *comp_;
    QSpinBox *level_, *keep_;
    QPlainTextEdit *excludes_, *log_;
    QLabel *free_, *status_;
    QTreeWidget *list_;
    QProgressBar *bar_;
    QPushButton *cancel_;
    QList<QWidget *> actions_;     // disabled while a job runs
    QStringList extra_;            // archives added with "Add file…"

    QPointer<QProcess> proc_;
    bool procRoot_ = false, cancelling_ = false;
    QJsonObject final_;
    qint64 lastBytes_ = 0, lastElapsed_ = 0;
    QString step_;                 // "step 2 of 4" while a chain is restored
    double rate_ = 0;
};
