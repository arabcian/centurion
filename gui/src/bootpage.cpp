#include "bootpage.h"
#include "flightrec.h"
#include "privileged.h"
#include "theme.h"

#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QSet>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QMap>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QScrollBar>
#include <QTreeWidget>
#include <QVBoxLayout>

static constexpr qint64 TAIL_BYTES = 512 << 10;   // same bound as KlogPage: only the end of the saved log is read
static constexpr int LOG_LINES = 50;
static const char *FLIGHT_CONF = "/etc/centurion/flight-interval";   // flight.rs CONF; absent = 5 s
static constexpr int ROLE_BOOT = Qt::UserRole;      // boot id of a row ("" = the unattributed-pstore row)

static QString when(qint64 t) { return QDateTime::fromSecsSinceEpoch(t).toString(QStringLiteral("yyyy-MM-dd HH:mm")); }

static QString duration(qint64 s) {
    return s < 60 ? BootPage::tr("%1 s").arg(s) : BootPage::tr("%1 h %2 min").arg(s / 3600).arg(s / 60 % 60);
}

static QString stateText(const QString &s) {
    if (s == QLatin1String("clean")) return BootPage::tr("clean");
    if (s == QLatin1String("unclean")) return BootPage::tr("unclean (crash, hang or power loss)");
    if (s == QLatin1String("running")) return BootPage::tr("running");
    return BootPage::tr("not tracked");   // left open: centurion-boot-guard did not run in that boot
}

static QString eventsText(const QJsonObject &ev) {
    QStringList p;
    for (auto it = ev.begin(); it != ev.end(); ++it) p << QStringLiteral("%1 ×%2").arg(it.key()).arg(it.value().toInt());
    return p.join(QStringLiteral(", "));
}

static QString curvesText(const QJsonObject &c) {
    QStringList p;
    for (auto it = c.begin(); it != c.end(); ++it) p << it.key() + QStringLiteral(": ") + it.value().toString();
    return p.join(QStringLiteral("; "));
}

// The curve of each kind in effect when the boot ended (or crashed): the last of its " → " states.
static QString lastCurvesText(const QJsonObject &c) {
    QStringList p;
    for (auto it = c.begin(); it != c.end(); ++it) p << it.key() + QStringLiteral(": ") + it.value().toString().section(QStringLiteral(" → "), -1);
    return p.join(QStringLiteral("; "));
}

BootPage::BootPage(QWidget *parent) : QWidget(parent) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(6);

    auto *top = new QHBoxLayout;
    info_ = new QLabel;
    info_->setWordWrap(true);
    info_->setProperty("tone", "muted");
    top->addWidget(info_, 1);
    bClear_ = new QPushButton(tr("Archive and clear pstore…"));
    bClear_->setVisible(false);
    connect(bClear_, &QPushButton::clicked, this, &BootPage::clearPstore);
    top->addWidget(bClear_);
    top->addWidget(new QLabel(tr("Flight recorder:")));
    interval_ = new QComboBox;
    for (int sec : {5, 10, 30, 60})   // flight.rs CHOICES
        interval_->addItem(sec < 60 ? tr("every %1 s").arg(sec) : tr("every %1 min").arg(sec / 60), sec);
    interval_->setToolTip(tr("How often centurion-boot-guard takes a sensor sample and syncs it to disk. Shorter: at most that much "
                             "is lost when the machine hangs, but one disk flush per sample. Longer: fewer writes, and the hour-long "
                             "ring covers more time (up to 12 h at 1 min). Applies from the next sample."));
    interval_->setCurrentIndex(qMax(0, interval_->findData(flightInterval())));
    connect(interval_, &QComboBox::activated, this, &BootPage::setFlightInterval);
    top->addWidget(interval_);
    auto *bRefresh = new QPushButton(tr("Refresh"));
    connect(bRefresh, &QPushButton::clicked, this, &BootPage::reload);
    top->addWidget(bRefresh);
    root->addLayout(top);

    auto *tools = new QHBoxLayout;
    tools->addWidget(new QLabel(tr("Show:")));
    filter_ = new QComboBox;
    filter_->addItem(tr("All boots"), QStringLiteral("all"));
    filter_->addItem(tr("Unclean only"), QStringLiteral("unclean"));
    filter_->addItem(tr("With kernel events"), QStringLiteral("events"));
    filter_->addItem(tr("With a CPU/GPU curve in effect"), QStringLiteral("curves"));
    connect(filter_, &QComboBox::currentIndexChanged, this, &BootPage::applyFilter);
    tools->addWidget(filter_);
    tools->addStretch(1);
    bKlog_ = new QPushButton(tr("Kernel log of this boot"));
    bKlog_->setToolTip(tr("Open the saved kernel log filtered to the selected boot."));
    connect(bKlog_, &QPushButton::clicked, this, [this] {
        if (auto *it = list_->currentItem(); it && !it->isHidden() && uniqueBootPrefix(it->data(0, ROLE_BOOT).toString()))
            Q_EMIT openKernelLog(it->data(0, ROLE_BOOT).toString().left(8));
    });
    tools->addWidget(bKlog_);
    auto *compare = new QPushButton(tr("Compare boots…"));
    connect(compare, &QPushButton::clicked, this, &BootPage::compareSelectedBoots);
    tools->addWidget(compare);
    auto *bCopy = new QPushButton(tr("Copy details"));
    connect(bCopy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(detail_->toPlainText()); });
    tools->addWidget(bCopy);
    auto *bSave = new QPushButton(tr("Save report…"));
    bSave->setToolTip(tr("Every recorded boot with its end, events and curves, plus the patterns below, as a text file (for a bug report or a forum post)."));
    connect(bSave, &QPushButton::clicked, this, &BootPage::saveReport);
    tools->addWidget(bSave);
    root->addLayout(tools);

    auto *split = new QSplitter(Qt::Vertical);
    list_ = new QTreeWidget;
    list_->setColumnCount(5);
    list_->setHeaderLabels({tr("Started"), tr("Lasted"), tr("Ended"), tr("Kernel Events"), tr("Curves in Effect")});
    list_->setRootIsDecorated(false);
    list_->setAlternatingRowColors(true);
    list_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    list_->header()->setStretchLastSection(true);
    connect(list_, &QTreeWidget::currentItemChanged, this, &BootPage::showDetail);
    split->addWidget(list_);
    detail_ = new QPlainTextEdit;
    detail_->setReadOnly(true);
    detail_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    split->addWidget(detail_);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);
    root->addWidget(split, 1);
    patterns_ = new QLabel;
    patterns_->setWordWrap(true);
    patterns_->setTextFormat(Qt::RichText);
    patterns_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(patterns_);

    auto *hint = new QLabel(tr("A boot is \"unclean\" when the next boot found it never closed: a crash, hang or forced power-off. "
                               "Kernel events are recorded when a boot ends cleanly, so an unclean boot has none; its last saved "
                               "kernel-log lines and the pstore records of the crash are shown instead. A repeatedly unclean boot "
                               "with the same curve in effect points at that curve."));
    hint->setWordWrap(true);
    hint->setProperty("tone", "muted");
    root->addWidget(hint);
}

void BootPage::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    if (!age_.isValid() || age_.elapsed() > 30000) reload();
}

void BootPage::reload() {
    if (busy_) return;
    const QString helper = privileged::helperPath(QStringLiteral("health-reader"));
    if (!QFileInfo(helper).isExecutable()) { info_->setText(tr("health-reader is not installed.")); return; }
    busy_ = true;
    privileged::run(helper, QJsonObject{{QStringLiteral("op"), QStringLiteral("boots")}}, this, [this](const privileged::Result &r) {
        busy_ = false;
        if (!r.reached) { info_->setText(tr("Could not read the boot history: %1").arg(r.message())); return; }
        if (!r.json.value(QStringLiteral("ok")).toBool()) { info_->setText(tr("Could not read the boot history: %1").arg(r.message())); return; }
        if (!r.json.contains(QStringLiteral("boots"))) { info_->setText(tr("The installed health-reader is too old for this page — reinstall Centurion.")); return; }
        apply(r.json);
    });
}

void BootPage::apply(const QJsonObject &r) {
    data_ = r;
    age_.start();
    const QJsonArray boots = r.value(QStringLiteral("boots")).toArray();
    const QJsonObject ps = r.value(QStringLiteral("pstore")).toObject();
    const QJsonArray entries = ps.value(QStringLiteral("entries")).toArray();

    const QString previous = list_->currentItem() ? list_->currentItem()->data(0, ROLE_BOOT).toString() : QString();
    const int scroll = list_->verticalScrollBar()->value();
    list_->blockSignals(true);
    list_->clear();
    QTreeWidgetItem *pick = nullptr, *retained = nullptr;
    const QString prevBoot = r.value(QStringLiteral("previous_unclean")).toObject().value(QStringLiteral("boot")).toString();
    int unclean = 0;
    for (const QJsonValue &v : boots) {
        const QJsonObject b = v.toObject();
        const QString state = b.value(QStringLiteral("state")).toString();
        const QJsonObject curves = b.value(QStringLiteral("curves")).toObject();
        auto *it = new QTreeWidgetItem({when(qint64(b.value(QStringLiteral("start")).toDouble())),
                                        b.contains(QStringLiteral("duration")) ? duration(qint64(b.value(QStringLiteral("duration")).toDouble())) : QStringLiteral("—"),
                                        stateText(state),
                                        b.value(QStringLiteral("event_total")).toInt() ? eventsText(b.value(QStringLiteral("events")).toObject()) : QStringLiteral("—"),
                                        curves.isEmpty() ? QStringLiteral("—") : curvesText(curves)});
        it->setData(0, ROLE_BOOT, b.value(QStringLiteral("boot")).toString());
        it->setData(0, ROLE_BOOT + 1, state);
        it->setData(0, ROLE_BOOT + 2, b.value(QStringLiteral("event_total")).toInt());
        it->setData(0, ROLE_BOOT + 3, !curves.isEmpty());
        it->setToolTip(4, tr("Empty: no curve was applied in this boot (stock), or the record is older than the curve journal."));
        const char *c = state == QLatin1String("unclean") ? theme::DANGER : state == QLatin1String("running") ? theme::OK
                        : state == QLatin1String("unknown") ? theme::MUTED : nullptr;
        if (c) for (int i = 0; i < 5; ++i) it->setForeground(i, QColor(QLatin1String(c)));
        list_->addTopLevelItem(it);
        if (!previous.isEmpty() && it->data(0, ROLE_BOOT).toString() == previous) retained = it;
        if (state == QLatin1String("unclean")) ++unclean;
        if (!pick && (b.value(QStringLiteral("boot")).toString() == prevBoot && !prevBoot.isEmpty())) pick = it;
    }
    int loose = 0;
    for (const QJsonValue &e : entries) if (!e.toObject().contains(QStringLiteral("boot"))) ++loose;
    if (loose) {
        auto *it = new QTreeWidgetItem({tr("pstore records without a matching boot (%1)").arg(loose), QString(), QString(), QString(), QString()});
        it->setData(0, ROLE_BOOT, QString());
        list_->addTopLevelItem(it);
    }
    if (!pick && list_->topLevelItemCount()) pick = list_->topLevelItem(0);
    list_->blockSignals(false);
    applyFilter();
    patterns_->setText(patterns(false));
    if (retained && !retained->isHidden()) list_->setCurrentItem(retained);
    else if (pick && !pick->isHidden()) list_->setCurrentItem(pick);
    // applyFilter already selected the first visible boot or displayed the empty state.
    if (list_->currentItem() && !list_->currentItem()->isHidden()) showDetail();
    if (retained) list_->verticalScrollBar()->setValue(scroll);

    bool clearable = false;
    for (const QJsonValue &e : entries) if (e.toObject().value(QStringLiteral("src")).toString() == QLatin1String("pstore")) clearable = true;
    bClear_->setVisible(clearable);   // no pstore records to free: the control does not appear
    info_->setText(boots.isEmpty() ? tr("No boot history yet — centurion-boot-guard records each boot once it is enabled.")
                                   : tr("%1 boot(s) recorded, %2 ended uncleanly. pstore records found: %3.").arg(boots.size()).arg(unclean).arg(entries.size()));
    const QJsonObject pu = r.value(QStringLiteral("previous_unclean")).toObject();
    Q_EMIT previousUnclean(pu.value(QStringLiteral("boot")).toString(), qint64(pu.value(QStringLiteral("end")).toDouble()));
}

void BootPage::showDetail() {
    auto *it = list_->currentItem();
    if (!it || it->isHidden()) { bKlog_->setEnabled(false); detail_->clear(); return; }
    const QString id = it->data(0, ROLE_BOOT).toString();
    bKlog_->setEnabled(uniqueBootPrefix(id));
    bKlog_->setToolTip(uniqueBootPrefix(id) ? tr("Open the saved kernel log filtered to the selected boot.") : tr("No unique eight-character boot tag is available; the legacy kernel log cannot identify this boot safely."));
    const QJsonArray entries = data_.value(QStringLiteral("pstore")).toObject().value(QStringLiteral("entries")).toArray();
    auto entryText = [](const QJsonObject &e) {
        return tr("── pstore: %1 (%2, %3, %4) ──\n%5\n").arg(e.value(QStringLiteral("name")).toString(), e.value(QStringLiteral("kind")).toString(),
                   e.value(QStringLiteral("src")).toString(), when(qint64(e.value(QStringLiteral("t")).toDouble())), e.value(QStringLiteral("text")).toString());
    };
    QString out;
    if (id.isEmpty()) {   // the unattributed pstore records
        for (const QJsonValue &v : entries) if (!v.toObject().contains(QStringLiteral("boot"))) out += entryText(v.toObject()) + QLatin1Char('\n');
        detail_->setPlainText(out);
        return;
    }
    QJsonObject b;
    for (const QJsonValue &v : data_.value(QStringLiteral("boots")).toArray()) if (v.toObject().value(QStringLiteral("boot")).toString() == id) b = v.toObject();
    const QString state = b.value(QStringLiteral("state")).toString();
    out += tr("Boot %1 — %2\n").arg(id, stateText(state));
    out += tr("Started: %1").arg(when(qint64(b.value(QStringLiteral("start")).toDouble())));
    if (b.contains(QStringLiteral("end"))) out += tr("   Last seen alive: %1").arg(when(qint64(b.value(QStringLiteral("end")).toDouble())));
    out += QLatin1String("\n");
    const QJsonObject ev = b.value(QStringLiteral("events")).toObject();
    if (!ev.isEmpty()) out += tr("Kernel events: %1\n").arg(eventsText(ev));
    const QJsonObject cv = b.value(QStringLiteral("curves")).toObject();
    if (!cv.isEmpty()) out += tr("Curves: %1\n").arg(curvesText(cv));
    out += comparedWithPrevious(b);
    const QJsonArray flight = b.value(QStringLiteral("flight")).toArray();
    if (!flight.isEmpty()) out += QLatin1Char('\n') + flightrec::text(flight);
    if (state == QLatin1String("unclean")) {
        out += QLatin1Char('\n');
        for (const QJsonValue &i : b.value(QStringLiteral("pstore")).toArray()) out += entryText(entries.at(i.toInt()).toObject()) + QLatin1Char('\n');
        const QStringList tail = uniqueBootPrefix(id) ? kernelLogTail(id.left(8), LOG_LINES) : QStringList{};
        if (!uniqueBootPrefix(id)) out += tr("Legacy log attribution unavailable: boot prefix is missing or ambiguous.\n");
        out += tail.isEmpty() ? tr("── no saved kernel-log lines for this boot ──\n")
                              : tr("── last %1 saved kernel-log lines of this boot ──\n%2\n").arg(tail.size()).arg(tail.join(QLatin1Char('\n')));
    }
    detail_->setPlainText(out);
}

void BootPage::clearPstore() {
    const QJsonArray entries = data_.value(QStringLiteral("pstore")).toObject().value(QStringLiteral("entries")).toArray();
    int n = 0;
    for (const QJsonValue &e : entries) if (e.toObject().value(QStringLiteral("src")).toString() == QLatin1String("pstore")) ++n;
    if (QMessageBox::question(this, tr("Archive and clear pstore"),
            tr("Copy %1 pstore record(s) to /var/lib/centurion/pstore (kernel addresses removed) and then delete them from /sys/fs/pstore?\n\n"
               "Deleting them frees the EFI variables they occupy. Records that cannot be archived are left where they are.").arg(n))
        != QMessageBox::Yes) return;
    bClear_->setEnabled(false);
    privileged::run(privileged::helperPath(QStringLiteral("tune-helper")), QJsonObject{{QStringLiteral("op"), QStringLiteral("pstore_clear")}}, this,
                    [this](const privileged::Result &r) {
        bClear_->setEnabled(true);
        if (!r.ok()) QMessageBox::warning(this, tr("Archive and clear pstore"), tr("pstore was not (fully) cleared: %1").arg(r.message()));
        age_.invalidate();
        reload();
    }, 120000);
}

int BootPage::flightInterval() {
    QFile f(QString::fromLatin1(FLIGHT_CONF));
    const int v = f.open(QIODevice::ReadOnly) ? f.read(16).trimmed().toInt() : 0;
    return QList<int>{5, 10, 30, 60}.contains(v) ? v : 5;
}

void BootPage::setFlightInterval() {
    const int want = interval_->currentData().toInt();
    if (want == flightInterval()) return;
    interval_->setEnabled(false);
    privileged::run(privileged::helperPath(QStringLiteral("tune-helper")), QJsonObject{{"op", "flight_interval"}, {"seconds", want}}, this,
                    [this](const privileged::Result &r) {
        interval_->setEnabled(true);
        if (!r.ok()) QMessageBox::warning(this, tr("Flight recorder"), tr("The interval was not changed: %1").arg(r.message()));
        interval_->setCurrentIndex(qMax(0, interval_->findData(flightInterval())));   // show what is in effect
    });
}

QString BootPage::logPath() {
    QString dir = qEnvironmentVariable("XDG_STATE_HOME");  // GenericStateLocation needs Qt 6.7
    if (dir.isEmpty()) dir = QDir::homePath() + QStringLiteral("/.local/state");
    return dir + QStringLiteral("/centurion/kernel.log");
}

QStringList BootPage::kernelLogTail(const QString &boot8, int lines) {
    const QString tag = QStringLiteral("boot=") + boot8 + QStringLiteral(" seq=");
    QStringList out;
    for (const QString &path : {logPath() + QStringLiteral(".1"), logPath()}) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        if (f.size() > TAIL_BYTES) { f.seek(f.size() - TAIL_BYTES); f.readLine(); }
        while (!f.atEnd()) {
            const QString l = QString::fromUtf8(f.readLine()).trimmed();
            if (!l.contains(tag)) continue;
            out << l;
            if (out.size() > lines) out.removeFirst();
        }
    }
    return out;
}

void BootPage::applyFilter() {
    const QString mode = filter_ ? filter_->currentData().toString() : QStringLiteral("all");
    for (int i = 0; i < list_->topLevelItemCount(); ++i) {
        QTreeWidgetItem *it = list_->topLevelItem(i);
        bool show = true;
        if (mode == QLatin1String("unclean")) show = it->data(0, ROLE_BOOT + 1).toString() == QLatin1String("unclean");
        else if (mode == QLatin1String("events")) show = it->data(0, ROLE_BOOT + 2).toInt() > 0;
        else if (mode == QLatin1String("curves")) show = it->data(0, ROLE_BOOT + 3).toBool();
        it->setHidden(!show);
    }
    if (!list_->currentItem() || list_->currentItem()->isHidden()) {
        QTreeWidgetItem *first = nullptr;
        for (int i = 0; i < list_->topLevelItemCount() && !first; ++i)
            if (!list_->topLevelItem(i)->isHidden()) first = list_->topLevelItem(i);
        list_->setCurrentItem(first);   // none: the detail of a hidden boot must not stay on screen
        if (!first) { bKlog_->setEnabled(false); detail_->setPlainText(list_->topLevelItemCount() ? tr("No recorded boot matches this filter.") : QString()); }
    }
}

QString BootPage::patterns(bool everyCurve) const {
    const QJsonArray boots = data_.value(QStringLiteral("boots")).toArray();
    int ended = 0, unclean = 0, timed = 0;
    qint64 total = 0, longest = 0;
    QMap<QString, QPair<int, int>> byCurves;   // curves in effect at the end → (ended boots, unclean)
    for (const QJsonValue &v : boots) {
        const QJsonObject b = v.toObject();
        const QString state = b.value(QStringLiteral("state")).toString();
        if (state != QLatin1String("clean") && state != QLatin1String("unclean")) continue;
        ++ended;
        const bool bad = state == QLatin1String("unclean");
        unclean += bad;
        if (b.contains(QStringLiteral("duration"))) {   // a boot without an end time has no length to average
            const qint64 d = qint64(b.value(QStringLiteral("duration")).toDouble());
            total += d;
            longest = std::max(longest, d);
            ++timed;
        }
        const QJsonObject c = b.value(QStringLiteral("curves")).toObject();
        auto &slot = byCurves[c.isEmpty() ? tr("stock (no curve)") : lastCurvesText(c)];
        ++slot.first;
        slot.second += bad;
    }
    if (!ended) return {};
    QStringList lines;
    lines << tr("<b>Patterns</b> over %1 finished boot(s): %2 unclean (%3%), average session %4, longest %5.")
                 .arg(ended).arg(unclean).arg(unclean * 100 / ended)
                 .arg(timed ? duration(total / timed) : QStringLiteral("—"), timed ? duration(longest) : QStringLiteral("—"));
    if (unclean && byCurves.size() > 1) {
        QStringList rows;
        for (auto it = byCurves.cbegin(); it != byCurves.cend(); ++it) {
            const int pct = it.value().second * 100 / it.value().first;
            const bool flagged = pct >= 50 && it.value().second >= 2;
            if (!everyCurve && !flagged) continue;   // on screen: one row per curve sequence would bury the page
            const QString row = tr("%1: %2 of %3 unclean").arg(it.key().toHtmlEscaped()).arg(it.value().second).arg(it.value().first);
            rows << (flagged ? QStringLiteral("<span style='color:%1'>%2</span>").arg(QLatin1String(theme::DANGER), row) : row);
        }
        if (!rows.isEmpty()) {
            lines << tr("By curve in effect — %1").arg(rows.join(QStringLiteral(" · ")));
            lines << QStringLiteral("<span style='color:%1'>%2</span>").arg(QLatin1String(theme::MUTED),
                         tr("Red: half or more of the boots with that curve ended uncleanly — test it at stock before blaming hardware."));
        }
    }
    return lines.join(QStringLiteral("<br>"));
}

QString BootPage::comparedWithPrevious(const QJsonObject &boot) const {
    const QJsonArray boots = data_.value(QStringLiteral("boots")).toArray();   // newest first
    int at = -1;
    for (int i = 0; i < boots.size(); ++i)
        if (boots.at(i).toObject().value(QStringLiteral("boot")) == boot.value(QStringLiteral("boot"))) { at = i; break; }
    if (at < 0 || at + 1 >= boots.size()) return {};
    const QJsonObject prev = boots.at(at + 1).toObject();
    return tr("Compared with the boot before (%1):\n%2\n").arg(when(prev.value(QStringLiteral("start")).toInteger()), compareBoots(prev, boot));
}

bool BootPage::uniqueBootPrefix(const QString &id) const {
    if (id.size() < 8) return false;
    int matches = 0;
    for (const auto &v : data_.value(QStringLiteral("boots")).toArray())
        if (v.toObject().value(QStringLiteral("boot")).toString().startsWith(id.left(8))) ++matches;
    return matches == 1;
}

QString BootPage::compareBoots(const QJsonObject &before, const QJsonObject &after) const {
    QStringList out;
    const auto unknown = tr("unknown / not recorded");
    const auto text = [&unknown](const QJsonValue &v) -> QString {
        if (v.isString() && !v.toString().isEmpty()) return v.toString();
        if (v.isDouble() && v.toInteger(-1) >= 0) return QString::number(v.toInteger());
        return unknown;
    };
    out << tr("Before: %1").arg(text(before.value(QStringLiteral("boot")))) << tr("After: %1").arg(text(after.value(QStringLiteral("boot"))));
    auto compareObject = [&](const QString &title, const QJsonObject &a, const QJsonObject &b, const QStringList &expected = {}) {
        out << QString() << title;
        QStringList keys = a.keys() + b.keys() + expected;
        keys.removeDuplicates(); keys.sort();
        if (keys.isEmpty()) out << unknown;
        for (const auto &key : keys) {
            const auto x = text(a.value(key)), y = text(b.value(key));
            if (x == unknown || y == unknown || x != y) out << QStringLiteral("  %1: %2 → %3").arg(key, x, y);
            else out << tr("  %1: %2 (unchanged)").arg(key, x);
        }
    };
    compareObject(tr("Recorded versions"), before.value(QStringLiteral("versions")).toObject(), after.value(QStringLiteral("versions")).toObject(), {QStringLiteral("kernel"), QStringLiteral("bios"), QStringLiteral("microcode"), QStringLiteral("nvidia")});
    compareObject(tr("Recorded curve states (missing does not establish stock)"), before.value(QStringLiteral("curves")).toObject(), after.value(QStringLiteral("curves")).toObject());
    const auto sa = before.value(QStringLiteral("state")).toString(), sb = after.value(QStringLiteral("state")).toString();
    out << QString() << tr("Boot state: %1 → %2").arg(stateText(sa), stateText(sb));
    out << tr("Recorded duration (seconds): %1 → %2").arg(text(before.value(QStringLiteral("duration"))), text(after.value(QStringLiteral("duration"))));
    if (sa == QLatin1String("clean") && sb == QLatin1String("clean")) {
        out << tr("Recorded kernel event total: %1 → %2").arg(text(before.value(QStringLiteral("event_total"))), text(after.value(QStringLiteral("event_total"))));
        compareObject(tr("Recorded event categories"), before.value(QStringLiteral("events")).toObject(), after.value(QStringLiteral("events")).toObject());
    } else out << tr("Kernel event counts are not comparable: both boots must have a clean recorded end.");
    const auto pstoreCount = [&unknown](const QJsonObject &boot) {
        const auto v = boot.value(QStringLiteral("pstore"));
        return v.isArray() ? QString::number(v.toArray().size()) : unknown;
    };
    out << tr("Attributed pstore entries (not a crash count): %1 → %2").arg(pstoreCount(before), pstoreCount(after));
    out << tr("Boot guard / preset history: unavailable in this snapshot.");
    return out.join(QLatin1Char('\n'));
}

void BootPage::compareSelectedBoots() {
    QDialog dlg(this); dlg.setWindowTitle(tr("Compare boots")); dlg.resize(800, 620);
    auto *layout = new QVBoxLayout(&dlg);
    auto *before = new QComboBox; auto *after = new QComboBox;
    // Freeze the visible records; a subsequent helper refresh cannot replace one of the selected boots.
    QMap<QString, QJsonObject> captured;
    QSet<QString> duplicate;
    for (const auto &v : data_.value(QStringLiteral("boots")).toArray()) {
        const auto b = v.toObject(); const auto id = b.value(QStringLiteral("boot")).toString();
        if (id.isEmpty()) continue;
        if (captured.contains(id)) duplicate.insert(id);
        captured.insert(id, b);
    }
    for (int i = 0; i < list_->topLevelItemCount(); ++i) {
        auto *row = list_->topLevelItem(i); const auto id = row->data(0, ROLE_BOOT).toString();
        if (row->isHidden() || !captured.contains(id) || duplicate.contains(id)) continue;
        const auto label = row->text(0) + QStringLiteral(" · ") + id;
        before->addItem(label, id); after->addItem(label, id);
    }
    layout->addWidget(new QLabel(tr("Before"))); layout->addWidget(before);
    layout->addWidget(new QLabel(tr("After"))); layout->addWidget(after);
    auto *detail = new QPlainTextEdit; detail->setReadOnly(true); layout->addWidget(detail, 1);
    if (before->count() > 1) before->setCurrentIndex(1);
    if (const auto *row = list_->currentItem(); row && !row->isHidden()) after->setCurrentIndex(qMax(0, after->findData(row->data(0, ROLE_BOOT))));
    auto update = [=, this] {
        const QString a = before->currentData().toString(), b = after->currentData().toString();
        detail->setPlainText(a.isEmpty() || b.isEmpty() || a == b ? tr("Select two different visible boot records.") : compareBoots(captured.value(a), captured.value(b)));
    };
    connect(before, &QComboBox::currentIndexChanged, &dlg, update);
    connect(after, &QComboBox::currentIndexChanged, &dlg, update);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    update(); dlg.exec();
}

void BootPage::saveReport() {
    const QString file = QFileDialog::getSaveFileName(this, tr("Save boot report"),
        QDir::homePath() + QStringLiteral("/centurion-boots-%1.txt").arg(QDate::currentDate().toString(Qt::ISODate)),
        tr("Text files (*.txt);;All files (*)"));
    if (file.isEmpty()) return;
    QStringList out{tr("Centurion boot history — %1").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))), QString()};
    for (const QJsonValue &v : data_.value(QStringLiteral("boots")).toArray()) {
        const QJsonObject b = v.toObject();
        out << QStringLiteral("%1  %2  %3  %4  %5").arg(when(qint64(b.value(QStringLiteral("start")).toDouble())),
                   b.contains(QStringLiteral("duration")) ? duration(qint64(b.value(QStringLiteral("duration")).toDouble())) : QStringLiteral("—"),
                   stateText(b.value(QStringLiteral("state")).toString()),
                   eventsText(b.value(QStringLiteral("events")).toObject()), curvesText(b.value(QStringLiteral("curves")).toObject()));
    }
    QString plain = patterns(true);
    plain.replace(QStringLiteral("<br>"), QStringLiteral("\n")).remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    out << QString() << plain;
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Save boot report"), tr("Cannot write %1: %2").arg(file, f.errorString()));
        return;
    }
    f.write((out.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8());
}
