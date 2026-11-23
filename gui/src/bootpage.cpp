#include "bootpage.h"
#include "flightrec.h"
#include "privileged.h"
#include "theme.h"

#include <QColor>
#include <QComboBox>
#include <QDateTime>
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
        if (auto *it = list_->currentItem(); it && !it->data(0, ROLE_BOOT).toString().isEmpty())
            Q_EMIT openKernelLog(it->data(0, ROLE_BOOT).toString().left(8));
    });
    tools->addWidget(bKlog_);
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

    list_->blockSignals(true);
    list_->clear();
    QTreeWidgetItem *pick = nullptr;
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
    if (pick) list_->setCurrentItem(pick);
    else detail_->clear();

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
    if (!it) { detail_->clear(); return; }
    const QString id = it->data(0, ROLE_BOOT).toString();
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
    out += tr("Boot %1 — %2\n").arg(id.left(8), stateText(state));
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
        const QStringList tail = kernelLogTail(id.left(8), LOG_LINES);
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
        if (!first) detail_->setPlainText(list_->topLevelItemCount() ? tr("No recorded boot matches this filter.") : QString());
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
    QStringList out;
    const QJsonObject a = prev.value(QStringLiteral("curves")).toObject(), b = boot.value(QStringLiteral("curves")).toObject();
    QStringList keys = a.keys() + b.keys();
    keys.removeDuplicates();
    for (const QString &k : std::as_const(keys)) {
        const QString x = a.value(k).toString(), y = b.value(k).toString();
        if (x != y) out << tr("  %1: %2 → %3").arg(k, x.isEmpty() ? tr("stock") : x, y.isEmpty() ? tr("stock") : y);
    }
    const QString sa = prev.value(QStringLiteral("state")).toString(), sb = boot.value(QStringLiteral("state")).toString();
    // Events are recorded when a boot ends cleanly: a running or unclean boot has none to compare.
    const int ea = prev.value(QStringLiteral("event_total")).toInt(), eb = boot.value(QStringLiteral("event_total")).toInt();
    if (ea != eb && sa == QLatin1String("clean") && sb == QLatin1String("clean")) out << tr("  kernel events: %1 → %2").arg(ea).arg(eb);
    if (sa != sb && sa != QLatin1String("running") && sb != QLatin1String("running"))
        out << tr("  ended: %1 → %2").arg(stateText(prev.value(QStringLiteral("state")).toString()), stateText(boot.value(QStringLiteral("state")).toString()));
    if (out.isEmpty()) return tr("Compared with the boot before: nothing changed in curves or events.\n");
    return tr("Compared with the boot before (%1):\n%2\n").arg(when(qint64(prev.value(QStringLiteral("start")).toDouble())), out.join(QLatin1Char('\n')));
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
