#include "firstrunwizard.h"
#include "backuppage.h"
#include "healthtab.h"
#include "lighting.h"
#include "mainwindow.h"
#include "mangohud.h"
#include "platformprofile.h"
#include "scenes.h"
#include "scenestab.h"
#include "shortcuts.h"
#include "sysinfo.h"
#include "theme.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <functional>

static const QString DEFAULT_SCENE = QStringLiteral("Default");
static const QString HANDBOOK_URL = QStringLiteral("https://github.com/arabcian/centurion/blob/main/docs/HANDBOOK.md");

static QSettings guiSettings() {
    return QSettings(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/centurion/gui.ini"),
                     QSettings::IniFormat);
}

static void markDone() { guiSettings().setValue(QStringLiteral("first_run/done"), true); }

/// A page whose settings take effect when Next / Finish is pressed (Cancel leaves them untouched).
class Page : public QWizardPage {
public:
    Page(const QString &title, const QString &subtitle) {
        setTitle(title);
        setSubTitle(subtitle);
        lay = new QVBoxLayout(this);
    }
    void initializePage() override { if (onShow) onShow(); }
    bool validatePage() override { if (onNext) onNext(); return true; }
    QVBoxLayout *lay;
    std::function<void()> onShow, onNext;
};

static QLabel *note(const QString &text, const char *color = nullptr) {
    auto *l = new QLabel(text);
    l->setWordWrap(true);
    l->setTextInteractionFlags(Qt::TextBrowserInteraction);
    l->setOpenExternalLinks(true);
    if (color) theme::setSheet(l, QStringLiteral("color:%1").arg(QLatin1String(color)));
    return l;
}

// ── 1. Hardware ──────────────────────────────────────────────────────────────
static QWizardPage *hardwarePage(MainWindow *win) {
    auto *p = new Page(QStringLiteral("Your machine"), QStringLiteral("What Centurion found. Tabs appear only for what is here."));
    QString why;
    const bool supported = sysinfo::supportedMachine(&why);
    p->lay->addWidget(note(sysinfo::systemInfo().value_or(QStringLiteral("Unknown model"))));
    if (!supported) p->lay->addWidget(note(QStringLiteral("Not a supported machine: ") + why, theme::DANGER));
    else if (!sysinfo::knownModel())
        p->lay->addWidget(note(QStringLiteral("Supported family, but this model was not tested yet — reports are welcome (Home → Device info…)."),
                               theme::WARN));
    else p->lay->addWidget(note(QStringLiteral("Supported model."), theme::OK));

    auto mod = [](const char *m) { return QFileInfo::exists(QStringLiteral("/sys/module/") + QLatin1String(m)); };
    const QList<QPair<QString, bool>> found{
        {QStringLiteral("lenovo-wmi-gamezone (power profiles, fans)"), mod("lenovo_wmi_gamezone")},
        {QStringLiteral("lenovo-wmi-other (firmware attributes)"), mod("lenovo_wmi_other")},
        {QStringLiteral("ideapad_laptop (battery, device toggles)"), mod("ideapad_laptop")},
        {QStringLiteral("Spectrum per-key keyboard"), lighting::present()},
        {QStringLiteral("4-zone RGB keyboard"), lighting::zone4Present()},
        {QStringLiteral("NVIDIA GPU"), win->nvidia() != nullptr},
        {QStringLiteral("AMD GPU (amdgpu)"), win->amdgpu() != nullptr},
    };
    QString rows;
    for (const auto &[what, ok] : found)
        rows += QStringLiteral("<tr><td style='color:%1'>%2</td><td>%3</td></tr>")
                    .arg(QLatin1String(ok ? theme::OK : theme::MUTED), ok ? QStringLiteral("✓") : QStringLiteral("–"), what.toHtmlEscaped());
    p->lay->addWidget(note(QStringLiteral("<table cellspacing=4>") + rows + QStringLiteral("</table>")));
    p->lay->addStretch();
    return p;
}

// ── 2. Boot services ─────────────────────────────────────────────────────────
struct Service { QString name, runlevel, role; bool recommended; };

static QWizardPage *servicesPage(MainWindow *win) {
    auto *p = new Page(QStringLiteral("Boot services"),
                       QStringLiteral("Optional. Each one does nothing until you set its default or preset in the app."));
    QList<Service> list{{QStringLiteral("centurion-boot-guard"), QStringLiteral("boot"),
                         QStringLiteral("records clean shutdowns; pauses the others after a crash"), true}};
    if (win->nvidia()) list.append({QStringLiteral("nvcurve-autoload"), QStringLiteral("default"), QStringLiteral("★ default GPU curve at boot"), false});
    list.append({QStringLiteral("centurion-tune"), QStringLiteral("boot"), QStringLiteral("⏻ Optimizations boot preset"), false});
    if (win->intel()) list.append({QStringLiteral("centurion-intel-uv"), QStringLiteral("boot"), QStringLiteral("Intel undervolt at boot / resume"), false});
    if (!QStandardPaths::findExecutable(QStringLiteral("nft"), {"/usr/sbin", "/sbin", "/usr/bin", "/bin"}).isEmpty())
        list.append({QStringLiteral("centurion-netguard"), QStringLiteral("default"), QStringLiteral("network guard (Health → Network)"), false});

    const bool systemd = QFileInfo::exists(QStringLiteral("/run/systemd/system"));
    const bool openrc = !systemd && (QFileInfo::exists(QStringLiteral("/sbin/rc-update")) || QFileInfo::exists(QStringLiteral("/usr/sbin/rc-update"))
                                     || QFileInfo::exists(QStringLiteral("/usr/bin/rc-update")));
    if (!systemd && !openrc) {
        p->lay->addWidget(note(QStringLiteral("Neither systemd nor OpenRC was found: see README → Boot services."), theme::WARN));
        p->lay->addStretch();
        return p;
    }
    // Already enabled: asked per unit (older systemd prints nothing on stdout for a unit it does not
    // know, which would shift a combined answer); OpenRC keeps a link per runlevel.
    QSet<QString> enabled;
    if (systemd) {
        for (const Service &s : list) {
            QProcess pr;
            pr.start(QStringLiteral("systemctl"), {QStringLiteral("is-enabled"), s.name + QStringLiteral(".service")});
            if (pr.waitForFinished(2000) && pr.readAllStandardOutput().trimmed() == "enabled") enabled.insert(s.name);
        }
    } else {
        for (const Service &s : list)
            for (const QString &rl : QDir(QStringLiteral("/etc/runlevels")).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
                if (QFileInfo::exists(QStringLiteral("/etc/runlevels/%1/%2").arg(rl, s.name))) enabled.insert(s.name);
    }

    p->lay->addWidget(note(QStringLiteral("Enabling a service needs root. Tick what you want, then run the command below in a terminal "
                                          "(%1).").arg(systemd ? QStringLiteral("systemd") : QStringLiteral("OpenRC"))));
    auto *cmd = new QPlainTextEdit;
    cmd->setReadOnly(true);
    cmd->setMaximumHeight(110);
    auto boxes = std::make_shared<QList<QPair<QCheckBox *, Service>>>();
    for (const Service &s : list) {
        const bool on = enabled.contains(s.name);
        auto *b = new QCheckBox(QStringLiteral("%1 — %2%3").arg(s.name, s.role, on ? QStringLiteral("  (already enabled)")
                                                                              : s.recommended ? QStringLiteral("  (recommended)") : QString()));
        b->setChecked(s.recommended && !on);
        b->setEnabled(!on);
        boxes->append({b, s});
        p->lay->addWidget(b);
    }
    auto update = [cmd, boxes, systemd] {
        QStringList lines, units;
        bool netguard = false;
        for (const auto &[box, s] : *boxes) {
            if (!box->isChecked() || !box->isEnabled()) continue;
            if (!systemd) lines << QStringLiteral("sudo rc-update add %1 %2").arg(s.name, s.runlevel);
            else if (s.name == QLatin1String("centurion-netguard")) netguard = true;
            else units << s.name + QStringLiteral(".service");
        }
        if (!units.isEmpty()) lines.prepend(QStringLiteral("sudo systemctl enable ") + units.join(' '));
        if (netguard) lines << QStringLiteral("sudo systemctl enable --now centurion-netguard.service");
        cmd->setPlainText(lines.join('\n'));
    };
    for (const auto &[box, s] : *boxes) QObject::connect(box, &QCheckBox::toggled, cmd, update);
    update();
    p->lay->addWidget(cmd);
    auto *copy = new QPushButton(QStringLiteral("Copy command"));
    QObject::connect(copy, &QPushButton::clicked, cmd, [cmd] { QApplication::clipboard()->setText(cmd->toPlainText()); });
    QObject::connect(cmd, &QPlainTextEdit::textChanged, copy, [cmd, copy] { copy->setEnabled(!cmd->toPlainText().isEmpty()); });
    copy->setEnabled(!cmd->toPlainText().isEmpty());
    p->lay->addWidget(copy, 0, Qt::AlignLeft);
    p->lay->addStretch();
    return p;
}

// ── 3. First backup ──────────────────────────────────────────────────────────
static QWizardPage *backupPage(MainWindow *win) {
    auto *p = new Page(QStringLiteral("First backup"),
                       QStringLiteral("A copy of everything Centurion stores, before you change anything. No root needed."));
    p->lay->addWidget(note(QStringLiteral("Saved to the folder set in Health → Backup (default ~/Backups/centurion). "
                                          "Automatic backups can be turned on there too.")));
    auto *btn = new QPushButton(QStringLiteral("Back up the configuration now"));
    auto *status = note(QString());
    BackupPage *bp = win->health()->backup();
    QObject::connect(btn, &QPushButton::clicked, p, [btn, status, bp] {
        btn->setEnabled(false);
        status->setText(QStringLiteral("Saving…"));
        theme::setSheet(status, QString());
        bp->configBackup();
    });
    QObject::connect(bp, &BackupPage::jobFinished, p, [btn, status](const QString &summary, bool good) {
        btn->setEnabled(true);
        status->setText(summary);
        theme::setSheet(status, QStringLiteral("color:%1").arg(QLatin1String(good ? theme::OK : theme::DANGER)));
    });
    p->lay->addWidget(btn, 0, Qt::AlignLeft);
    p->lay->addWidget(status);
    p->lay->addStretch();
    return p;
}

// ── 4. First scene ───────────────────────────────────────────────────────────
static QWizardPage *scenePage(MainWindow *win) {
    auto *p = new Page(QStringLiteral("First scene"),
                       QStringLiteral("A scene is the whole machine under one name: a restore point you can go back to in one click."));
    auto *btn = new QPushButton(QStringLiteral("Capture current as '%1'").arg(DEFAULT_SCENE));
    auto *status = note(QString());
    auto *autoBox = new QCheckBox(QStringLiteral("Switch scenes automatically with the power source"));
    auto *onAc = new QComboBox, *onBat = new QComboBox;
    auto fill = [onAc, onBat] {
        for (QComboBox *c : {onAc, onBat}) {
            c->clear();
            c->addItem(QStringLiteral("(leave as is)"), QString());
            for (const QString &n : scenes::names()) c->addItem(n, n);
        }
    };
    fill();
    auto syncAuto = [autoBox, onAc, onBat] {
        const bool any = onAc->count() > 1;
        autoBox->setEnabled(any);
        onAc->setEnabled(any && autoBox->isChecked());
        onBat->setEnabled(any && autoBox->isChecked());
    };
    if (scenes::load(DEFAULT_SCENE)) {
        btn->setEnabled(false);
        status->setText(QStringLiteral("A scene named '%1' exists already.").arg(DEFAULT_SCENE));
    }
    QObject::connect(btn, &QPushButton::clicked, p, [win, btn, status, fill, syncAuto, onAc] {
        // Same snapshot as Scenes → Capture current: power profile and the writable firmware limits
        // (those need Custom). The fan curve / fan boost need a helper call: Scenes tab, Capture current.
        scenes::Scene s;
        s.name = DEFAULT_SCENE;
        bool wmiUnread = false;
        s.firmware = ScenesTab::firmwareSnapshot(win, &wmiUnread);
        const auto h = pp::primaryHandler();
        if (!s.firmware.isEmpty() && h && h->supportsCustom()) s.platformProfile = QStringLiteral("custom");
        else s.platformProfile = pp::currentProfile(h).value_or(QString());
        QString err;
        if (!scenes::save(s, &err)) {
            status->setText(QStringLiteral("Could not save the scene: ") + err);
            theme::setSheet(status, QStringLiteral("color:%1").arg(QLatin1String(theme::DANGER)));
            return;
        }
        btn->setEnabled(false);
        status->setText(QStringLiteral("Saved '%1': power profile %2, %3 firmware value(s).%4 Fan curve and fan boost: Scenes tab → Capture current.")
                            .arg(DEFAULT_SCENE, s.platformProfile.isEmpty() ? QStringLiteral("unchanged") : s.platformProfile)
                            .arg(s.firmware.size())
                            .arg(wmiUnread ? QStringLiteral(" GPU cTGP / boost were not read yet (open the Firmware tab once).") : QString()));
        theme::setSheet(status, QStringLiteral("color:%1").arg(QLatin1String(theme::OK)));
        fill();
        onAc->setCurrentIndex(onAc->findData(DEFAULT_SCENE));
        syncAuto();
    });
    QObject::connect(autoBox, &QCheckBox::toggled, p, syncAuto);
    syncAuto();
    p->lay->addWidget(btn, 0, Qt::AlignLeft);
    p->lay->addWidget(status);
    p->lay->addSpacing(8);
    p->lay->addWidget(autoBox);
    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("On AC power"), onAc);
    form->addRow(QStringLiteral("On battery"), onBat);
    p->lay->addLayout(form);
    p->lay->addStretch();
    p->onNext = [win, autoBox, onAc, onBat] {
        if (!autoBox->isEnabled() || !autoBox->isChecked()) return;
        scenes::Auto a = win->scenes()->autoConfig();
        a.enabled = true;
        a.onAc = onAc->currentData().toString();
        a.onBattery = onBat->currentData().toString();
        win->scenes()->setAuto(a);
    };
    return p;
}

// ── 5. Done ──────────────────────────────────────────────────────────────────
static QWizardPage *finishPage(MainWindow *win) {
    auto *p = new Page(QStringLiteral("Extras"), QStringLiteral("All optional; each can be changed later."));
    // Shortcut for the first scene (desktop global-shortcuts portal, like Scenes → Shortcut).
    auto *keys = new QKeySequenceEdit;
    auto *keysRow = new QWidget;
    auto *kf = new QFormLayout(keysRow);
    kf->setContentsMargins(0, 0, 0, 0);
    kf->addRow(QStringLiteral("Shortcut for '%1'").arg(DEFAULT_SCENE), keys);
    p->lay->addWidget(keysRow);
    p->lay->addWidget(note(QStringLiteral("Every scene can have one: Scenes tab → Shortcut. It is registered with the desktop "
                                          "(global-shortcuts portal), which may ask you to confirm it."), theme::MUTED));
    QCheckBox *mango = nullptr;
    if (mangohud::available()) {
        mango = new QCheckBox(QStringLiteral("Show the active scene and power profile in games (MangoHud)"));
        mango->setChecked(mangohud::enabled());
        p->lay->addWidget(mango);
    }
    p->lay->addSpacing(8);
    p->lay->addWidget(note(QStringLiteral("Scripts, KDE Connect, Home Assistant or a Stream Deck can switch scenes and power profiles "
                                          "and read the sensors over D-Bus: <code>com.centurion.Centurion</code> (README → D-Bus API).")));
    p->lay->addWidget(note(QStringLiteral("Next: the <a href='%1'>Handbook</a>, task by task. This wizard again: Home → Setup wizard…")
                               .arg(HANDBOOK_URL)));
    p->lay->addStretch();
    // The shortcut row only makes sense once the scene exists (page 4 may have just made it).
    p->onShow = [keysRow] { keysRow->setVisible(scenes::load(DEFAULT_SCENE).has_value()); };
    p->onNext = [win, keys, mango] {
        if (mango && mango->isChecked() != mangohud::enabled() && mangohud::setEnabled(mango->isChecked())) win->updateOverlay();
        const QString seq = keys->keySequence().toString(QKeySequence::PortableText);
        if (seq.isEmpty()) return;
        auto s = scenes::load(DEFAULT_SCENE);
        if (!s) return;
        s->shortcut = seq;
        if (scenes::save(*s)) win->shortcuts()->sync();
    };
    return p;
}

FirstRunWizard::FirstRunWizard(MainWindow *win) : QWizard(win) {
    setWindowTitle(QStringLiteral("Centurion setup"));
    setAttribute(Qt::WA_DeleteOnClose);
    setOption(QWizard::NoBackButtonOnStartPage);
    setButtonText(QWizard::CancelButton, QStringLiteral("Skip setup"));
    addPage(hardwarePage(win));
    addPage(servicesPage(win));
    addPage(backupPage(win));
    addPage(scenePage(win));
    addPage(finishPage(win));
    // Finished or skipped: either way, not shown again by itself.
    connect(this, &QDialog::finished, this, [] { markDone(); });
    resize(640, 480);
}

void FirstRunWizard::maybeShow(MainWindow *win) {
    if (qEnvironmentVariableIsSet("CENTURION_PGO_TRAIN") || qEnvironmentVariableIsSet("CENTURION_SCREENSHOT")) return;
    if (guiSettings().value(QStringLiteral("first_run/done")).toBool()) return;
    // An install from before the wizard (it already has scenes): nothing to set up.
    if (!scenes::names().isEmpty()) { markDone(); return; }
    showFor(win);
}

void FirstRunWizard::showFor(MainWindow *win) {
    if (FirstRunWizard *open = win->findChild<FirstRunWizard *>()) { open->raise(); open->activateWindow(); return; }
    (new FirstRunWizard(win))->show();
}
