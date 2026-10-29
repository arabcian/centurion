#include "bootpage.h"
#include "privileged.h"
#include "theme.h"

#include <QColor>
#include <QDateTime>
#include <QDir>
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
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>

static constexpr qint64 TAIL_BYTES = 512 << 10;   // same bound as KlogPage: only the end of the saved log is read
static constexpr int LOG_LINES = 30;
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

BootPage::BootPage(QWidget *parent) : QWidget(parent) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(6);

    auto *top = new QHBoxLayout;
    info_ = new QLabel;
    info_->setWordWrap(true);
    info_->setStyleSheet(QStringLiteral("color:%1").arg(theme::MUTED));
    top->addWidget(info_, 1);
    bClear_ = new QPushButton(tr("Archive and clear pstore…"));
    bClear_->setVisible(false);
    connect(bClear_, &QPushButton::clicked, this, &BootPage::clearPstore);
    top->addWidget(bClear_);
    auto *bRefresh = new QPushButton(tr("Refresh"));
    connect(bRefresh, &QPushButton::clicked, this, &BootPage::reload);
    top->addWidget(bRefresh);
    root->addLayout(top);

    auto *split = new QSplitter(Qt::Vertical);
    list_ = new QTreeWidget;
    list_->setColumnCount(5);
    list_->setHeaderLabels({tr("Started"), tr("Lasted"), tr("Ended"), tr("Kernel events"), tr("Curves in effect")});
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

    auto *hint = new QLabel(tr("A boot is \"unclean\" when the next boot found it never closed: a crash, hang or forced power-off. "
                               "Kernel events are recorded when a boot ends cleanly, so an unclean boot has none; its last saved "
                               "kernel-log lines and the pstore records of the crash are shown instead. A repeatedly unclean boot "
                               "with the same curve in effect points at that curve."));
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:%1").arg(theme::MUTED));
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
