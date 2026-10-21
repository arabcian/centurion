#include "sleeppage.h"
#include "privileged.h"
#include "theme.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

static const char *SLEEP_LOG = "/var/log/centurion/sleep.log";
static constexpr int MAX_ROWS = 200;

static QString rd(const QString &p, qint64 max = 4096) {
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.read(max)).trimmed() : QString();
}

static QLabel *muted(const QString &t) {
    auto *l = new QLabel(t);
    l->setWordWrap(true);
    l->setTextFormat(Qt::RichText);
    l->setStyleSheet(QStringLiteral("color:%1").arg(theme::MUTED));
    return l;
}

static QTreeWidget *table(const QStringList &head) {
    auto *t = new QTreeWidget;
    t->setColumnCount(head.size());
    t->setHeaderLabels(head);
    t->setRootIsDecorated(false);
    t->setAlternatingRowColors(true);
    for (int i = 0; i < head.size() - 1; ++i) t->header()->setSectionResizeMode(i, QHeaderView::ResizeToContents);
    t->header()->setStretchLastSection(true);
    return t;
}

SleepPage::SleepPage(QWidget *parent) : QWidget(parent) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(6);
    auto *top = new QHBoxLayout;
    summary_ = new QLabel;
    summary_->setWordWrap(true);
    summary_->setTextFormat(Qt::RichText);
    top->addWidget(summary_, 1);
    auto *bRefresh = new QPushButton(QStringLiteral("Refresh"));
    connect(bRefresh, &QPushButton::clicked, this, &SleepPage::refresh);
    top->addWidget(bRefresh, 0, Qt::AlignTop);
    root->addLayout(top);

    root->addWidget(new QLabel(QStringLiteral("<b>Recent sleeps</b>")));
    sleeps_ = table({QStringLiteral("Went to sleep"), QStringLiteral("Slept"), QStringLiteral("Mode"), QStringLiteral("Battery used"),
                     QStringLiteral("Per hour"), QStringLiteral("HW sleep"), QStringLiteral("dGPU in → out"), QStringLiteral("Woken by")});
    root->addWidget(sleeps_, 2);
    hint_ = muted(QString());
    root->addWidget(hint_);

    root->addWidget(new QLabel(QStringLiteral("<b>Wake sources (ACPI)</b>")));
    wake_ = table({QStringLiteral("Device"), QStringLiteral("Sleep state"), QStringLiteral("May wake"), QStringLiteral("Sysfs node")});
    root->addWidget(wake_, 1);
    sources_ = muted(QString());
    root->addWidget(sources_);
    root->addWidget(muted(QStringLiteral(
        "Untick a device to stop it from waking the machine (e.g. a USB controller whose mouse wakes the laptop in a bag, "
        "or a PCIe port that ends sleep at once). The choice is remembered and restored at boot by the centurion-tune service. "
        "Keep the lid (LID*) and the power button enabled.")));

    connect(wake_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *it, int col) {
        if (filling_ || col != 2) return;
        const QString dev = it->text(0);
        const bool on = it->checkState(2) == Qt::Checked;
        if (!on && dev.startsWith(QLatin1String("LID"))
            && QMessageBox::question(this, QStringLiteral("Wake sources"), QStringLiteral("Opening the lid will no longer wake the machine. Continue?")) != QMessageBox::Yes) {
            fillWake();
            return;
        }
        privileged::run(privileged::helperPath(QStringLiteral("tune-helper")), QJsonObject{{"op", "wakeup_set"}, {"device", dev}, {"enabled", on}}, this,
                        [this, dev](const privileged::Result &r) {
                            if (!r.ok()) QMessageBox::warning(this, QStringLiteral("Wake sources"), dev + QStringLiteral(": ") + r.message());
                            fillWake();
                        });
    });
}

void SleepPage::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    refresh();
}

void SleepPage::refresh() {
    const QString s = QStringLiteral("/sys/power/suspend_stats/");
    QStringList p;
    const QString modes = rd(QStringLiteral("/sys/power/mem_sleep"));
    const int a = modes.indexOf('['), b = modes.indexOf(']');
    const QString mode = a >= 0 && b > a ? modes.mid(a + 1, b - a - 1) : QString();
    if (!mode.isEmpty())
        p << QStringLiteral("Suspend mode: <b>%1</b>%2").arg(mode == QLatin1String("s2idle") ? QStringLiteral("s2idle (modern standby)") : mode == QLatin1String("deep") ? QStringLiteral("deep (S3)") : mode,
                                                           modes.split(' ').size() > 1 ? QStringLiteral(" (offered: %1)").arg(QString(modes).remove('[').remove(']')) : QString());
    if (const QString ok = rd(s + "success"); !ok.isEmpty())
        p << QStringLiteral("since boot: %1 suspends, <span style='color:%2'>%3 failed</span>").arg(ok, rd(s + "fail").toInt() ? theme::DANGER : theme::OK, rd(s + "fail"));
    if (const int fails = rd(s + "fail").toInt(); fails > 0) {
        QString last = rd(s + "last_failed_dev");
        if (const QString step = rd(s + "last_failed_step"); !step.isEmpty()) last += (last.isEmpty() ? QString() : QStringLiteral(" · ")) + step;
        if (const QString err = rd(s + "last_failed_errno"); !err.isEmpty() && err != QLatin1String("0")) last += QStringLiteral(" · errno ") + err;
        if (!last.isEmpty()) p << QStringLiteral("last failure: ") + last.toHtmlEscaped();
    }
    if (const QString hw = rd(s + "total_hw_sleep"); !hw.isEmpty())
        p << QStringLiteral("hardware sleep since boot: %1 h (longest %2 h)").arg(hw.toDouble() / 3.6e9, 0, 'f', 1).arg(rd(s + "max_hw_sleep").toDouble() / 3.6e9, 0, 'f', 1);
    summary_->setText(p.isEmpty() ? QStringLiteral("This kernel reports no suspend statistics.") : p.join(QStringLiteral(" &nbsp;·&nbsp; ")));
    fillSleeps();
    fillWake();
}

void SleepPage::fillSleeps() {
    sleeps_->clear();
    QFile f(QString::fromLatin1(SLEEP_LOG));
    if (!f.open(QIODevice::ReadOnly)) {
        hint_->setText(QStringLiteral("No sleep log yet. It is written by the centurion-sleep-log system-sleep hook (installed by install.sh / the ebuild) "
                                      "from the next suspend on."));
        return;
    }
    if (f.size() > (1 << 20)) f.seek(f.size() - (1 << 20));
    QList<QJsonObject> rows;
    for (const QByteArray &l : f.readAll().split('\n')) {
        const QJsonObject o = QJsonDocument::fromJson(l).object();
        if (!o.isEmpty()) rows << o;
    }
    std::reverse(rows.begin(), rows.end());
    double drainSum = 0, hoursSum = 0;
    for (const QJsonObject &o : rows.mid(0, MAX_ROWS)) {
        const qint64 t0 = o.value("start").toInteger(), t1 = o.value("end").toInteger();
        const double h = (t1 - t0) / 3600.0;
        const QString dur = t1 > t0 ? (h >= 1 ? QStringLiteral("%1 h %2 min").arg(int(h)).arg(int(h * 60) % 60) : QStringLiteral("%1 min").arg((t1 - t0 + 30) / 60)) : QStringLiteral("—");
        const double e0 = o.value("energy_before").toDouble(), e1 = o.value("energy_after").toDouble(), full = o.value("full").toDouble();
        const bool onBattery = o.value("status_before").toString() == QLatin1String("Discharging");
        QString used = QStringLiteral("on AC"), per = QStringLiteral("—");
        if (onBattery && full > 0 && e0 >= e1) {
            const double pct = 100.0 * (e0 - e1) / full;
            used = QStringLiteral("%1%").arg(pct, 0, 'f', 1);
            if (o.value("unit").toString() == QLatin1String("uWh")) used += QStringLiteral(" (%1 Wh)").arg((e0 - e1) / 1e6, 0, 'f', 2);
            if (h >= 0.25) { per = QStringLiteral("%1 %/h").arg(pct / h, 0, 'f', 2); drainSum += pct; hoursSum += h; }
        } else if (!onBattery && o.value("status_before").toString().isEmpty()) {
            used = QStringLiteral("—");
        }
        const double hwUs = o.value("hw_sleep_us").toString().toDouble();
        const QString hw = (hwUs > 0 && t1 > t0) ? QStringLiteral("%1%").arg(std::min(100.0, hwUs / 1e6 / double(t1 - t0) * 100), 0, 'f', 0) : QStringLiteral("—");
        QString woke = o.value("woke_by").toString();
        woke.replace(',', QStringLiteral(", "));
        if (woke.isEmpty() && !o.value("wake_irq").toString().isEmpty()) woke = QStringLiteral("IRQ ") + o.value("wake_irq").toString();
        auto *it = new QTreeWidgetItem({QDateTime::fromSecsSinceEpoch(t0).toString(QStringLiteral("yyyy-MM-dd HH:mm")), dur,
                                        o.value("mode").toString().isEmpty() ? o.value("kind").toString() : o.value("mode").toString(),
                                        used, per, hw, o.value("dgpu_before").toString() + QStringLiteral(" → ") + o.value("dgpu_after").toString(), woke});
        // A sleep that kept the SoC mostly awake, or drained fast, stands out.
        if ((hwUs > 0 && t1 - t0 > 600 && hwUs / 1e6 / double(t1 - t0) < 0.5) || (per != QLatin1String("—") && per.section(' ', 0, 0).toDouble() > 2.0))
            for (int i = 0; i < 8; ++i) it->setForeground(i, QColor(QLatin1String(theme::WARN)));
        if (o.value("dgpu_after").toString() == QLatin1String("error"))
            for (int i = 0; i < 8; ++i) it->setForeground(i, QColor(QLatin1String(theme::DANGER)));
        sleeps_->addTopLevelItem(it);
    }
    hint_->setText(hoursSum > 0 ? QStringLiteral("Average on battery: %1 %/h over %2 h of sleep. Orange rows: hardware sleep under half the time, or more than 2 %/h. "
                                                 "Red: the dGPU did not come back.").arg(drainSum / hoursSum, 0, 'f', 2).arg(hoursSum, 0, 'f', 1)
                                : QStringLiteral("Battery drain shows for sleeps of 15 min or more that started on battery."));
}

void SleepPage::fillWake() {
    filling_ = true;
    wake_->clear();
    const QStringList lines = rd(QStringLiteral("/proc/acpi/wakeup"), 64 * 1024).split('\n');
    for (int i = 1; i < lines.size(); ++i) {
        const QStringList f = lines.at(i).simplified().split(' ');
        if (f.size() < 3) continue;
        const bool on = f.at(2).contains(QLatin1String("enabled"));
        auto *it = new QTreeWidgetItem({f.at(0), f.at(1), QString(), f.size() > 3 ? f.at(3) : QString()});
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(2, on ? Qt::Checked : Qt::Unchecked);
        wake_->addTopLevelItem(it);
    }
    if (!wake_->topLevelItemCount()) wake_->addTopLevelItem(new QTreeWidgetItem({QStringLiteral("/proc/acpi/wakeup is not available."), {}, {}, {}}));
    filling_ = false;
    // Who actually woke the machine most (kernel wakeup sources, all of them, since boot).
    QList<QPair<qint64, QString>> top;
    for (const QString &w : QDir(QStringLiteral("/sys/class/wakeup")).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System)) {
        const qint64 n = rd("/sys/class/wakeup/" + w + "/wakeup_count").toLongLong();
        if (n > 0) top.append({n, rd("/sys/class/wakeup/" + w + "/name")});
    }
    std::sort(top.begin(), top.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    QStringList l;
    for (const auto &[n, name] : top.mid(0, 6)) l << QStringLiteral("%1 ×%2").arg(name.toHtmlEscaped()).arg(n);
    sources_->setText(l.isEmpty() ? QStringLiteral("No wakeup events counted since boot.") : QStringLiteral("Wakeups since boot by source: ") + l.join(QStringLiteral(", ")));
}
