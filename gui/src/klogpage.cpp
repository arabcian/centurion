#include "klogpage.h"

#include "diagsources.h"
#include "eventinfo.h"
#include "klogview.h"
#include "privileged.h"
#include "theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QIntValidator>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QDialog>
#include <QDialogButtonBox>
#include <QScrollBar>
#include <QSettings>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <optional>
#include <unistd.h>
#include <utility>

static constexpr int DEBOUNCE_MS = 3000;
static constexpr qint64 LOG_MAX_BYTES = 16 << 20;  // then kernel.log -> kernel.log.1
static constexpr int MAX_SHOWN_LINES = 6000;
static constexpr qint64 TAIL_BYTES = 512 << 10;
static constexpr int CONTEXT_BEFORE = 50;          // lines before a record opened from Monitor
static constexpr int CONTEXT_AFTER = 20;           // and after it

static QString helperPath() { return privileged::helperPath(QStringLiteral("tune-profile-helper")); }

static const char *const LEVELS[] = {"EMERG", "ALERT", "CRIT", "ERR", "WARN"};
// The level filter's choices, in order: the highest level index still shown ("… and worse").
static constexpr int LEVEL_MAX[] = {4, 3, 2, 1, 0};

static QString guiIniPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/centurion/gui.ini");
}

KlogPage::KlogPage(QWidget *parent) : QWidget(parent) {
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(8, 6, 8, 6);

    auto *top = new QHBoxLayout;
    auto *about = new QLabel(QStringLiteral("Kernel messages of level warning or worse, saved across reboots "
                                            "(the kernel's own ring buffer is lost at every boot)."));
    about->setWordWrap(true);
    about->setProperty("role", "muted");
    v->addWidget(about);
    scope_ = new QComboBox;
    scope_->addItems({QStringLiteral("This boot"), QStringLiteral("All saved boots")});
    top->addWidget(scope_);
    filter_ = new QLineEdit;
    filter_->setPlaceholderText(tr("Filter… (a|b = either)"));
    filter_->setToolTip(tr("Shows lines containing the text (case-insensitive). Separate alternatives with |, e.g. nvme|ata|EXT4."));
    filter_->setClearButtonEnabled(true);
    top->addWidget(filter_, 1);
    auto *bPresets = new QPushButton(tr("Quick filters ▾"));
    auto *presets = new QMenu(bPresets);
    connect(presets, &QMenu::aboutToShow, this, [this, presets] { fillPresets(presets); });
    bPresets->setMenu(presets);
    top->addWidget(bPresets);
    auto *bRefresh = new QPushButton(QStringLiteral("Refresh"));
    auto *bCopy = new QPushButton(QStringLiteral("Copy"));
    auto *bOpen = new QPushButton(QStringLiteral("Open saved log"));
    auto *bSave = new QPushButton(tr("Save view…"));
    bSave->setToolTip(tr("Save the lines shown now (with the current filters) to a text file, e.g. for a bug report."));
    auto *bClear = new QPushButton(QStringLiteral("Clear saved log"));
    bClear->setObjectName(QStringLiteral("btnDanger"));
    top->addWidget(bRefresh);
    v->addLayout(top);
    auto *exports = new QHBoxLayout;
    follow_ = new QCheckBox(tr("Follow latest"));
    follow_->setChecked(true);
    follow_->setToolTip(tr("Keep following new entries when the view is at the bottom. Disable to hold the current scroll position."));
    exports->addWidget(follow_);
    exports->addStretch(1);
    for (auto *b : {bCopy, bSave, bOpen, bClear}) exports->addWidget(b);
    v->addLayout(exports);
    connect(bSave, &QPushButton::clicked, this, &KlogPage::saveView);

    auto *opts = new QHBoxLayout;
    level_ = new QComboBox;
    level_->addItems({tr("WARN and worse"), tr("ERR and worse"), tr("CRIT and worse"), tr("ALERT and worse"), tr("EMERG only")});
    group_ = new QCheckBox(tr("Group repeats"));
    group_->setChecked(true);
    group_->setToolTip(tr("Messages that differ only in numbers (counters, addresses, process ids) become one line with a count."));
    onlyNew_ = new QCheckBox(tr("Only new"));
    onlyNew_->setToolTip(tr("Messages that were not in the saved log of an earlier boot (new after an update or a change)."));
    showMuted_ = new QCheckBox(tr("Show muted"));
    opts->addWidget(level_);
    opts->addWidget(group_);
    opts->addWidget(onlyNew_);
    opts->addWidget(showMuted_);
    opts->addStretch(1);
    v->addLayout(opts);
    auto *typedPanel=new QWidget(this);
    auto *typed = new QFormLayout(typedPanel);
    typed->setContentsMargins(0,0,0,0);
    auto *identity = new QHBoxLayout;
    bootFilter_ = new QLineEdit; bootFilter_->setPlaceholderText(tr("Exact boot UUID or legacy tag")); bootFilter_->setMaxLength(36);
    deviceFilter_ = new QLineEdit; deviceFilter_->setPlaceholderText(tr("Device or driver token")); deviceFilter_->setMaxLength(128);
    identity->addWidget(bootFilter_,1); identity->addWidget(deviceFilter_,1);
    typed->addRow(tr("Identity"),identity);
    auto *event = new QHBoxLayout;
    kind_ = new QComboBox;
    kind_->addItem(tr("All event types"),QString());
    for (const auto *key : {"xid","gsp","mce","aer","lockup","d3cold","amdgpu","nvme","fs","igpu","wifi","thermal","oom","oops"})
        kind_->addItem(QString::fromLatin1(key).toUpper(),QString::fromLatin1(key));
    kind_->addItem(tr("Unclassified"),QStringLiteral("unclassified"));
    codeFilter_=new QLineEdit; codeFilter_->setPlaceholderText(tr("Event code (optional)"));
    codeFilter_->setValidator(new QIntValidator(0,2147483647,codeFilter_));
    event->addWidget(kind_,1); event->addWidget(codeFilter_,1); typed->addRow(tr("Event"),event);
    auto *dates = new QHBoxLayout;
    fromFilter_=new QLineEdit; fromFilter_->setPlaceholderText(tr("From date (YYYY-MM-DD)")); fromFilter_->setMaxLength(10);
    toFilter_=new QLineEdit; toFilter_->setPlaceholderText(tr("To date (YYYY-MM-DD)")); toFilter_->setMaxLength(10);
    dates->addWidget(fromFilter_,1); dates->addWidget(toFilter_,1); typed->addRow(tr("Recorded local date"),dates);
    auto *typedToggle=new QPushButton(tr("Advanced filters")); typedToggle->setCheckable(true);
    typedToggle->setChecked(false); typedPanel->hide();
    connect(typedToggle,&QPushButton::toggled,typedPanel,&QWidget::setVisible);
    v->addWidget(typedToggle,0,Qt::AlignLeft); v->addWidget(typedPanel);
    filterInfo_=new QLabel; filterInfo_->setWordWrap(true); filterInfo_->setProperty("role","muted"); v->addWidget(filterInfo_);
    auto *filterDelay=new QTimer(this); filterDelay->setSingleShot(true); filterDelay->setInterval(250);
    connect(filterDelay,&QTimer::timeout,this,&KlogPage::render);
    for(auto *edit : {filter_,bootFilter_,deviceFilter_,codeFilter_,fromFilter_,toFilter_})
        connect(edit,&QLineEdit::textChanged,filterDelay,[filterDelay]{filterDelay->start();});
    connect(kind_,&QComboBox::currentIndexChanged,this,&KlogPage::render);

    focusBar_ = new QWidget;
    auto *fb = new QHBoxLayout(focusBar_);
    fb->setContentsMargins(0, 0, 0, 0);
    focusText_ = new QLabel;
    focusText_->setProperty("role", "muted");
    focusText_->setWordWrap(true);
    fb->addWidget(focusText_, 1);
    auto *bWhole = new QPushButton(tr("Show the whole log"));
    connect(bWhole, &QPushButton::clicked, this, [this] { focusBoot_.clear(); render(); });
    fb->addWidget(bWhole);
    focusBar_->setVisible(false);
    v->addWidget(focusBar_);

    text_ = new QTextBrowser;
    text_->setLineWrapMode(QTextEdit::NoWrap);
    text_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(text_, &QWidget::customContextMenuRequested, this, &KlogPage::contextMenu);
    text_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    v->addWidget(text_, 1);
    info_ = new QLabel;
    info_->setWordWrap(true);
    info_->setProperty("role", "muted");
    v->addWidget(info_);

    connect(scope_, &QComboBox::currentIndexChanged, this, &KlogPage::render);
    connect(level_, &QComboBox::currentIndexChanged, this, &KlogPage::render);
    for (QCheckBox *c : {group_, onlyNew_, showMuted_}) connect(c, &QCheckBox::toggled, this, &KlogPage::render);
    connect(bRefresh, &QPushButton::clicked, this, &KlogPage::capture);
    connect(bCopy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(text_->toPlainText()); });
    connect(bOpen, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl::fromLocalFile(logPath())); });
    connect(bClear, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, QStringLiteral("Clear saved log"),
                QStringLiteral("Delete the saved kernel log of every boot?")) != QMessageBox::Yes) return;
        const bool cleared = diagsrc::clearSavedLog(diagsrc::SavedLog::Kernel);
        // Keep savedSeq_: what this boot already showed is not written again.
        render();
        if (!cleared) info_->setText(tr("Could not clear all saved log files."));
    });

    {
        QSettings st(guiIniPath(), QSettings::IniFormat);
        for (const QString &k : st.value(QStringLiteral("klog/muted")).toStringList()) muted_.insert(k);
    }

    QFile b(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    if (b.open(QIODevice::ReadOnly)) bootId_ = QString::fromLatin1(b.readAll().trimmed()).toLower();

    // Resume point: highest seq of this boot in the tail of the file, so a
    // restart of the app does not write the same records twice.
    {
        // Prefix-only rows are deliberately not used as a cursor for a full UUID.
        if (klogview::fullBootId(bootId_)) {
            diagsrc::scanSavedLog(diagsrc::SavedLog::Kernel, TAIL_BYTES, [this](const QString &record, bool) {
                const auto line = klogview::parse(record);
                if (line && line->boot == bootId_ && line->seq <= quint64(std::numeric_limits<qint64>::max()))
                    savedSeq_ = std::max(savedSeq_, line->seq);
                return line.has_value();
            });
        }
        lastSeq_ = savedSeq_;
    }

    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(DEBOUNCE_MS);
    connect(debounce_, &QTimer::timeout, this, [this] { if (!needsRoot_) capture(); });

    // SEEK_END: the start-up capture below reads the history; the notifier
    // only wakes for new records.
    kmsgFd_ = ::open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (kmsgFd_ >= 0) {
        ::lseek(kmsgFd_, 0, SEEK_END);
        auto *n = new QSocketNotifier(kmsgFd_, QSocketNotifier::Read, this);
        auto rate = std::make_shared<std::pair<QElapsedTimer, int>>();
        rate->first.start();
        connect(n, &QSocketNotifier::activated, this, [this, n, rate] {
            drainKmsg();
            if (rate->first.elapsed() > 1000) { rate->first.restart(); rate->second = 0; }
            if (++rate->second > 50) {  // flood: pause, capture once, resume later
                n->setEnabled(false);
                debounce_->start();
                QTimer::singleShot(5000, n, [this, n, rate] { drainKmsg(); rate->first.restart(); rate->second = 0; n->setEnabled(true); });
            }
        });
        QTimer::singleShot(4000, this, [this] { if (!needsRoot_) capture(); });  // history of this boot, once
    } else {
        needsRoot_ = true;  // restricted: read on demand only (a pkexec prompt is never a background event)
    }
    updateInfo();
    render();
}

KlogPage::~KlogPage() { if (kmsgFd_ >= 0) ::close(kmsgFd_); }

void KlogPage::showAt(const QString &bootId, quint64 seq) {
    // Use the exact recorded identity; never silently attach a legacy prefix to a UUID.
    scope_->setCurrentIndex(1);
    filter_->clear();
    focusBoot_ = bootId.toLower();
    focusSeq_ = seq;
    render();
}

void KlogPage::showBoot(const QString &bootId) {
    focusBoot_.clear(); scope_->setCurrentIndex(1);
    filter_->clear(); deviceFilter_->clear(); codeFilter_->clear(); fromFilter_->clear(); toFilter_->clear();
    kind_->setCurrentIndex(0); level_->setCurrentIndex(0); onlyNew_->setChecked(false); showMuted_->setChecked(true);
    bootFilter_->setText(bootId.toLower()); render();
}

void KlogPage::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    if (!shownOnce_) { shownOnce_ = true; if (needsRoot_) capture(); }
    render();
}

void KlogPage::drainKmsg() {
    char buf[8192];
    bool relevant = false;
    for (int i = 0; i < 512; ++i) {  // bounded: a flood must not pin the GUI thread
        const ssize_t n = ::read(kmsgFd_, buf, sizeof buf - 1);
        if (n < 0 && errno == EPIPE) continue;
        if (n <= 0) break;
        if (relevant) continue;
        buf[n] = 0;
        char *end = nullptr;
        const long pri = std::strtol(buf, &end, 10);
        if (end != buf && (pri & 7) <= 4) relevant = true;
    }
    if (relevant) debounce_->start();
}

void KlogPage::capture() {
    if (busy_) {
        if (!needsRoot_) debounce_->start();
        return;
    }
    if (!QFileInfo(helperPath()).isExecutable()) { info_->setText(QStringLiteral("tune-helper is not installed.")); return; }
    busy_ = true;
    const QJsonObject req{{QStringLiteral("op"), QStringLiteral("klog")}, {QStringLiteral("since"), qint64(lastSeq_)}};
    if (needsRoot_) {
        privileged::run(helperPath(), req, this, [this](const privileged::Result &r) {
            busy_ = false;
            if (!r.reached) { info_->setText(QStringLiteral("Kernel log restricted and pkexec failed: ") + r.message()); return; }
            onReply(r.json, true);
        });
        return;
    }
    auto *p = new QProcess(this);
    QPointer<QProcess> guard(p);
    connect(p, &QProcess::finished, this, [this, p](int exitCode, QProcess::ExitStatus status) {
        busy_ = false;
        const QByteArray out = p->readAllStandardOutput().trimmed();
        p->deleteLater();
        const auto reply = QJsonDocument::fromJson(out.mid(out.lastIndexOf('\n') + 1)).object();
        if (status != QProcess::NormalExit || !reply.value(QStringLiteral("ok")).isBool()
            || (exitCode != 0 && reply.value(QStringLiteral("ok")).toBool())) {
            info_->setText(tr("Kernel log capture failed or timed out."));
            return;
        }
        onReply(reply, false);
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        busy_ = false;
        info_->setText(tr("Kernel log helper could not start."));
        p->deleteLater();
    });
    QTimer::singleShot(10000, p, [guard] { if (guard && guard->state() != QProcess::NotRunning) guard->kill(); });
    p->start(helperPath(), {});
    p->write(QJsonDocument(req).toJson(QJsonDocument::Compact));
    p->closeWriteChannel();
}

void KlogPage::onReply(const QJsonObject &r, bool viaRoot) {
    if (!r.value(QStringLiteral("ok")).toBool()) {
        if (r.value(QStringLiteral("needs_root")).toBool() && !needsRoot_) { needsRoot_ = true; updateInfo(); return; }
        info_->setText(QStringLiteral("Capture failed: ") + r.value(QStringLiteral("error")).toString());
        return;
    }
    const auto replyBoot=r.value(QStringLiteral("boot_id")).toString().toLower();
    const auto seq=r.value(QStringLiteral("last_seq")).toInteger(-1);
    if (!klogview::fullBootId(replyBoot) || replyBoot!=bootId_ || seq<0 || quint64(seq)<lastSeq_ || !r.value(QStringLiteral("records")).isArray()) {
        info_->setText(tr("Kernel log reply has an invalid or different boot identity.")); return;
    }
    const auto records=r.value(QStringLiteral("records")).toArray();
    qint64 previous=qint64(lastSeq_);
    for(const auto &record:records) {
        const auto n=record.toObject().value(QStringLiteral("seq")).toInteger(-1);
        if(n<=previous || n>seq) { info_->setText(tr("Malformed kernel log record; capture cursor not advanced.")); return; }
        previous=n;
    }
    if (!persist(records,replyBoot)) return;
    lastSeq_ = std::max(lastSeq_,quint64(seq));
    Q_UNUSED(viaRoot);
    updateInfo();
    render();
}

QString KlogPage::logPath() {
    return diagsrc::savedLogPath(diagsrc::SavedLog::Kernel);
}

bool KlogPage::persist(const QJsonArray &records, const QString &bootId) {
    if (!klogview::fullBootId(bootId) || bootId!=bootId_) return false;
    timespec now{};
    ::clock_gettime(CLOCK_MONOTONIC, &now);  // kernel timestamps count from boot
    const qint64 bootMs = QDateTime::currentMSecsSinceEpoch() - (qint64(now.tv_sec) * 1000 + now.tv_nsec / 1000000);
    QStringList lines;
    quint64 top = savedSeq_;
    for (const QJsonValue &rv : records) {
        const QJsonObject o = rv.toObject();
        const auto seqValue=o.value(QStringLiteral("seq")).toInteger(-1);
        const auto tsValue=o.value(QStringLiteral("ts_us")).toInteger(-1);
        const auto lvlValue=o.value(QStringLiteral("level")).toInteger(-1);
        if(seqValue<0 || tsValue<0 || lvlValue<0 || lvlValue>4 || !o.value(QStringLiteral("msg")).isString()) {
            info_->setText(tr("Malformed kernel log record; capture cursor not advanced.")); return false;
        }
        const quint64 seq=quint64(seqValue);
        if (seq <= savedSeq_) continue;
        top = std::max(top, seq);
        const int lvl=int(lvlValue);
        const qint64 tsMs=tsValue/1000;
        auto message=o.value(QStringLiteral("msg")).toString();
        message.replace(QLatin1Char('\n'),QStringLiteral(" ⏎ ")); message.replace(QLatin1Char('\r'),QStringLiteral(" ⏎ "));
        lines << QStringLiteral("%1  boot=%2 seq=%3  %4  [%5.%6] %7")
                     .arg(QDateTime::fromMSecsSinceEpoch(bootMs + tsMs).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                     .arg(bootId_).arg(seq)
                     .arg(QString::fromLatin1(LEVELS[lvl]), -5)
                     .arg(tsValue / 1'000'000).arg(tsValue % 1'000'000, 6, 10, QLatin1Char('0'))
                     .arg(message);
    }
    if (lines.isEmpty()) return true;
    const QByteArray bytes = (lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8();
    if (!diagsrc::appendSavedLog(diagsrc::SavedLog::Kernel, bytes, LOG_MAX_BYTES)) {
        info_->setText(tr("Could not write the saved log."));
        return false;
    }
    savedSeq_ = top; return true;
}

void KlogPage::updateInfo() {
    const QFileInfo a(logPath()), b(logPath() + QStringLiteral(".1"));
    const qint64 sz = (a.exists() ? a.size() : 0) + (b.exists() ? b.size() : 0);
    QString s = QStringLiteral("Saved log: %1 · %2 KiB").arg(a.absoluteFilePath()).arg((sz + 1023) / 1024);
    if (needsRoot_) s += QStringLiteral("\nKernel log is restricted (kernel.dmesg_restrict=1): captured through pkexec when this page is opened or on Refresh, "
                                        "not in the background. sysctl kernel.dmesg_restrict=0 enables continuous capture.");
    else s += QStringLiteral("\nCapturing continuously (a scan runs only when the kernel logs a warning or error).");
    s += QLatin1Char('\n') + tr("%1 displayed row(s).").arg(displayedRows_);
    if (tailLimited_) s += QLatin1Char(' ') + tr("Showing at most the newest %1 matching records before grouping. Refine the filter to see older records.").arg(MAX_SHOWN_LINES);
    if (hiddenMuted_) s += QLatin1Char('\n') + tr("%1 muted line(s) hidden — right-click a line to unmute its message.").arg(hiddenMuted_);
    info_->setText(s);
}

void KlogPage::render() {
    const bool all = scope_->currentIndex() == 1;
    const QString boot = bootId_;
    bool codeOk=false;
    const auto code=codeFilter_->text().toInt(&codeOk);
    const klogview::Filter typed{bootFilter_->text().trimmed(),kind_->currentData().toString(),deviceFilter_->text().trimmed(),fromFilter_->text().trimmed(),toFilter_->text().trimmed(),codeFilter_->text().isEmpty()?-1:(codeOk?code:-2)};
    const bool valid=typed.valid();
    const auto devicePattern=typed.devicePattern();
    // "a|b|c": any of the alternatives.
    QStringList needles;
    for (const QString &n : filter_->text().split(QLatin1Char('|'), Qt::SkipEmptyParts))
        if (!n.trimmed().isEmpty()) needles << n.trimmed();
    auto matches = [&needles](const QString &l) {
        if (needles.isEmpty()) return true;
        for (const QString &n : needles) if (l.contains(n, Qt::CaseInsensitive)) return true;
        return false;
    };
    const int levelMax = LEVEL_MAX[std::clamp(level_->currentIndex(), 0, 4)];
    // "Only new" needs the earlier boots too, independent of scope and display filters.
    const bool focused = !focusBoot_.isEmpty();
    klogview::ContextWindow context(focusBoot_, focusSeq_, CONTEXT_BEFORE, CONTEXT_AFTER);
    tailLimited_ = false;
    QHash<QString, QString> firstBoot;
    const bool onlyNew = onlyNew_->isChecked();
    const bool currentBootKnown = klogview::fullBootId(boot);
    int matching=0; hiddenMuted_=0;
    std::deque<klogview::Line> shown;  // the newest lines of the scope, level and text filter
    const auto coverage = diagsrc::scanSavedLog(diagsrc::SavedLog::Kernel, LOG_MAX_BYTES, [&](const QString &l, bool) {
        auto p = klogview::parse(l);
        if (!p) return false;
        if (focused) { context.append(*p); return true; }
        const bool trackFirstBoot = onlyNew && klogview::fullBootId(p->boot);
        if (trackFirstBoot && !firstBoot.contains(p->key)) firstBoot.insert(p->key, p->boot);
        if (!all && (!currentBootKnown || p->boot != boot)) return true;
        if (p->level > levelMax) return true;
        if (!matches(l) || !valid) return true;
        QJsonObject event;
        if (!typed.kind.isEmpty() || typed.code>=0) event=eventinfo::classifyLine(p->msg);
        if (!typed.matches(*p,event.value(QStringLiteral("kind")).toString(),event.value(QStringLiteral("code")).toInt(-1),devicePattern)) return true;
        if (muted_.contains(p->key) && !showMuted_->isChecked()) { ++hiddenMuted_; return true; }
        if (trackFirstBoot && firstBoot.value(p->key)!=p->boot) return true;
        ++matching;
        shown.push_back(std::move(*p));
        if (shown.size() > MAX_SHOWN_LINES) { shown.pop_front(); tailLimited_ = true; }
        return true;
    });
    const bool scanLimited = std::any_of(coverage.cbegin(), coverage.cend(), [](const auto &file) {
        return file.omittedBytes > 0 || file.rejectedLines > 0 || file.incompleteTail
            || file.state == diagsrc::LogFileState::Unreadable;
    });

    // Opened from Monitor: same-boot neighbours, without the normal tail cap or severity filter.
    std::vector<klogview::Row> rows;
    int mark = -1;
    if (focused) {
        rows = context.rows();
        mark = context.mark();
    } else {
        rows = klogview::group(shown, group_->isChecked());
    }

    displayedRows_ = int(rows.size());
    rowMessages_.clear();
    focusBar_->setVisible(focused);
    const bool advancedActive=!typed.boot.isEmpty() || !typed.kind.isEmpty() || !typed.device.isEmpty() || !typed.fromDate.isEmpty() || !typed.toDate.isEmpty() || typed.code!=-1;
    filterInfo_->setText(focused ? tr("Context ignores all display filters.") : !valid ? tr("Invalid boot identity, event code or date range.") : tr("Matching saved records: %1. Legacy eight-character boot tags are not full boot identities; use All saved boots to inspect them separately.").arg(matching) + (advancedActive ? tr(" Advanced filters active.") : QString()));
    if (scanLimited)
        filterInfo_->setText(filterInfo_->text() + QLatin1Char(' ') + tr("Some saved records could not be scanned."));
    if (focused)
        focusText_->setText(tr("Showing up to %1 saved lines before and %2 after kernel message %3 in boot %4, independent of display filters.")
                                .arg(CONTEXT_BEFORE).arg(CONTEXT_AFTER).arg(focusSeq_).arg(focusBoot_)
                                + (!klogview::fullBootId(focusBoot_) ? tr(" — Legacy identity; actual boot not verified.") : QString()));
    QString html = QStringLiteral("<pre style='margin:0'>");
    for (int i = 0; i < int(rows.size()); ++i) {
        const klogview::Row &r = rows.at(i);
        const klogview::Line &l = r.first;
        QString plain = l.text;
        if (!klogview::fullBootId(l.boot)) plain += tr(" [legacy boot tag]");
        if (r.count > 1) plain += tr("  ×%1, last %2").arg(r.count).arg(r.last.mid(11));
        if (muted_.contains(l.key)) plain += tr("  [muted]");
        rowMessages_.insert(plain.trimmed(), {l.key, l.msg});
        const char *c = l.level <= 2 ? theme::DANGER : l.level == 3 ? theme::WARN : theme::FG_DIM;
        html += QStringLiteral("<span style='color:%1%2'>%3%4</span>\n").arg(QString::fromLatin1(c),
                                                                             i == mark ? QStringLiteral(";font-weight:bold") : QString(),
                                                                             i == mark ? QStringLiteral("<a name='focus'></a>") : QString(),
                                                                             plain.toHtmlEscaped());
    }
    html += QStringLiteral("</pre>");
    const int vs = text_->verticalScrollBar()->value();
    const bool atEnd = vs >= text_->verticalScrollBar()->maximum() - 4;
    if (focused && rows.empty()) {
        text_->setHtml((scanLimited ? tr("<i>Kernel message %1 was not found in the scanned records.</i>")
                                    : tr("<i>Kernel message %1 is not in the saved log (yet).</i>")).arg(focusSeq_));
        updateInfo();
        return;
    }
    text_->setHtml(rows.empty() ? QStringLiteral("<i>%1</i>").arg(tr("No saved records match the selected filters.").toHtmlEscaped()) : html);
    updateInfo();
    if (mark >= 0) {
        text_->scrollToAnchor(QStringLiteral("focus"));
        return;
    }
    text_->verticalScrollBar()->setValue(follow_->isChecked() && atEnd ? text_->verticalScrollBar()->maximum() : vs);
}

void KlogPage::contextMenu(const QPoint &pos) {
    std::unique_ptr<QMenu> menu(text_->createStandardContextMenu(pos));
    QTextCursor c = text_->cursorForPosition(pos);
    c.select(QTextCursor::LineUnderCursor);
    if (const auto it = rowMessages_.constFind(c.selectedText().trimmed()); it != rowMessages_.constEnd()) {
        const QString key = it->key;
        const QString msg = it->message;
        menu->addSeparator();
        const QJsonObject ev = eventinfo::classifyLine(msg);
        if (!ev.isEmpty()) {
            menu->addAction(tr("What does this mean?"), this, [this, ev] {
                QDialog dlg(this);
                dlg.setWindowTitle(tr("Kernel Message Explained"));
                dlg.resize(620, 480);
                auto *v = new QVBoxLayout(&dlg);
                auto *view = new QTextBrowser;
                view->setHtml(eventinfo::html(ev, ev.value(QStringLiteral("title")).toString()));
                v->addWidget(view, 1);
                auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
                connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
                v->addWidget(bb);
                dlg.exec();
            });
        }
        menu->addAction(tr("Copy this line"), this, [line = c.selectedText().trimmed()] { QApplication::clipboard()->setText(line); });
        const QString stem = klogview::stem(msg);
        if (!stem.isEmpty())
            menu->addAction(tr("Show only lines like this"), this, [this, stem] { filter_->setText(stem); });
        if (muted_.contains(key)) menu->addAction(tr("Unmute this message"), this, [this, key] { setMuted(key, false); });
        else menu->addAction(tr("Mute this message"), this, [this, key] { setMuted(key, true); });
    }
    menu->exec(text_->mapToGlobal(pos));
}

void KlogPage::setMuted(const QString &key, bool on) {
    if (on) muted_.insert(key);
    else muted_.remove(key);
    QStringList list;
    for (const QString &k : std::as_const(muted_)) list << k;
    list.sort();
    QSettings st(guiIniPath(), QSettings::IniFormat);
    st.setValue(QStringLiteral("klog/muted"), list);
    render();
}

void KlogPage::fillPresets(QMenu *menu) {
    menu->clear();
    struct Preset { const char *label, *filter; };
    static const Preset PRESETS[] = {
        {QT_TR_NOOP("Graphics (NVIDIA, AMD, Intel, DRM)"), "NVRM|nvidia|amdgpu|i915|xe 0000|drm|nouveau"},
        {QT_TR_NOOP("Storage (NVMe, SATA, file systems)"), "nvme|ata1|ata2|ata3|ata4|ahci|[sd|EXT4-fs|BTRFS|XFS|I/O error|blk_update"},
        {QT_TR_NOOP("USB and Thunderbolt"), "usb|xhci|thunderbolt|typec|ucsi"},
        {QT_TR_NOOP("PCIe"), "pcieport|AER|PCIe|pci 0000"},
        {QT_TR_NOOP("Network and Wi-Fi"), "iwlwifi|mt79|ath1|rtw|r8169|igc|e1000|wlan|wlp|enp|Bluetooth"},
        {QT_TR_NOOP("Firmware and ACPI"), "firmware|ACPI|BIOS|efi:|microcode"},
        {QT_TR_NOOP("CPU, memory and machine checks"), "mce:|Machine check|Hardware Error|EDAC|Out of memory|oom-kill|lockup|detected stall"},
        {QT_TR_NOOP("Power, suspend and thermal"), "PM:|suspend|resume|thermal|temperature|throttl|D3cold"},
        {QT_TR_NOOP("Kernel bugs and warnings"), "BUG:|Oops|WARNING:|Call Trace|taint"},
        {QT_TR_NOOP("Audio"), "snd|sof-audio|hda|soundwire"},
    };
    for (const Preset &p : PRESETS)
        menu->addAction(tr(p.label), this, [this, f = QString::fromLatin1(p.filter)] { filter_->setText(f); });
    menu->addSeparator();
    menu->addAction(tr("Clear filter"), this, [this] {
        filter_->clear(); bootFilter_->clear(); deviceFilter_->clear(); codeFilter_->clear();
        fromFilter_->clear(); toFilter_->clear(); kind_->setCurrentIndex(0);
    });
}

void KlogPage::saveView() {
    const QString file = QFileDialog::getSaveFileName(this, tr("Save kernel log view"),
        QDir::homePath() + QStringLiteral("/kernel-log-%1.txt").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm"))),
        tr("Text files (*.txt *.log);;All files (*)"));
    if (file.isEmpty()) return;
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Save kernel log view"), tr("Cannot write %1: %2").arg(file, f.errorString()));
        return;
    }
    f.write(text_->toPlainText().toUtf8());
    f.write("\n");
}
