#include "backuppage.h"
#include <QTimer>

#include "privileged.h"
#include "sysinfo.h"
#include "theme.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QRegularExpression>
#include <QPushButton>
#include <QSettings>
#include <QShowEvent>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

namespace {

enum { PathRole = Qt::UserRole, KindRole };

// Mirrors backup::DEFAULT_EXCLUDES. "/x/*" keeps the empty mount point in the
// archive; /home/* is the "Include /home" box.
const char *const DEFAULT_EXCLUDES =
    "/proc/*\n/sys/*\n/dev/*\n/run/*\n/tmp/*\n/var/tmp/*\n/mnt/*\n/media/*\n/lost+found\n/var/cache/*\n/usr/portage/distfiles/*";

struct Comp { const char *id, *label; int lo, hi, def; };
const Comp COMPS[] = {
    {"pigz", "gzip (pigz, all cores) — .tar.gz", 1, 9, 6},
    {"zstd", "zstd (all cores, fastest) — .tar.zst", 1, 19, 3},
    {"xz", "xz (all cores, smallest, slow) — .tar.xz", 0, 9, 6},
};

QString helper() { return privileged::helperPath(QStringLiteral("backup-helper")); }
QString settingsPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/centurion/gui.ini");
}
QString human(qint64 bytes) { return QLocale::c().formattedDataSize(bytes, 1, QLocale::DataSizeIecFormat); }
QString clock(qint64 s) {
    return QStringLiteral("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
}
QString str(const QJsonObject &o, const char *k) { return o.value(QLatin1String(k)).toString(); }
qint64 num(const QJsonObject &o, const char *k) { return o.value(QLatin1String(k)).toInteger(); }

QJsonObject readJson(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > (1 << 20)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

// $XDG_STATE_HOME/centurion/backup-verify.json: what this user's verifications found, per archive.
QString verifyStatePath() {
    const QString base = qEnvironmentVariableIsEmpty("XDG_STATE_HOME") ? QDir::homePath() + QStringLiteral("/.local/state")
                                                                        : qEnvironmentVariable("XDG_STATE_HOME");
    return base + QStringLiteral("/centurion/backup-verify.json");
}

QString shortDate(const QString &iso) { return iso.left(16).replace(QLatin1Char('T'), QLatin1Char(' ')); }

// Mirrors backup::is_image_name (increments "-iN" are part of the name; encrypted images end in ".age").
bool isImageName(const QString &n) {
    return n.startsWith(QLatin1String("backup-")) &&
           (n.endsWith(QLatin1String(".tar.gz")) || n.endsWith(QLatin1String(".tar.zst")) || n.endsWith(QLatin1String(".tar.xz")) ||
            n.endsWith(QLatin1String(".tar.gz.age")) || n.endsWith(QLatin1String(".tar.zst.age")) || n.endsWith(QLatin1String(".tar.xz.age")));
}
bool isConfigName(const QString &n) { return n.startsWith(QLatin1String("centurion-config-")) && n.endsWith(QLatin1String(".tar.gz")); }

QHBoxLayout *folderRow(const QString &label, QLineEdit *edit, QWidget *owner, const QString &title) {
    auto *h = new QHBoxLayout;
    h->addWidget(new QLabel(label));
    h->addWidget(edit, 1);
    auto *b = new QPushButton(QStringLiteral("Browse…"));
    QObject::connect(b, &QPushButton::clicked, owner, [edit, owner, title] {
        const QString d = QFileDialog::getExistingDirectory(owner, title, edit->text());
        if (!d.isEmpty()) { edit->setText(d); Q_EMIT edit->editingFinished(); }
    });
    h->addWidget(b);
    return h;
}

} // namespace

BackupPage::BackupPage(QWidget *parent) : QWidget(parent) {
    QSettings st(settingsPath(), QSettings::IniFormat);
    st.beginGroup(QStringLiteral("backup"));
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(8, 6, 8, 6);
    v->setSpacing(6);
    auto action = [this](const char *text, QBoxLayout *l, const char *objName = nullptr) {
        auto *b = new QPushButton(QString::fromUtf8(text));
        if (objName) b->setObjectName(QLatin1String(objName));
        l->addWidget(b);
        actions_.append(b);
        return b;
    };

    // ── Centurion configuration ──
    auto *cb = new QGroupBox(QStringLiteral("Centurion configuration"));
    auto *cv = new QVBoxLayout(cb);
    cv->setContentsMargins(8, 6, 8, 6);
    cv->setSpacing(4);
    cfgDir_ = new QLineEdit(st.value(QStringLiteral("config_dir"), QDir::homePath() + QStringLiteral("/Backups/centurion")).toString());
    cv->addLayout(folderRow(QStringLiteral("Folder"), cfgDir_, this, QStringLiteral("Folder for configuration backups")));
    auto *cr = new QHBoxLayout;
    cfgProfile_ = new QCheckBox(QStringLiteral("System profile"));
    cfgProfile_->setChecked(st.value(QStringLiteral("config_profile"), true).toBool());
    cfgProfile_->setToolTip(QStringLiteral(
        "Also saves how this installation is put together, as reference material:\n"
        "/etc/portage, the world set, the kernel .config, fstab, GRUB/dracut/kernel settings,\n"
        "modprobe.d, modules-load.d, sysctl, conf.d, runlevels, udev rules, plus the kernel\n"
        "command line, DMI, loaded modules and the installed package list.\n"
        "It is stored under system-profile/ and is never restored automatically."));
    cfgLogs_ = new QCheckBox(QStringLiteral("Logs"));
    cfgLogs_->setChecked(st.value(QStringLiteral("config_logs"), false).toBool());
    cfgLogs_->setToolTip(QStringLiteral("Health / kernel-log history, the write log and the network-guard log."));
    cr->addWidget(cfgProfile_);
    cr->addWidget(cfgLogs_);
    auto *ci = new QLabel(QStringLiteral("Scenes, presets, CPU/GPU curves, lighting, boot profiles, network-guard rules, calibration — one dated .tar.gz."));
    ci->setProperty("role", "muted");
    cr->addWidget(ci, 1);
    connect(action("Back up now", cr, "btnAccent"), &QPushButton::clicked, this, &BackupPage::configBackup);
    connect(action("Restore selected…", cr), &QPushButton::clicked, this, &BackupPage::configRestore);
    auto *bCmp = new QPushButton(tr("Compare two…"));
    bCmp->setToolTip(tr("Select two \"Centurion configuration\" archives (Ctrl+click) to see what changed from the older to the newer: "
                        "files, keys in JSON / .ini / KEY=value files, lines in other text and, when both have a system profile, "
                        "the kernel configuration and installed packages. Reads only; restores nothing."));
    actions_.append(bCmp);
    cr->addWidget(bCmp);
    connect(bCmp, &QPushButton::clicked, this, &BackupPage::compareConfigs);
    auto *bNow = new QPushButton(tr("Compare with now…"));
    bNow->setToolTip(tr("What changed since the selected \"Centurion configuration\" archive was made: it is compared with the "
                        "configuration as it is now. Reads only; restores nothing."));
    actions_.append(bNow);
    cr->addWidget(bNow);
    connect(bNow, &QPushButton::clicked, this, &BackupPage::compareWithCurrent);
    cv->addLayout(cr);
    // Automatic: made by the tray app itself (as you, no password), when the newest configuration backup is older than this.
    auto *ar = new QHBoxLayout;
    autoDays_ = new QSpinBox;
    autoDays_->setRange(0, 365);
    autoDays_->setSpecialValueText(QStringLiteral("off"));
    autoDays_->setSuffix(QStringLiteral(" days"));
    autoDays_->setValue(st.value(QStringLiteral("auto_days"), 7).toInt());
    autoDays_->setToolTip(QStringLiteral("Centurion makes a configuration backup on its own when the newest one in the folder is this old\n"
                                         "(checked a few minutes after login and every 6 hours while it runs). Same options as Back up now."));
    autoKeep_ = new QSpinBox;
    autoKeep_->setRange(1, 99);
    autoKeep_->setValue(st.value(QStringLiteral("auto_keep"), 8).toInt());
    autoKeep_->setToolTip(QStringLiteral("Only automatic backups (…-auto.tar.gz) are pruned; the ones you make by hand are never deleted."));
    ar->addWidget(new QLabel(QStringLiteral("Automatic backup every")));
    ar->addWidget(autoDays_);
    ar->addWidget(new QLabel(QStringLiteral("keep newest")));
    ar->addWidget(autoKeep_);
    autoNote_ = new QLabel;
    autoNote_->setProperty("role", "muted");
    ar->addWidget(autoNote_, 1);
    cv->addLayout(ar);
    v->addWidget(cb);

    // ── System image ──
    auto *sb = new QGroupBox(QStringLiteral("System image (tar of /)"));
    auto *sv = new QVBoxLayout(sb);
    sv->setContentsMargins(8, 6, 8, 6);
    sv->setSpacing(4);
    sysDir_ = new QLineEdit(st.value(QStringLiteral("system_dir"), QStringLiteral("/")).toString());
    auto *fr = folderRow(QStringLiteral("Folder"), sysDir_, this, QStringLiteral("Folder for system images"));
    free_ = new QLabel;
    free_->setProperty("role", "muted");
    fr->addWidget(free_);
    sv->addLayout(fr);

    auto *o = new QHBoxLayout;
    comp_ = new QComboBox;
    for (const Comp &c : COMPS) comp_->addItem(QString::fromUtf8(c.label), QLatin1String(c.id));
    comp_->setCurrentIndex(std::max(0, comp_->findData(st.value(QStringLiteral("compressor"), QStringLiteral("pigz")))));
    level_ = new QSpinBox;
    auto levelRange = [this](bool reset) {
        const Comp &c = COMPS[comp_->currentIndex()];
        level_->setRange(c.lo, c.hi);
        if (reset) level_->setValue(c.def);
    };
    levelRange(true);
    if (st.contains(QStringLiteral("level"))) level_->setValue(st.value(QStringLiteral("level")).toInt());
    connect(comp_, &QComboBox::currentIndexChanged, this, [levelRange] { levelRange(true); });
    low_ = new QCheckBox(QStringLiteral("Low priority"));
    low_->setChecked(st.value(QStringLiteral("low_priority"), true).toBool());
    low_->setToolTip(QStringLiteral("tar and the compressor run at nice 19 and the lowest I/O priority: the desktop stays responsive."));
    verify_ = new QCheckBox(QStringLiteral("Verify"));
    verify_->setChecked(st.value(QStringLiteral("verify"), true).toBool());
    verify_->setToolTip(QStringLiteral("Reads the finished archive back (decompress + tar listing) and compares the entry count."));
    home_ = new QCheckBox(QStringLiteral("Include /home"));
    home_->setChecked(st.value(QStringLiteral("include_home"), false).toBool());
    keep_ = new QSpinBox;
    keep_->setRange(0, 99);
    keep_->setSpecialValueText(QStringLiteral("all"));
    keep_->setValue(st.value(QStringLiteral("keep"), 0).toInt());
    keep_->setToolTip(tr("After a successful image, older image sets in this folder beyond this count are deleted. A set is a full image with its increments, removed whole."));
    o->addWidget(new QLabel(QStringLiteral("Compression")));
    o->addWidget(comp_);
    o->addWidget(new QLabel(QStringLiteral("Level")));
    o->addWidget(level_);
    o->addWidget(low_);
    o->addWidget(verify_);
    o->addWidget(home_);
    o->addWidget(new QLabel(QStringLiteral("Keep newest")));
    o->addWidget(keep_);
    o->addStretch(1);
    sv->addLayout(o);

    auto *ir = new QHBoxLayout;
    incr_ = new QCheckBox(tr("Incremental"));
    incr_->setChecked(st.value(QStringLiteral("incremental"), false).toBool());
    incr_->setToolTip(tr("Only what changed since the newest image in the folder. The first image is a full one; up to 6 increments follow it. "
                         "A chain is only ever pruned whole, and a restore applies the full image and then each increment in order."));
    encMode_ = new QComboBox;
    encMode_->addItem(tr("Not encrypted"), QStringLiteral("none"));
    encMode_->addItem(tr("Passphrase"), QStringLiteral("passphrase"));
    encMode_->addItem(tr("Public key (age)"), QStringLiteral("recipient"));
    const QString savedRecipient = st.value(QStringLiteral("recipient")).toString().trimmed();
    const QString savedMode = st.value(QStringLiteral("encryption"), savedRecipient.isEmpty() ? QStringLiteral("none") : QStringLiteral("recipient")).toString();
    encMode_->setCurrentIndex(std::max(0, encMode_->findData(savedMode)));
    encMode_->setToolTip(tr("The image holds /etc/shadow and every private key on the system: encrypt it when it leaves this computer.\n"
                            "Passphrase: asked when the image is made and when it is read back; never stored.\n"
                            "Public key: anyone can make images with it, only the matching secret key reads them — the key can stay off this computer.\n"
                            "Uses age (app-crypt/age). An increment keeps the encryption of its chain."));
    recipient_ = new QLineEdit(savedRecipient);
    recipient_->setPlaceholderText(tr("age1… public key"));
    recipient_->setToolTip(tr("Encrypts the image with age to this public key (a public key is not a secret). Reading the image back needs the "
                              "matching AGE-SECRET-KEY line, which Centurion asks for when needed and never stores. Keep that key somewhere other "
                              "than this computer: without it the image cannot be read."));
    keyGen_ = new QPushButton(tr("New key pair…"));
    keyGen_->setToolTip(tr("Makes a new age key pair with age-keygen: the secret key goes into a file you choose (keep it off this computer, "
                           "e.g. on a USB stick or in a password manager), the public key is filled in here."));
    connect(keyGen_, &QPushButton::clicked, this, &BackupPage::newKeyPair);
    actions_.append(keyGen_);
    ir->addWidget(incr_);
    ir->addSpacing(12);
    ir->addWidget(new QLabel(tr("Encryption")));
    ir->addWidget(encMode_);
    ir->addWidget(recipient_, 1);
    ir->addWidget(keyGen_);
    ir->addStretch(0);
    sv->addLayout(ir);
    connect(encMode_, &QComboBox::currentIndexChanged, this, [this] { updateEncryptionUi(); saveSettings(); });
    updateEncryptionUi();

    auto *er = new QHBoxLayout;
    auto *el = new QLabel(QStringLiteral("Exclude"));
    el->setAlignment(Qt::AlignTop);
    er->addWidget(el);
    excludes_ = new QPlainTextEdit(st.value(QStringLiteral("excludes"), QString::fromLatin1(DEFAULT_EXCLUDES)).toString());
    excludes_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    excludes_->setFixedHeight(fontMetrics().lineSpacing() * 4 + 12);
    excludes_->setToolTip(QStringLiteral(
        "One tar --exclude pattern per line. \"/proc/*\" (not \"/proc\") keeps the empty mount point,\n"
        "so a restore onto a fresh disk can still mount /proc, /sys, /dev and /run.\n"
        "The archive being written and older backup-* images in the folder are always excluded."));
    er->addWidget(excludes_, 1);
    auto *ec = new QVBoxLayout;
    auto *bDef = new QPushButton(QStringLiteral("Defaults"));
    connect(bDef, &QPushButton::clicked, this, [this] { excludes_->setPlainText(QString::fromLatin1(DEFAULT_EXCLUDES)); });
    ec->addWidget(bDef);
    connect(action("Create image", ec, "btnAccent"), &QPushButton::clicked, this, &BackupPage::systemBackup);
    ec->addStretch(1);
    er->addLayout(ec);
    sv->addLayout(er);

    target_ = new QLineEdit(st.value(QStringLiteral("restore_target")).toString());
    target_->setPlaceholderText(QStringLiteral("/mnt/gentoo — or / to restore over the running system"));
    auto *rr = folderRow(QStringLiteral("Restore to"), target_, this, QStringLiteral("Directory to restore the image into"));
    connect(action("Verify selected", rr), &QPushButton::clicked, this, &BackupPage::verifySelected);
    connect(action("Restore selected…", rr, "btnDanger"), &QPushButton::clicked, this, &BackupPage::systemRestore);
    sv->addLayout(rr);
    // A system image needs the password and minutes of I/O: Centurion only reminds, at the interval you set.
    auto *rm = new QHBoxLayout;
    remindDays_ = new QSpinBox;
    remindDays_->setRange(0, 365);
    remindDays_->setSpecialValueText(QStringLiteral("off"));
    remindDays_->setSuffix(QStringLiteral(" days"));
    remindDays_->setValue(st.value(QStringLiteral("remind_days"), 30).toInt());
    remindDays_->setToolTip(QStringLiteral("A notification when the newest system image in the folder is older than this (at most once a day)."));
    rm->addWidget(new QLabel(QStringLiteral("Remind me when the newest image is older than")));
    rm->addWidget(remindDays_);
    remindNote_ = new QLabel;
    remindNote_->setProperty("role", "muted");
    rm->addWidget(remindNote_, 1);
    sv->addLayout(rm);
    v->addWidget(sb);

    // ── Archives ──
    list_ = new QTreeWidget;
    list_->setColumnCount(5);
    list_->setHeaderLabels({tr("Archive"), tr("Kind"), tr("Size"), tr("Written"), tr("Notes")});
    list_->setRootIsDecorated(true);
    list_->setAlternatingRowColors(true);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setUniformRowHeights(true);
    for (int i = 0; i < 4; ++i) list_->header()->setSectionResizeMode(i, QHeaderView::ResizeToContents);
    list_->header()->setStretchLastSection(true);
    v->addWidget(list_, 1);

    auto *pr = new QHBoxLayout;
    bar_ = new QProgressBar;
    bar_->setFixedWidth(180);
    bar_->setTextVisible(false);
    bar_->hide();
    pr->addWidget(bar_);
    status_ = new QLabel(QStringLiteral("Idle."));
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pr->addWidget(status_, 1);
    auto *bAdd = action("Add file…", pr);
    bAdd->setToolTip(QStringLiteral("List an archive that lives somewhere else (to verify or restore it)."));
    connect(bAdd, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, QStringLiteral("Backup archive"), sysDir_->text(),
                                                       QStringLiteral("tar archives (*.tar *.tar.gz *.tgz *.tar.zst *.tar.xz *.tar.bz2 *.age);;All files (*)"));
        if (f.isEmpty()) return;
        if (!extra_.contains(f)) extra_.append(f);
        refreshList();
        select(f);
    });
    cancel_ = new QPushButton(QStringLiteral("Cancel"));
    cancel_->setEnabled(false);
    connect(cancel_, &QPushButton::clicked, this, &BackupPage::cancel);
    pr->addWidget(cancel_);
    v->addLayout(pr);

    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(400);
    log_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    log_->setFixedHeight(fontMetrics().lineSpacing() * 5 + 12);
    v->addWidget(log_);

    for (QLineEdit *e : {cfgDir_, sysDir_})
        connect(e, &QLineEdit::editingFinished, this, [this] { saveSettings(); refreshList(); updateFree(); });
    connect(target_, &QLineEdit::editingFinished, this, &BackupPage::saveSettings);
    for (QSpinBox *s : {autoDays_, autoKeep_, remindDays_})
        connect(s, &QSpinBox::valueChanged, this, [this] { saveSettings(); updateAutoNote(); });

    autoTimer_ = new QTimer(this);
    autoTimer_->setTimerType(Qt::VeryCoarseTimer);
    autoTimer_->setInterval(6 * 3600 * 1000);
    connect(autoTimer_, &QTimer::timeout, this, &BackupPage::autoTick);
    if (!qEnvironmentVariableIsSet("CENTURION_PGO_TRAIN")) {
        autoTimer_->start();
        QTimer::singleShot(3 * 60 * 1000, this, &BackupPage::autoTick);  // after login settles
    }
    updateAutoNote();
}

QDateTime BackupPage::newest(const QString &dir, bool image) const {
    QDateTime t;
    if (dir.isEmpty()) return t;
    for (const QFileInfo &fi : QDir(dir).entryInfoList(QDir::Files | QDir::System, QDir::NoSort)) {
        if (image ? !isImageName(fi.fileName()) : !isConfigName(fi.fileName())) continue;
        if (!t.isValid() || fi.lastModified() > t) t = fi.lastModified();
    }
    return t;
}

void BackupPage::updateAutoNote() {
    auto age = [](const QDateTime &t) {
        if (!t.isValid()) return QStringLiteral("none yet");
        const qint64 d = t.daysTo(QDateTime::currentDateTime());
        return d == 0 ? QStringLiteral("newest: today") : QStringLiteral("newest: %1 day(s) ago").arg(d);
    };
    if (autoNote_) autoNote_->setText(age(newest(cfgDir_->text().trimmed(), false)));
    if (remindNote_) remindNote_->setText(age(newest(sysDir_->text().trimmed(), true)));
}

/// Only "-auto" archives are pruned, oldest first, beyond `keep`.
void BackupPage::pruneAuto(const QString &dir, int keep) {
    QFileInfoList autos;
    for (const QFileInfo &fi : QDir(dir).entryInfoList(QDir::Files, QDir::Time))
        if (isConfigName(fi.fileName()) && fi.fileName().endsWith(QLatin1String("-auto.tar.gz"))) autos << fi;
    for (int i = keep; i < autos.size(); ++i)
        if (QFile::remove(autos.at(i).absoluteFilePath())) log(QStringLiteral("removed old automatic backup ") + autos.at(i).fileName(), theme::MUTED);
}

void BackupPage::autoTick() {
    QSettings st(settingsPath(), QSettings::IniFormat);
    st.beginGroup(QStringLiteral("backup"));
    const QDateTime now = QDateTime::currentDateTime();
    // System image reminder: at most once a day.
    if (const int rd = remindDays_->value(); rd > 0) {
        const QDateTime img = newest(sysDir_->text().trimmed(), true);
        const QDateTime last = st.value(QStringLiteral("last_reminder")).toDateTime();
        if ((!img.isValid() || img.daysTo(now) >= rd) && (!last.isValid() || last.secsTo(now) >= 20 * 3600)) {
            st.setValue(QStringLiteral("last_reminder"), now);
            Q_EMIT alert(QStringLiteral("Backup reminder"),
                         img.isValid() ? QStringLiteral("The newest system image is %1 days old. Health → Backup → Create image.").arg(img.daysTo(now))
                                       : QStringLiteral("No system image yet. Health → Backup → Create image."));
        }
    }
    // Automatic configuration backup.
    const int days = autoDays_->value();
    const QString dir = cfgDir_->text().trimmed();
    if (days <= 0 || proc_ || !QDir::isAbsolutePath(dir)) { updateAutoNote(); return; }
    const QDateTime cfg = newest(dir, false);
    if (cfg.isValid() && cfg.secsTo(now) < qint64(days) * 86400 - 3600) { updateAutoNote(); return; }
    const int keep = autoKeep_->value();
    run({{"op", "config_backup"}, {"dest_dir", dir}, {"include_profile", cfgProfile_->isChecked()}, {"include_logs", cfgLogs_->isChecked()},
         {"tag", "auto"}, {"centurion_version", QStringLiteral(CENTURION_VERSION)}},
        false, QStringLiteral("Automatic configuration backup…"), [this, dir, keep](const QJsonObject &r) {
            if (!r.value("ok").toBool()) { finished(QStringLiteral("Automatic configuration backup failed: ") + str(r, "error"), false); return; }
            pruneAuto(dir, keep);
            finished(QStringLiteral("Automatic configuration backup saved: %1 (%2).").arg(QFileInfo(str(r, "archive")).fileName(), human(num(r, "bytes"))), true);
            updateAutoNote();
        });
}

BackupPage::~BackupPage() {
    saveSettings();
    if (proc_) proc_->closeWriteChannel();  // the helper cancels on EOF
}

void BackupPage::saveSettings() {
    QSettings st(settingsPath(), QSettings::IniFormat);
    st.beginGroup(QStringLiteral("backup"));
    st.setValue(QStringLiteral("config_dir"), cfgDir_->text());
    st.setValue(QStringLiteral("config_profile"), cfgProfile_->isChecked());
    st.setValue(QStringLiteral("config_logs"), cfgLogs_->isChecked());
    st.setValue(QStringLiteral("system_dir"), sysDir_->text());
    st.setValue(QStringLiteral("compressor"), comp_->currentData());
    st.setValue(QStringLiteral("level"), level_->value());
    st.setValue(QStringLiteral("low_priority"), low_->isChecked());
    st.setValue(QStringLiteral("verify"), verify_->isChecked());
    st.setValue(QStringLiteral("include_home"), home_->isChecked());
    st.setValue(QStringLiteral("keep"), keep_->value());
    st.setValue(QStringLiteral("incremental"), incr_->isChecked());
    st.setValue(QStringLiteral("recipient"), recipient_->text().trimmed());
    if (encMode_) st.setValue(QStringLiteral("encryption"), encMode_->currentData());
    st.setValue(QStringLiteral("excludes"), excludes_->toPlainText());
    st.setValue(QStringLiteral("restore_target"), target_->text());
    if (autoDays_) {
        st.setValue(QStringLiteral("auto_days"), autoDays_->value());
        st.setValue(QStringLiteral("auto_keep"), autoKeep_->value());
        st.setValue(QStringLiteral("remind_days"), remindDays_->value());
    }
}

void BackupPage::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    refreshList();
    updateFree();
    updateAutoNote();
}

void BackupPage::updateFree() {
    const QStorageInfo si(sysDir_->text());
    free_->setText(si.isValid() ? QStringLiteral("%1 free on %2").arg(human(si.bytesAvailable()), si.rootPath()) : QString());
}

void BackupPage::refreshList() {
    const QString keepSel = selected();
    list_->clear();
    struct Entry { QFileInfo fi; bool image; QJsonObject info; };
    QList<Entry> found;
    QSet<QString> seen;
    auto add = [&](const QFileInfo &fi, bool img) {
        if (seen.contains(fi.absoluteFilePath())) return;
        seen.insert(fi.absoluteFilePath());
        found.append({fi, img, img ? readJson(fi.absoluteFilePath() + QStringLiteral(".info.json")) : QJsonObject()});
    };
    auto scan = [&](const QString &dir) {
        const QDir d(dir);
        if (dir.isEmpty() || !d.exists()) return;
        for (const QFileInfo &fi : d.entryInfoList(QDir::Files | QDir::Hidden | QDir::System, QDir::NoSort)) {
            const bool img = isImageName(fi.fileName()), cfg = isConfigName(fi.fileName());
            if (img || cfg) add(fi, img);
        }
    };
    scan(cfgDir_->text());
    scan(sysDir_->text());
    for (const QString &f : std::as_const(extra_)) {
        const QFileInfo fi(f);
        if (fi.isFile()) add(fi, !isConfigName(fi.fileName()));
    }
    std::sort(found.begin(), found.end(), [](const Entry &a, const Entry &b) { return a.fi.lastModified() > b.fi.lastModified(); });

    // An increment hangs under the full image of its chain (when that is listed); the chain row sums the set.
    QHash<QString, QTreeWidgetItem *> chainHead;   // full image's file name → its row
    QHash<QString, int> chainCount;
    QHash<QString, qint64> chainBytes;
    for (const Entry &e : std::as_const(found))
        if (e.image && num(e.info, "level") > 0) {
            chainCount[str(e.info, "chain")] += 1;
            chainBytes[str(e.info, "chain")] += e.fi.size();
        }
    QTreeWidgetItem *current = nullptr;
    auto makeItem = [&](const Entry &e) {
        QStringList notes;
        bool bad = false;
        if (e.image) {
            const QJsonObject &info = e.info;
            if (!info.isEmpty()) {
                notes << tr("%L1 entries").arg(num(info, "files"));
                if (num(info, "level") > 0) notes << tr("increment %1").arg(num(info, "level"));
                if (str(info, "encryption") == QLatin1String("passphrase")) notes << tr("encrypted (passphrase)");
                else if (info.value(QLatin1String("encrypted")).toBool()) notes << tr("encrypted (key)");
                if (!str(info, "verified").isEmpty()) notes << tr("verified %1").arg(shortDate(str(info, "verified")));
                if (info.value(QLatin1String("include_home")).toBool()) notes << tr("with /home");
                if (num(info, "tar_status") == 2) notes << tr("tar reported errors");
                if (!str(info, "kernel").isEmpty()) notes << tr("kernel %1").arg(str(info, "kernel"));
            }
        } else if (e.fi.fileName().contains(QLatin1String("pre-restore"))) {
            notes << tr("saved automatically before a restore");
        } else if (e.fi.fileName().contains(QLatin1String("-pre-"))) {
            notes << tr("saved automatically before a firmware write");
        }
        // This user's own verification runs (a root run also stamps the sidecar above).
        const QJsonObject v = verifyRecord(e.fi);
        if (!v.isEmpty()) {
            if (v.value(QLatin1String("ok")).toBool()) notes << tr("readable %1").arg(shortDate(str(v, "when")));
            else { notes.prepend(tr("FAILED verification %1: %2").arg(shortDate(str(v, "when")), str(v, "error"))); bad = true; }
        }
        auto *it = new QTreeWidgetItem({e.fi.fileName(), e.image ? tr("System image") : tr("Centurion configuration"),
                                        human(e.fi.size()), e.fi.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                                        notes.join(QStringLiteral(" · "))});
        it->setData(0, PathRole, e.fi.absoluteFilePath());
        it->setData(0, KindRole, e.image ? QStringLiteral("system") : QStringLiteral("config"));
        it->setToolTip(0, e.fi.absoluteFilePath());
        it->setToolTip(4, notes.join(QLatin1Char('\n')));
        it->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        if (bad) for (int c = 0; c < 5; ++c) it->setForeground(c, QColor(QLatin1String(theme::DANGER)));
        if (e.fi.absoluteFilePath() == keepSel) current = it;
        return it;
    };
    // Full images (and everything else) first, so increments find their head.
    for (const Entry &e : std::as_const(found)) {
        if (e.image && num(e.info, "level") > 0) continue;
        QTreeWidgetItem *it = makeItem(e);
        list_->addTopLevelItem(it);
        if (e.image && chainCount.contains(e.fi.fileName())) chainHead.insert(e.fi.fileName(), it);
    }
    for (const Entry &e : std::as_const(found)) {
        if (!(e.image && num(e.info, "level") > 0)) continue;
        QTreeWidgetItem *it = makeItem(e);
        if (QTreeWidgetItem *head = chainHead.value(str(e.info, "chain"))) head->insertChild(0, it);  // in chain order, under the head
        else { it->setText(4, tr("its full image is not listed — it cannot be restored alone") + QStringLiteral(" · ") + it->text(4));
               it->setForeground(4, QColor(QLatin1String(theme::WARN))); list_->addTopLevelItem(it); }
    }
    for (auto i = chainHead.cbegin(); i != chainHead.cend(); ++i) {
        QTreeWidgetItem *head = i.value();
        head->setText(4, tr("full image + %n increment(s), %1 in all", nullptr, chainCount.value(i.key()))
                              .arg(human(QFileInfo(head->data(0, PathRole).toString()).size() + chainBytes.value(i.key())))
                         + QStringLiteral(" · ") + head->text(4));
        head->setExpanded(true);
    }
    if (current) list_->setCurrentItem(current);
}

QJsonObject BackupPage::verifyRecord(const QFileInfo &fi) const {
    const QJsonObject all = readJson(verifyStatePath());
    const QJsonObject r = all.value(fi.absoluteFilePath()).toObject();
    // Only for the file as it was then: a rewritten archive is not the one that was checked.
    if (r.isEmpty() || num(r, "size") != fi.size() || num(r, "mtime") != fi.lastModified().toSecsSinceEpoch()) return {};
    return r;
}

void BackupPage::rememberVerify(const QString &path, bool ok, const QString &error) {
    const QFileInfo fi(path);
    if (!fi.exists()) return;
    QJsonObject all = readJson(verifyStatePath());
    all.insert(fi.absoluteFilePath(), QJsonObject{{"ok", ok}, {"when", QDateTime::currentDateTime().toString(Qt::ISODate)},
                                                  {"error", error.left(200)}, {"size", fi.size()},
                                                  {"mtime", fi.lastModified().toSecsSinceEpoch()}});
    // Bounded: forget files that are gone, then the oldest beyond 300.
    for (const QString &k : all.keys()) if (!QFileInfo::exists(k)) all.remove(k);
    while (all.size() > 300) {
        QString oldest;
        for (auto i = all.constBegin(); i != all.constEnd(); ++i)
            if (oldest.isEmpty() || str(i.value().toObject(), "when") < str(all.value(oldest).toObject(), "when")) oldest = i.key();
        all.remove(oldest);
    }
    QDir().mkpath(QFileInfo(verifyStatePath()).absolutePath());
    QFile f(verifyStatePath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(all).toJson(QJsonDocument::Compact));
}

void BackupPage::select(const QString &path) {
    for (QTreeWidgetItemIterator it(list_); *it; ++it)
        if ((*it)->data(0, PathRole).toString() == path) { list_->setCurrentItem(*it); return; }
}

QString BackupPage::selected(QString *kind) const {
    const QTreeWidgetItem *it = list_->currentItem();
    if (!it) return {};
    if (kind) *kind = it->data(0, KindRole).toString();
    return it->data(0, PathRole).toString();
}

QStringList BackupPage::selectedPaths() const {
    QStringList out;
    for (const QTreeWidgetItem *it : list_->selectedItems()) out << it->data(0, PathRole).toString();
    return out;
}

// ── job plumbing ────────────────────────────────────────────────────────────

void BackupPage::log(const QString &line, const char *color) {
    if (color) log_->appendHtml(QStringLiteral("<span style='color:%1'>%2</span>").arg(QLatin1String(color), line.toHtmlEscaped()));
    else log_->appendPlainText(line);
}

void BackupPage::setBusy(bool busy, const QString &what) {
    for (QWidget *w : std::as_const(actions_)) w->setEnabled(!busy);
    cancel_->setEnabled(busy);
    bar_->setVisible(busy);
    if (busy) {
        bar_->setRange(0, 0);
        status_->setText(what);
        lastBytes_ = lastElapsed_ = 0;
        rate_ = 0;
        cancelling_ = false;
    }
}

void BackupPage::finished(const QString &summary, bool good) {
    status_->setText(summary);
    theme::setSheet(status_, QStringLiteral("color:%1").arg(QLatin1String(good ? theme::OK : theme::DANGER)));
    log(summary, good ? theme::OK : theme::DANGER);
    refreshList();
    updateFree();
    if (!isVisible()) Q_EMIT alert(QStringLiteral("Backup"), summary);
    Q_EMIT jobFinished(summary, good);
}

void BackupPage::run(const QJsonObject &req, bool root, const QString &what, Done done) {
    auto failNow = [&](const QString &msg) { done(QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), msg}}); };
    if (proc_) return failNow(QStringLiteral("another backup job is running"));
    if (qEnvironmentVariableIsSet("CENTURION_PGO_TRAIN")) return failNow(QStringLiteral("disabled during the PGO training run"));
    if (!QFileInfo(helper()).isExecutable()) return failNow(QStringLiteral("backup-helper is not installed (") + helper() + QLatin1Char(')'));
    QString pkexec;
    for (const char *c : {"/usr/bin/pkexec", "/bin/pkexec"})
        if (QFileInfo(QString::fromLatin1(c)).isFile()) { pkexec = QString::fromLatin1(c); break; }
    if (root && pkexec.isEmpty()) return failNow(QStringLiteral("pkexec was not found. Install polkit (sys-auth/polkit)."));

    // Parented to the app: a root child cannot be killed from here, and a
    // QProcess destructor would block on it. It ends when its stdin closes.
    auto *p = new QProcess(QCoreApplication::instance());
    proc_ = p;
    procRoot_ = root;
    final_ = {};
    theme::setSheet(status_, QString());
    setBusy(true, what);
    QPointer<BackupPage> self(this);
    connect(p, &QProcess::readyReadStandardOutput, this, [this, p] { while (p->canReadLine()) onLine(p->readLine()); });
    auto end = [self, p, done](const QJsonObject &fallback) {
        p->deleteLater();
        if (!self) return;
        while (p->canReadLine()) self->onLine(p->readLine());
        self->proc_ = nullptr;
        self->setBusy(false);
        done(self->final_.isEmpty() ? fallback : self->final_);
    };
    connect(p, &QProcess::finished, p, [end, p](int code, QProcess::ExitStatus) {
        const QString err = QString::fromUtf8(p->readAllStandardError().left(2048)).trimmed();
        QString msg = code == 126 ? QStringLiteral("the authorization request was dismissed.")
                    : code == 127 ? QStringLiteral("polkit did not authorize this action.")
                    : err.isEmpty() ? QStringLiteral("the helper exited with code %1.").arg(code) : err;
        end(QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), msg}});
    });
    connect(p, &QProcess::errorOccurred, p, [end, p](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            end(QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("could not start: ") + p->errorString()}});
    });
    if (root) { p->setProgram(pkexec); p->setArguments({helper()}); }
    else p->setProgram(helper());
    p->start();
    p->write(QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n');  // stdin stays open: EOF = cancel
}

void BackupPage::cancel() {
    if (!proc_ || cancelling_) return;
    cancelling_ = true;
    status_->setText(QStringLiteral("Cancelling…"));
    proc_->closeWriteChannel();
    // Still waiting for the password: the helper is not running yet, pkexec can be stopped.
    if (procRoot_ && final_.isEmpty() && lastElapsed_ == 0 && bar_->maximum() == 0) proc_->terminate();
}

void BackupPage::onLine(const QByteArray &line) {
    const QJsonObject o = QJsonDocument::fromJson(line).object();
    if (o.isEmpty()) return;
    if (o.contains(QLatin1String("ok"))) { final_ = o; return; }
    const QString ev = str(o, "event"), phase = str(o, "phase");
    if (ev == QLatin1String("start")) {
        if (!str(o, "command").isEmpty()) log(QStringLiteral("$ ") + str(o, "command"), theme::MUTED);
        for (const QJsonValue &n : o.value(QLatin1String("notes")).toArray()) log(n.toString(), theme::MUTED);
        const QString name = QFileInfo(str(o, "archive")).fileName();
        step_.clear();
        if (num(o, "step") > 0 && num(o, "steps") > 1) {
            step_ = tr("step %1 of %2").arg(num(o, "step")).arg(num(o, "steps"));
            log(tr("%1: %2").arg(step_, name), theme::MUTED);
        }
        status_->setText((phase == QLatin1String("backup") ? QStringLiteral("Writing ") : phase == QLatin1String("restore")
                          ? QStringLiteral("Restoring ") : QStringLiteral("Verifying ")) + name
                         + (step_.isEmpty() ? QString() : QStringLiteral(" (") + step_ + QLatin1Char(')')) + QStringLiteral("…"));
        lastBytes_ = lastElapsed_ = 0;
        rate_ = 0;
        return;
    }
    if (ev != QLatin1String("progress") || cancelling_) return;
    const qint64 bytes = num(o, "bytes"), total = num(o, "total"), el = num(o, "elapsed");
    if (el > lastElapsed_ && bytes >= lastBytes_) {
        const double r = double(bytes - lastBytes_) / double(el - lastElapsed_);
        rate_ = rate_ > 0 ? rate_ * 0.7 + r * 0.3 : r;
    }
    lastBytes_ = bytes;
    lastElapsed_ = el;
    QString t;
    if (total > 0) {
        bar_->setRange(0, 1000);
        bar_->setValue(int(bytes * 1000 / total));
        t = QStringLiteral("%1 %2 of %3").arg(phase == QLatin1String("restore") ? QStringLiteral("Restoring:") : QStringLiteral("Verifying:"),
                                               human(bytes), human(total));
        if (rate_ > 0 && bytes < total) t += QStringLiteral(" · about %1 left").arg(clock(qint64(double(total - bytes) / rate_)));
    } else {
        bar_->setRange(0, 0);
        t = QStringLiteral("Writing: %1").arg(human(bytes));
    }
    if (!step_.isEmpty()) t += QStringLiteral(" · ") + step_;
    t += QStringLiteral(" · %1/s · %L2 entries · %3").arg(human(qint64(rate_))).arg(num(o, "files")).arg(clock(el));
    const QString cur = str(o, "current");
    if (!cur.isEmpty()) t += QStringLiteral(" · ") + status_->fontMetrics().elidedText(cur, Qt::ElideLeft, 320);
    status_->setText(t);
}

// ── Centurion configuration ───────────────────────────────────────────────────────

void BackupPage::configBackup() {
    saveSettings();
    const QString dir = cfgDir_->text().trimmed();
    if (!QDir::isAbsolutePath(dir)) { finished(QStringLiteral("Choose a folder for the configuration backups first."), false); return; }
    run({{"op", "config_backup"}, {"dest_dir", dir}, {"include_profile", cfgProfile_->isChecked()},
         {"include_logs", cfgLogs_->isChecked()}, {"centurion_version", QStringLiteral(CENTURION_VERSION)}},
        false, QStringLiteral("Saving the configuration…"), [this](const QJsonObject &r) {
            if (!r.value("ok").toBool()) { finished(QStringLiteral("Configuration backup failed: ") + str(r, "error"), false); return; }
            const QJsonObject c = r.value("contents").toObject();
            const QJsonArray skipped = r.value("skipped").toArray();
            for (const QJsonValue &s : skipped) log(QStringLiteral("not saved: ") + s.toString(), theme::WARN);
            finished(QStringLiteral("Saved %1 (%2): %3 user, %4 system, %5 system-profile file(s)%6.")
                         .arg(QFileInfo(str(r, "archive")).fileName(), human(num(r, "bytes")))
                         .arg(num(c, "user_config") + num(c, "user_state")).arg(num(c, "system")).arg(num(c, "system_profile"))
                         .arg(skipped.isEmpty() ? QString() : QStringLiteral(", %1 unreadable (see below)").arg(skipped.size())), true);
            select(str(r, "archive"));
        });
}

void BackupPage::configRestore() {
    QString kind;
    const QString archive = selected(&kind);
    if (archive.isEmpty() || kind != QLatin1String("config")) {
        finished(QStringLiteral("Select an \"Centurion configuration\" archive in the list first."), false);
        return;
    }
    run({{"op", "config_inspect"}, {"archive", archive}}, false, QStringLiteral("Reading the backup…"), [this, archive](const QJsonObject &r) {
        if (!r.value("ok").toBool()) { finished(QStringLiteral("Cannot restore: ") + str(r, "error"), false); return; }
        const QJsonObject m = r.value("manifest").toObject(), c = m.value("contents").toObject(), mach = m.value("machine").toObject();
        const QString here = sysinfo::dmiClean(QStringLiteral("product_version")).value_or(QString());
        QString text = QStringLiteral("<b>%1</b><br>made %2 on %3 (%4, BIOS %5)<br><br>%6 user file(s) → ~/.config/centurion, ~/.config/ryzen-curve-optimizer<br>"
                                      "%7 system file(s) → presets, boot profiles, network-guard rules, NVIDIA profiles, calibration<br><br>"
                                      "Files in the backup replace the current ones; anything that is not in the backup stays. "
                                      "The current configuration is saved first (…-pre-restore.tar.gz). The system profile is not restored.")
                           .arg(QFileInfo(archive).fileName().toHtmlEscaped(), str(m, "created").left(16).replace(QLatin1Char('T'), QLatin1Char(' ')),
                                str(m, "host").toHtmlEscaped(), str(mach, "product").toHtmlEscaped(), str(mach, "bios").toHtmlEscaped())
                           .arg(num(c, "user_config")).arg(num(c, "system"));
        if (!str(mach, "product").isEmpty() && !here.isEmpty() && str(mach, "product") != here)
            text += QStringLiteral("<br><br><span style='color:%1'><b>This backup comes from another model (%2).</b> Firmware limits, "
                                   "curves and calibration from it may not suit this machine.</span>").arg(QLatin1String(theme::WARN), str(mach, "product").toHtmlEscaped());
        QMessageBox box(QMessageBox::Question, QStringLiteral("Restore configuration"), text, QMessageBox::Cancel, this);
        box.setTextFormat(Qt::RichText);
        box.addButton(QStringLiteral("Restore"), QMessageBox::AcceptRole);
        if (box.exec() == QMessageBox::Cancel) { status_->setText(QStringLiteral("Idle.")); return; }
        const bool hasRoot = num(c, "system") > 0;
        // 1. safety copy of what is here now, 2. root part (password; nothing changed if dismissed), 3. user part.
        run({{"op", "config_backup"}, {"dest_dir", QFileInfo(archive).absolutePath()}, {"include_profile", false},
             {"tag", "pre-restore"}, {"centurion_version", QStringLiteral(CENTURION_VERSION)}},
            false, QStringLiteral("Saving the current configuration…"), [this, archive, hasRoot](const QJsonObject &pre) {
                if (pre.value("ok").toBool()) log(QStringLiteral("current configuration saved as ") + QFileInfo(str(pre, "archive")).fileName());
                else if (str(pre, "error") != QLatin1String("nothing to back up yet")) {
                    finished(QStringLiteral("Restore stopped — the current configuration could not be saved first: ") + str(pre, "error"), false);
                    return;
                }
                auto userPart = [this, archive](qint64 rootFiles) {
                    run({{"op", "config_restore_user"}, {"archive", archive}}, false, QStringLiteral("Restoring user files…"),
                        [this, rootFiles](const QJsonObject &u) {
                            if (!u.value("ok").toBool()) { finished(QStringLiteral("Restoring the user files failed: ") + str(u, "error"), false); return; }
                            for (const QJsonValue &d : u.value("commands_dropped").toArray())
                                log(QStringLiteral("scene \"%1\": its Command step was not restored (re-enter it in the scene editor)").arg(d.toString()));
                            finished(QStringLiteral("Restored %1 user and %2 system file(s).").arg(num(u, "files")).arg(rootFiles), true);
                            QMessageBox ask(QMessageBox::Information, QStringLiteral("Configuration restored"),
                                            QStringLiteral("Centurion has to restart to load the restored configuration."),
                                            QMessageBox::NoButton, this);
                            ask.addButton(QStringLiteral("Later"), QMessageBox::RejectRole);
                            auto *now = ask.addButton(QStringLiteral("Restart now"), QMessageBox::AcceptRole);
                            ask.exec();
                            if (ask.clickedButton() == now) theme::requestRestart();
                        });
                };
                if (!hasRoot) { userPart(0); return; }
                run({{"op", "config_restore_root"}, {"archive", archive}}, true, QStringLiteral("Restoring system files (waiting for the password)…"),
                    [this, userPart](const QJsonObject &s) {
                        if (!s.value("ok").toBool()) { finished(QStringLiteral("Nothing was restored: ") + str(s, "error"), false); return; }
                        userPart(num(s, "files"));
                    });
            });
    });
}

// ── system image ────────────────────────────────────────────────────────────

void BackupPage::updateEncryptionUi() {
    const bool key = encMode_->currentData().toString() == QLatin1String("recipient");
    recipient_->setVisible(key);
    keyGen_->setVisible(key);
}

void BackupPage::systemBackup() {
    saveSettings();
    const QString dir = sysDir_->text().trimmed();
    if (!QDir::isAbsolutePath(dir) || !QFileInfo(dir).isDir()) { finished(QStringLiteral("The image folder does not exist: ") + dir, false); return; }
    QJsonArray ex;
    for (const QString &l : excludes_->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts))
        if (!l.trimmed().isEmpty()) ex.append(l.trimmed());
    const QString mode = encMode_->currentData().toString();
    const QString recipient = recipient_->text().trimmed();
    if (mode == QLatin1String("recipient") && !(recipient.startsWith(QLatin1String("age1")) && recipient.size() == 62)) {
        finished(tr("\"Public key\" needs an age public key (age1…, 62 characters): paste one, or make one with \"New key pair…\"."), false);
        return;
    }
    QJsonObject req{{"op", "system_backup"}, {"dest_dir", dir}, {"compressor", comp_->currentData().toString()}, {"level", level_->value()},
                    {"include_home", home_->isChecked()}, {"excludes", ex}, {"low_priority", low_->isChecked()},
                    {"verify", verify_->isChecked()}, {"keep", keep_->value()}, {"incremental", incr_->isChecked()}};
    if (mode == QLatin1String("passphrase")) {
        QString pass;
        if (!askNewPassphrase(&pass)) { status_->setText(tr("Image cancelled.")); return; }
        req["passphrase"] = pass;
    } else if (mode == QLatin1String("recipient")) {
        req["recipient"] = recipient;
    }
    log_->clear();
    if (mode == QLatin1String("recipient"))
        log(tr("Encrypted: keep the AGE-SECRET-KEY that goes with this key somewhere other than this computer. Without it the image cannot be read."), theme::WARN);
    else if (mode == QLatin1String("passphrase"))
        log(tr("Encrypted with a passphrase: without it the image cannot be read. Centurion does not keep it."), theme::WARN);
    run(req, true, QStringLiteral("Waiting for the administrator password…"), [this, mode](const QJsonObject &r) {
            for (const QJsonValue &m : r.value("messages").toArray()) log(m.toString(), theme::WARN);
            for (const QJsonValue &n : r.value("notes").toArray()) log(n.toString(), theme::MUTED);
            if (!r.value("ok").toBool()) { finished(QStringLiteral("System image failed: ") + str(r, "error"), false); return; }
            for (const QJsonValue &d : r.value("removed").toArray()) log(QStringLiteral("removed old image ") + d.toString());
            QString s = QStringLiteral("%1 written: %2, %L3 entries, %4").arg(QFileInfo(str(r, "archive")).fileName(), human(num(r, "bytes")))
                            .arg(num(r, "files")).arg(clock(num(r, "elapsed")));
            if (r.value("verified").toBool()) s += QStringLiteral(", verified");
            if (r.value("encrypted").toBool())
                s += mode == QLatin1String("recipient") && !r.value("verified").toBool() ? tr(", encrypted: verify it with its key") : tr(", encrypted");
            if (num(r, "level") > 0) s += tr(", increment %1").arg(num(r, "level"));
            if (r.value("degraded").toBool()) s += QStringLiteral(" — tar reported errors on some files (%1 message(s) below)").arg(num(r, "message_count"));
            else if (num(r, "message_count") > 0) s += QStringLiteral(" — %1 tar note(s), e.g. files that changed while being read").arg(num(r, "message_count"));
            if (r.value("verified").toBool()) rememberVerify(str(r, "archive"), true, QString());
            finished(s, !r.value("degraded").toBool());
            select(str(r, "archive"));
        });
}

bool BackupPage::askNewPassphrase(QString *pass) {
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Passphrase for the image"));
    auto *v = new QVBoxLayout(&dlg);
    auto *info = new QLabel(tr("The image is encrypted with this passphrase (age, scrypt). It is needed to verify or restore the image "
                               "and is not stored anywhere: <b>if it is lost, the image is lost</b>. An increment must use the "
                               "passphrase of its chain."));
    info->setWordWrap(true);
    v->addWidget(info);
    auto *a = new QLineEdit, *b = new QLineEdit;
    for (QLineEdit *e : {a, b}) { e->setEchoMode(QLineEdit::Password); e->setMinimumWidth(320); }
    a->setPlaceholderText(tr("passphrase"));
    b->setPlaceholderText(tr("again"));
    v->addWidget(a);
    v->addWidget(b);
    auto *show = new QCheckBox(tr("Show"));
    v->addWidget(show);
    auto *hint = new QLabel;
    hint->setProperty("role", "muted");
    v->addWidget(hint);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    v->addWidget(bb);
    connect(show, &QCheckBox::toggled, &dlg, [a, b](bool on) { for (QLineEdit *e : {a, b}) e->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password); });
    auto check = [a, b, hint, bb] {
        const QString p = a->text();
        QString h;
        bool ok = !p.isEmpty() && p == b->text() && !p.contains(QLatin1Char('\n'));
        if (p.isEmpty()) h = tr("Type it twice.");
        else if (p != b->text()) h = tr("The two do not match yet.");
        else if (p.size() < 12) h = tr("Short: anyone holding the file can try passphrases offline. Several words are better.");
        else h = tr("Matches.");
        hint->setText(h);
        bb->button(QDialogButtonBox::Ok)->setEnabled(ok);
    };
    connect(a, &QLineEdit::textChanged, &dlg, check);
    connect(b, &QLineEdit::textChanged, &dlg, check);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    check();
    const bool ok = dlg.exec() == QDialog::Accepted;
    if (ok) *pass = a->text();
    a->clear();
    b->clear();
    return ok;
}

// age-keygen as the user: the secret key into a file of the user's choice (0600), the public key into the field.
void BackupPage::newKeyPair() {
    QString keygen;
    for (const char *c : {"/usr/bin/age-keygen", "/bin/age-keygen"})
        if (QFileInfo(QString::fromLatin1(c)).isExecutable()) { keygen = QString::fromLatin1(c); break; }
    if (keygen.isEmpty()) { finished(tr("age-keygen was not found. Install age (app-crypt/age)."), false); return; }
    const QString file = QFileDialog::getSaveFileName(this, tr("Save the secret key — keep it off this computer"),
                                                      QDir::homePath() + QStringLiteral("/centurion-backup.agekey"),
                                                      tr("age key (*.agekey *.txt);;All files (*)"));
    if (file.isEmpty()) return;
    if (QFileInfo::exists(file)) { finished(tr("%1 exists; choose a new file name (an existing key is never overwritten).").arg(file), false); return; }
    // Asynchronous, so a hung age-keygen cannot freeze the window; a 15 s watchdog kills it.
    auto *p = new QProcess(this);
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;  // every other error is followed by finished()
        finished(tr("age-keygen failed: %1").arg(p->errorString()), false);
        p->deleteLater();
    });
    auto *watchdog = new QTimer(p);
    watchdog->setSingleShot(true);
    connect(watchdog, &QTimer::timeout, p, [p] { if (p->state() != QProcess::NotRunning) p->kill(); });
    connect(p, &QProcess::finished, this, [this, p, file](int code, QProcess::ExitStatus status) {
        const QString err = QString::fromUtf8(p->readAllStandardError()).trimmed();
        p->deleteLater();
        if (status != QProcess::NormalExit || code != 0) { finished(tr("age-keygen failed: %1").arg(err), false); return; }
        QFile::setPermissions(file, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QFile f(file);
        QString pub;
        if (f.open(QIODevice::ReadOnly))
            for (const QByteArray &l : f.readAll().split('\n'))
                if (l.startsWith("# public key: ")) pub = QString::fromLatin1(l.mid(14)).trimmed();
        if (pub.isEmpty()) { finished(tr("age-keygen wrote no public key."), false); return; }
        recipient_->setText(pub);
        saveSettings();
        log(tr("New key pair: the public key is filled in; the secret key is in %1 (only you can read it).").arg(file), theme::OK);
        QMessageBox::information(this, tr("New key pair"),
            tr("The secret key was saved to\n%1\n\nMove it (or a copy) somewhere other than this computer — a USB stick, a password manager. "
               "Restoring or verifying an encrypted image asks for it; without it the images cannot be read.").arg(file));
    });
    p->start(keygen, {QStringLiteral("-o"), file});
    watchdog->start(15000);
}

// Asks for what opens encrypted images: the sidecar says whether they were made with a passphrase or a key.
bool BackupPage::askSecret(const QStringList &archives, QJsonObject *req) {
    QString kind;  // "passphrase", "recipient" or empty (not recorded)
    for (const QString &a : archives) {
        const QJsonObject info = readJson(a + QStringLiteral(".info.json"));
        const QString k = str(info, "encryption").isEmpty() && !str(info, "recipient").isEmpty() ? QStringLiteral("recipient") : str(info, "encryption");
        if (!k.isEmpty()) { kind = k; break; }
    }
    const QString name = archives.size() == 1 ? QFileInfo(archives.first()).fileName() : tr("%n encrypted archive(s)", nullptr, int(archives.size()));
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Encrypted image"));
    auto *v = new QVBoxLayout(&dlg);
    auto *info = new QLabel(kind == QLatin1String("passphrase") ? tr("%1 is encrypted with a passphrase. Type it:").arg(name)
                          : kind == QLatin1String("recipient") ? tr("%1 is encrypted to an age key. Paste its AGE-SECRET-KEY-1… line or load the key file:").arg(name)
                                                               : tr("%1 is encrypted. Type its passphrase, or paste / load its age secret key:").arg(name));
    info->setWordWrap(true);
    v->addWidget(info);
    auto *edit = new QLineEdit;
    edit->setEchoMode(QLineEdit::Password);
    edit->setMinimumWidth(380);
    v->addWidget(edit);
    auto *row = new QHBoxLayout;
    auto *show = new QCheckBox(tr("Show"));
    row->addWidget(show);
    row->addStretch(1);
    auto *load = new QPushButton(tr("Load key file…"));
    load->setVisible(kind != QLatin1String("passphrase"));
    row->addWidget(load);
    v->addLayout(row);
    auto *note = new QLabel(tr("Used for this run only; Centurion does not store it."));
    note->setProperty("role", "muted");
    v->addWidget(note);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    v->addWidget(bb);
    QString keyFile;  // contents of a loaded key file
    connect(show, &QCheckBox::toggled, &dlg, [edit](bool on) { edit->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password); });
    connect(load, &QPushButton::clicked, &dlg, [&] {
        const QString f = QFileDialog::getOpenFileName(&dlg, tr("age secret key"), QDir::homePath(), tr("age key (*.agekey *.txt *.key);;All files (*)"));
        if (f.isEmpty()) return;
        QFile kf(f);
        const QByteArray body = kf.open(QIODevice::ReadOnly) && kf.size() < 65536 ? kf.readAll() : QByteArray();
        if (!body.contains("AGE-SECRET-KEY-1")) { note->setText(tr("%1 holds no AGE-SECRET-KEY line.").arg(QFileInfo(f).fileName())); return; }
        keyFile = QString::fromLatin1(body);
        edit->setText(tr("(key file %1)").arg(QFileInfo(f).fileName()));
        edit->setEnabled(false);
        show->setEnabled(false);
        note->setText(tr("Key loaded from %1.").arg(QFileInfo(f).fileName()));
    });
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return false;
    const QString typed = keyFile.isEmpty() ? edit->text() : keyFile;
    edit->clear();
    if (typed.trimmed().isEmpty()) return false;
    if (typed.contains(QLatin1String("AGE-SECRET-KEY-1"))) (*req)["identity"] = typed;
    else (*req)["passphrase"] = typed;
    return true;
}

// Two configuration archives → what changed from the older to the newer (backup::config_diff).
void BackupPage::compareConfigs() {
    QStringList two;
    for (const QString &p : selectedPaths()) if (isConfigName(QFileInfo(p).fileName())) two << p;
    if (two.size() != 2) {
        finished(tr("Select exactly two \"Centurion configuration\" archives (Ctrl+click) to compare."), false);
        return;
    }
    runDiff({{"op", "config_diff"}, {"a", two[0]}, {"b", two[1]}, {"profile", true}});
}

void BackupPage::compareWithCurrent() {
    QStringList one;
    for (const QString &p : selectedPaths()) if (isConfigName(QFileInfo(p).fileName())) one << p;
    if (one.size() != 1) {
        finished(tr("Select one \"Centurion configuration\" archive to compare with the configuration as it is now."), false);
        return;
    }
    runDiff({{"op", "config_diff"}, {"a", one[0]}, {"b", QStringLiteral("current")}, {"profile", true}});
}

void BackupPage::runDiff(const QJsonObject &req) {
    run(req, false, tr("Comparing the configurations…"), [this](const QJsonObject &r) {
        if (!r.value("ok").toBool()) { finished(tr("Comparison failed: ") + str(r, "error"), false); return; }
        const QJsonObject c = r.value("counts").toObject(), pk = r.value("packages").toObject().value("counts").toObject();
        QString summary = tr("%1 added, %2 removed, %3 changed, %4 unchanged.")
            .arg(num(c, "added")).arg(num(c, "removed")).arg(num(c, "changed")).arg(num(r, "same"));
        if (!pk.isEmpty())
            summary += tr(" Packages: %1 new, %2 removed, %3 other version.").arg(num(pk, "added")).arg(num(pk, "removed")).arg(num(pk, "changed"));
        finished(summary, true);
        showDiff(r);
    });
}

// The comparison as a tree: one branch per kind of change, files under it, keys / lines / packages under a file.
void BackupPage::showDiff(const QJsonObject &r) {
    const QJsonObject a = r.value("a").toObject(), b = r.value("b").toObject();
    const bool now = b.value(QLatin1String("current")).toBool();
    QDialog dlg(this);
    dlg.setWindowTitle(now ? tr("Configuration: backup → now") : tr("Configuration: older → newer backup"));
    dlg.resize(980, 640);
    auto *v = new QVBoxLayout(&dlg);
    auto *head = new QLabel(tr("<b>Before</b>: %1 (made %2)<br><b>After</b>: %3")
                                .arg(str(a, "name").toHtmlEscaped(), shortDate(str(a, "created")),
                                     now ? tr("the configuration as it is now") : tr("%1 (made %2)").arg(str(b, "name").toHtmlEscaped(), shortDate(str(b, "created")))));
    head->setTextFormat(Qt::RichText);
    v->addWidget(head);
    QStringList facts;
    if (r.value("swapped").toBool()) facts << tr("The archives were put in time order.");
    if (!r.value("profile_compared").toBool()) facts << tr("System profile not compared (not in both).");
    if (num(r, "ignored") > 0) facts << tr("%n log or per-boot file(s) not compared.", nullptr, int(num(r, "ignored")));
    if (r.value("truncated").toBool()) facts << tr("Long lists were cut short.");
    if (!facts.isEmpty()) {
        auto *f = new QLabel(facts.join(QLatin1Char(' ')));
        f->setProperty("role", "muted");
        f->setWordWrap(true);
        v->addWidget(f);
    }
    auto *filter = new QLineEdit;
    filter->setPlaceholderText(tr("Filter (file, key or package)…"));
    filter->setClearButtonEnabled(true);
    v->addWidget(filter);
    auto *tree = new QTreeWidget;
    tree->setColumnCount(3);
    tree->setHeaderLabels({tr("What"), tr("Before"), tr("After")});
    tree->setAlternatingRowColors(true);
    tree->setUniformRowHeights(true);
    tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    tree->header()->resizeSection(0, 460);
    tree->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    v->addWidget(tree, 1);
    const QColor ok(QLatin1String(theme::OK)), bad(QLatin1String(theme::DANGER)), warn(QLatin1String(theme::WARN)), muted(QLatin1String(theme::MUTED));
    auto branch = [&](const QString &title, int n) {
        auto *it = new QTreeWidgetItem(tree, {QStringLiteral("%1 (%2)").arg(title).arg(n)});
        QFont f = it->font(0);
        f.setBold(true);
        it->setFont(0, f);
        it->setExpanded(true);
        return it;
    };
    auto leaf = [](QTreeWidgetItem *parent, const QStringList &cols, const QColor &c) {
        auto *it = new QTreeWidgetItem(parent, cols);
        if (c.isValid()) for (int i = 0; i < cols.size(); ++i) it->setForeground(i, c);
        it->setToolTip(0, cols.value(0));
        it->setToolTip(1, cols.value(1));
        it->setToolTip(2, cols.value(2));
        return it;
    };
    const QJsonArray changed = r.value("changed").toArray(), added = r.value("added").toArray(), removed = r.value("removed").toArray(),
                     unread = r.value("unreadable").toArray();
    const QJsonObject c = r.value("counts").toObject();
    if (!changed.isEmpty()) {
        QTreeWidgetItem *br = branch(tr("Changed"), int(num(c, "changed")));
        for (const QJsonValue &cv : changed) {
            const QJsonObject o = cv.toObject();
            const QString kind = str(o, "kind");
            auto *file = leaf(br, {str(o, "path")}, warn);
            const QJsonArray keys = o.value(QLatin1String("keys")).toArray();
            for (const QJsonValue &k : keys) {
                const QJsonObject x = k.toObject();
                auto val = [](const QJsonValue &v) { return v.isNull() ? tr("(none)") : v.toString(); };
                leaf(file, {str(x, "key"), val(x.value(QLatin1String("old"))), val(x.value(QLatin1String("new")))}, QColor());
            }
            const QJsonObject lines = o.value(QLatin1String("lines")).toObject();
            for (const QJsonValue &l : lines.value(QLatin1String("removed")).toArray()) leaf(file, {QStringLiteral("− ") + l.toString()}, bad);
            for (const QJsonValue &l : lines.value(QLatin1String("added")).toArray()) leaf(file, {QStringLiteral("+ ") + l.toString()}, ok);
            if (num(lines, "more") > 0) leaf(file, {tr("… %n more line(s)", nullptr, int(num(lines, "more")))}, muted);
            if (o.value(QLatin1String("reordered")).toBool()) leaf(file, {tr("same lines, another order")}, muted);
            if (kind == QLatin1String("binary")) leaf(file, {tr("binary file: contents differ")}, muted);
            if (kind == QLatin1String("packages")) leaf(file, {tr("see \"Packages\" below")}, muted);
            if (!keys.isEmpty()) file->setText(0, file->text(0) + QStringLiteral("  ·  ") + tr("%n key(s)", nullptr, int(keys.size())));
            file->setExpanded(changed.size() <= 40);
        }
    }
    if (!added.isEmpty()) {
        QTreeWidgetItem *br = branch(tr("Added"), int(num(c, "added")));
        for (const QJsonValue &x : added) leaf(br, {x.toString()}, ok);
    }
    if (!removed.isEmpty()) {
        QTreeWidgetItem *br = branch(tr("Removed"), int(num(c, "removed")));
        for (const QJsonValue &x : removed) leaf(br, {x.toString()}, bad);
    }
    if (!unread.isEmpty()) {
        QTreeWidgetItem *br = branch(tr("Not readable now (not compared)"), int(unread.size()));
        for (const QJsonValue &x : unread) leaf(br, {x.toString()}, muted);
    }
    const QJsonObject pk = r.value("packages").toObject();
    if (!pk.isEmpty()) {
        const QJsonObject pc = pk.value("counts").toObject();
        QTreeWidgetItem *br = branch(tr("Packages"), int(num(pc, "added") + num(pc, "removed") + num(pc, "changed")));
        for (const QJsonValue &x : pk.value("changed").toArray()) {
            const QJsonObject o = x.toObject();
            leaf(br, {str(o, "name"), str(o, "old"), str(o, "new")}, warn);
        }
        for (const QJsonValue &x : pk.value("added").toArray()) leaf(br, {QStringLiteral("+ ") + x.toString()}, ok);
        for (const QJsonValue &x : pk.value("removed").toArray()) leaf(br, {QStringLiteral("− ") + x.toString()}, bad);
    }
    if (tree->topLevelItemCount() == 0) new QTreeWidgetItem(tree, {tr("No differences.")});
    // Filter: a row stays when it or anything below it matches; its branch stays open.
    connect(filter, &QLineEdit::textChanged, &dlg, [tree](const QString &t) {
        std::function<bool(QTreeWidgetItem *)> apply = [&](QTreeWidgetItem *it) {
            bool any = t.isEmpty() || it->text(0).contains(t, Qt::CaseInsensitive) || it->text(1).contains(t, Qt::CaseInsensitive)
                       || it->text(2).contains(t, Qt::CaseInsensitive);
            const bool self = any;
            for (int i = 0; i < it->childCount(); ++i) any |= apply(it->child(i));
            if (self && !t.isEmpty()) for (int i = 0; i < it->childCount(); ++i) it->child(i)->setHidden(false);
            it->setHidden(!any);
            return any;
        };
        for (int i = 0; i < tree->topLevelItemCount(); ++i) { apply(tree->topLevelItem(i)); tree->topLevelItem(i)->setHidden(false); }
    });
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *copy = bb->addButton(tr("Copy as text"), QDialogButtonBox::ActionRole);
    connect(copy, &QPushButton::clicked, &dlg, [tree, head] {
        QString out = head->text().replace(QStringLiteral("<br>"), QStringLiteral("\n")).remove(QRegularExpression(QStringLiteral("<[^>]*>"))) + QLatin1Char('\n');
        std::function<void(QTreeWidgetItem *, int)> walk = [&](QTreeWidgetItem *it, int depth) {
            if (it->isHidden()) return;
            QString line = QString(depth * 2, QLatin1Char(' ')) + it->text(0);
            if (!it->text(1).isEmpty() || !it->text(2).isEmpty()) line += QStringLiteral(": %1 → %2").arg(it->text(1), it->text(2));
            out += line + QLatin1Char('\n');
            for (int i = 0; i < it->childCount(); ++i) walk(it->child(i), depth + 1);
        };
        for (int i = 0; i < tree->topLevelItemCount(); ++i) walk(tree->topLevelItem(i), 0);
        QApplication::clipboard()->setText(out);
    });
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(bb);
    dlg.exec();
}

// {"op":"verify"} — one archive, or several (each read back on its own; one line per archive). Every result is
// remembered for the list (this user's state), failures in red.
void BackupPage::verifySelected() {
    const QStringList paths = selectedPaths();
    if (paths.isEmpty()) { finished(QStringLiteral("Select an archive in the list first."), false); return; }
    // A system image is 0600 root: reading it back needs the password too. Encrypted ones need their secret, asked once.
    bool root = false;
    QStringList encrypted;
    for (const QString &p : paths) {
        if (!QFileInfo(p).isReadable()) root = true;
        if (p.endsWith(QLatin1String(".age"))) encrypted << p;
    }
    QJsonObject req{{"op", "verify"}};
    if (!encrypted.isEmpty() && !askSecret(encrypted, &req)) { status_->setText(tr("Verify cancelled.")); return; }
    if (paths.size() == 1) {
        req["archive"] = paths.first();
    } else {
        QJsonArray list;
        for (const QString &p : paths) list.append(p);
        req["archives"] = list;
    }
    log_->clear();
    run(req, root, QStringLiteral("Verifying…"), [this, paths](const QJsonObject &r) {
        auto chainNote = [this](const QJsonObject &o) {
            if (!str(o, "chain_error").isEmpty())
                log(tr("%1: readable, but its image set is incomplete — %2").arg(QFileInfo(str(o, "archive")).fileName(), str(o, "chain_error")), theme::WARN);
        };
        // A run that never reached the archives (password dismissed, helper missing) says nothing about them.
        const bool reached = r.contains(QLatin1String("results")) || r.contains(QLatin1String("archive"));
        if (paths.size() > 1) {
            if (!r.contains(QLatin1String("results"))) { finished(tr("Verification failed: ") + str(r, "error"), false); return; }
            int good = 0;
            for (const QJsonValue &v : r.value("results").toArray()) {
                const QJsonObject o = v.toObject();
                const QString name = QFileInfo(str(o, "archive")).fileName();
                if (o.value("ok").toBool()) {
                    ++good;
                    log(tr("%1: readable end to end, %L2 entries").arg(name).arg(num(o, "files")), theme::OK);
                    chainNote(o);
                } else {
                    log(QStringLiteral("%1: %2").arg(name, str(o, "error")), theme::DANGER);
                }
                if (!str(o, "archive").isEmpty() && !o.value("needs_key").toBool()) rememberVerify(str(o, "archive"), o.value("ok").toBool(), str(o, "error"));
            }
            finished(tr("%1 of %2 archive(s) verified.").arg(good).arg(paths.size()), good == paths.size());
            return;
        }
        const QString archive = paths.first();
        if (!r.value("ok").toBool()) {
            if (reached && !r.value("needs_key").toBool() && !r.value("cancelled").toBool()) rememberVerify(archive, false, str(r, "error"));
            finished(QStringLiteral("%1 did NOT verify: %2").arg(QFileInfo(archive).fileName(), str(r, "error")), false);
            return;
        }
        rememberVerify(archive, true, QString());
        chainNote(r);
        finished(QStringLiteral("%1 is readable end to end: %L2 entries%3.").arg(QFileInfo(archive).fileName()).arg(num(r, "files"))
                     .arg(r.value("compared").toBool() ? QStringLiteral(", same count as when it was written") : QString()), true);
    });
}

void BackupPage::systemRestore() {
    saveSettings();
    QString kind;
    const QString archive = selected(&kind);
    if (archive.isEmpty() || kind != QLatin1String("system")) {
        finished(QStringLiteral("Select a \"System image\" in the list first (\"Add file…\" lists one from elsewhere)."), false);
        return;
    }
    const QString target = QFileInfo(target_->text().trimmed()).canonicalFilePath();
    if (target.isEmpty() || !QFileInfo(target).isDir()) {
        finished(QStringLiteral("\"Restore to\" must be an existing directory (the mounted new root, or /)."), false);
        return;
    }
    const QString name = QFileInfo(archive).fileName();
    const qint64 level = num(readJson(archive + QStringLiteral(".info.json")), "level");
    const QString chainText = level > 0
        ? tr("\n\nThis is increment %1: its full image and increments 1–%1 are unpacked in that order. What was deleted or renamed "
             "between them is deleted or renamed again — only among the files these images restored. Nothing else on the target "
             "is removed, and nothing across a mount point.").arg(level)
        : QString();
    if (target == QLatin1String("/")) {
        bool ok = false;
        const QString typed = QInputDialog::getText(this, QStringLiteral("Restore over the running system"),
            QStringLiteral("%1 will be unpacked over / — every file in the image replaces the one on this system, while it is running.\n"
                           "Files created after the image was made stay. Close other programs first and reboot afterwards.").arg(name)
                + chainText + QStringLiteral("\n\nType RESTORE to continue:"), QLineEdit::Normal, QString(), &ok);
        if (!ok || typed != QLatin1String("RESTORE")) { status_->setText(QStringLiteral("Restore cancelled.")); return; }
    } else {
        const int n = QDir(target).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).size();
        if (QMessageBox::warning(this, QStringLiteral("Restore system image"),
                QStringLiteral("Unpack %1 into %2?\n\n%3").arg(name, target, n ? QStringLiteral("The directory is not empty (%1 entries): files in the image replace existing ones.").arg(n)
                                                                                 : QStringLiteral("The directory is empty.")) + chainText,
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) { status_->setText(QStringLiteral("Restore cancelled.")); return; }
    }
    QJsonObject req{{"op", "system_restore"}, {"archive", archive}, {"target", target}, {"confirm", target}, {"low_priority", false}};
    if (archive.endsWith(QLatin1String(".age")) && !askSecret({archive}, &req)) { status_->setText(QStringLiteral("Restore cancelled.")); return; }
    log_->clear();
    run(req, true, QStringLiteral("Waiting for the administrator password…"), [this, target, name](const QJsonObject &r) {
            for (const QJsonValue &m : r.value("messages").toArray()) log(m.toString(), theme::WARN);
            if (!r.value("ok").toBool()) {
                if (r.value("needs_key").toBool()) {
                    finished(tr("Restore stopped before anything was written: an image of this set is encrypted. Select the newest "
                                "image again and restore; Centurion asks for its passphrase or key."), false);
                    return;
                }
                finished(QStringLiteral("Restore failed: ") + str(r, "error"), false);
                return;
            }
            if (num(r, "steps") > 1) {
                log(tr("restored the full image and %1 increment(s), in order").arg(num(r, "steps") - 1));
                log(tr("as recorded between the images: %1 deleted, %2 directory(ies) renamed").arg(num(r, "deleted")).arg(num(r, "renamed")));
            }
            for (const QJsonValue &n : r.value("notes").toArray()) log(n.toString(), theme::WARN);
            QStringList made;
            for (const QJsonValue &d : r.value("created_dirs").toArray()) made << d.toString();
            if (!made.isEmpty()) log(QStringLiteral("created missing mount points: ") + made.join(QLatin1Char(' ')));
            if (target != QLatin1String("/"))
                log(QStringLiteral("new disk? check the UUIDs in %1/etc/fstab and reinstall the boot loader before rebooting.").arg(target));
            QString s = QStringLiteral("%1 restored into %2: %L3 entries, %4").arg(name, target).arg(num(r, "files")).arg(clock(num(r, "elapsed")));
            if (r.value("degraded").toBool()) s += QStringLiteral(" — tar reported errors on some files (%1 message(s) below)").arg(num(r, "message_count"));
            finished(s, !r.value("degraded").toBool());
        });
}
