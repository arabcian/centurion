#include "smartpage.h"

#include "privileged.h"
#include "smartwatch.h"
#include "theme.h"

#include <QAbstractItemView>
#include <QColor>
#include <QDate>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <optional>

namespace {

constexpr int CALL_TIMEOUT_MS = 120000;
constexpr int TREND_DAYS = 30;

QString num(std::optional<double> x, const QString &suffix = {}) {
    return x ? QString::number(*x) + suffix : QStringLiteral("—");
}

} // namespace

SmartPage::SmartPage(QWidget *parent) : QWidget(parent) {
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(8, 6, 8, 6);
    auto *top = new QHBoxLayout;
    auto *about = new QLabel(QStringLiteral("Drive health (root) for every disk. Trends compare with the first sample of the last "
                                            "30 days. Raw smartctl output is on the SMART details tab."));
    about->setWordWrap(true);
    about->setProperty("role", "muted");
    top->addWidget(about, 1);
    auto *run = new QPushButton(QStringLiteral("Run again"));
    top->addWidget(run);
    v->addLayout(top);
    status_ = new QLabel;
    status_->setProperty("role", "muted");
    v->addWidget(status_);
    alert_ = new QLabel;
    alert_->setWordWrap(true);
    alert_->setVisible(false);
    v->addWidget(alert_);
    table_ = new QTreeWidget;
    table_->setHeaderLabels({QStringLiteral("Disk"), QStringLiteral("Model"), QStringLiteral("Temp"), QStringLiteral("Life used"),
                             QStringLiteral("Spare"), QStringLiteral("Media errors"), QStringLiteral("Media, 30 days"),
                             QStringLiteral("Unsafe shutdowns"), QStringLiteral("Written"), QStringLiteral("Critical"),
                             QStringLiteral("Self-test")});
    table_->setRootIsDecorated(false);
    table_->setAlternatingRowColors(true);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    v->addWidget(table_, 1);
    connect(run, &QPushButton::clicked, this, [this] { start(); });
}

void SmartPage::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    if (!ran_) start();  // lazy: nothing runs until the page is opened
}

void SmartPage::start() {
    if (busy_) return;
    ran_ = busy_ = true;
    status_->setText(QStringLiteral("Running…"));
    privileged::run(privileged::helperPath(QStringLiteral("tune-helper")),
                    QJsonObject{{"op", "tool"}, {"tool", "smart_json"}}, this, [this](const privileged::Result &r) {
        busy_ = false;
        if (!r.ok()) { status_->setText(QStringLiteral("Not run: ") + r.message()); return; }
        const QString missing = r.json.value(QStringLiteral("missing")).toString();
        if (!missing.isEmpty()) { status_->setText(QStringLiteral("(not installed — emerge %1)").arg(missing)); return; }
        const QJsonArray disks = r.json.value(QStringLiteral("disks")).toArray();
        if (disks.isEmpty()) { status_->setText(QStringLiteral("No disks found.")); return; }
        render(disks, smart::record(disks));
        status_->setText(QStringLiteral("Sample recorded %1.").arg(QDate::currentDate().toString(Qt::ISODate)));
    }, CALL_TIMEOUT_MS);
}

void SmartPage::render(const QJsonArray &disks, const QStringList &alerts) {
    const QJsonObject all = smart::loadHistory().value(QStringLiteral("disks")).toObject();
    const QString cutoff = QDate::currentDate().addDays(-TREND_DAYS).toString(Qt::ISODate);
    alert_->setText(alerts.join(QLatin1Char('\n')));
    theme::setSheet(alert_, QStringLiteral("color: %1").arg(QString::fromLatin1(theme::DANGER)));
    alert_->setVisible(!alerts.isEmpty());

    table_->clear();
    for (const QJsonValue &v : disks) {
        const QJsonObject d = v.toObject();
        const QString dev = d.value(QStringLiteral("dev")).toString();
        if (d.contains(QStringLiteral("error"))) {
            table_->addTopLevelItem(new QTreeWidgetItem({dev, d.value(QStringLiteral("error")).toString()}));
            continue;
        }
        // Trend: media errors against the first sample from the last 30 days.
        const QJsonArray samples = all.value(d.value(QStringLiteral("id")).toString()).toObject().value(QStringLiteral("samples")).toArray();
        std::optional<double> base;
        for (const QJsonValue &s : samples) {
            const QJsonObject o = s.toObject();
            if (o.value(QStringLiteral("d")).toString() >= cutoff) { base = smart::field(o, "media_errors"); break; }
        }
        const auto media = smart::field(d, "media_errors");
        const std::optional<double> delta = media && base ? std::optional<double>(*media - *base) : std::nullopt;
        const auto temp = smart::field(d, "temp_c"), life = smart::field(d, "life_used_pct");
        const auto spare = smart::field(d, "spare_pct"), threshold = smart::field(d, "spare_threshold_pct");
        const auto crit = smart::field(d, "critical_warning");
        const QJsonValue passedV = d.value(QStringLiteral("passed"));

        const QString spareTxt = spare ? num(spare, QStringLiteral(" %")) + (threshold ? QStringLiteral(" (min %1)").arg(*threshold) : QString())
                                       : QStringLiteral("—");
        const QString deltaTxt = delta ? (*delta > 0 ? QStringLiteral("+%1").arg(*delta) : QString::number(*delta)) : QStringLiteral("—");
        const QString passedTxt = passedV.isBool() ? (passedV.toBool() ? QStringLiteral("PASSED") : QStringLiteral("FAILED")) : QStringLiteral("—");

        auto *it = new QTreeWidgetItem({dev, d.value(QStringLiteral("model")).toString(), num(temp, QStringLiteral(" °C")),
                                        num(life, QStringLiteral(" %")), spareTxt, num(media), deltaTxt,
                                        num(smart::field(d, "unsafe_shutdowns")), num(smart::field(d, "written_tb"), QStringLiteral(" TB")),
                                        num(crit), passedTxt});
        auto mark = [it](int col, const char *color) { it->setForeground(col, QColor(color)); };
        if (crit && *crit != 0) mark(9, theme::DANGER);
        if (passedV.isBool() && !passedV.toBool()) mark(10, theme::DANGER);
        if (spare && threshold && *spare < *threshold) mark(4, theme::DANGER);
        if (delta && *delta > 0) mark(6, theme::WARN);
        if (life && *life >= 80) mark(3, theme::WARN);
        table_->addTopLevelItem(it);
    }
}
