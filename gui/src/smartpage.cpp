#include "smartpage.h"

#include "privileged.h"
#include "smartwatch.h"
#include "theme.h"

#include <QAbstractItemView>
#include <QColor>
#include <QCoreApplication>
#include <QDate>
#include <QDialog>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QHeaderView>
#include <QSet>
#include <QDateTime>
#include <QScrollBar>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QLabel>
#include <QFrame>
#include <QScrollArea>
#include <QFont>
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

void historyWorkspace(QWidget *owner) {
    const auto snapshot=smart::historySnapshot();
    const auto all=snapshot.value_or(QJsonObject{}).value(QStringLiteral("disks")).toObject();
    QDialog dlg(owner);dlg.setWindowTitle(SmartPage::tr("Recorded Drive History"));dlg.resize(1100,650);
    auto *layout=new QVBoxLayout(&dlg);
    auto *note=new QLabel(SmartPage::tr("Saved daily SMART samples only. Opening this view does not query drives or start a self-test. Missing values are not zero; older ambiguous identities are excluded."));note->setWordWrap(true);layout->addWidget(note);
    if(!snapshot) layout->addWidget(new QLabel(SmartPage::tr("History could not be read or is damaged. The file is preserved.")));
    auto *row=new QHBoxLayout;auto *device=new QComboBox;auto *period=new QComboBox;
    period->addItem(SmartPage::tr("All recorded days"),0);period->addItem(SmartPage::tr("Last 30 days"),30);period->addItem(SmartPage::tr("Last 90 days"),90);
    for(auto it=all.cbegin();it!=all.cend();++it) if(smart::usableIdentity(it.key())) device->addItem(it.value().toObject().value("model").toString()+QStringLiteral(" · ")+it.key().section(QLatin1Char('|'),-1),it.key());
    row->addWidget(device,1);row->addWidget(period);layout->addLayout(row);
    auto *table=new QTreeWidget;table->setHeaderLabels({SmartPage::tr("Date"),SmartPage::tr("Temperature (°C)"),SmartPage::tr("Life used (%)"),SmartPage::tr("Spare (%)"),SmartPage::tr("Spare threshold (%)"),SmartPage::tr("Media errors"),SmartPage::tr("Unsafe shutdowns"),SmartPage::tr("Written (TB)"),SmartPage::tr("Power-on hours"),SmartPage::tr("Critical warning")});
    table->setRootIsDecorated(false);table->setAlternatingRowColors(true);table->setUniformRowHeights(true);table->header()->setSectionResizeMode(QHeaderView::ResizeToContents);layout->addWidget(table,1);
    auto *summary=new QLabel;layout->addWidget(summary);
    const auto refresh=[&]{
        table->clear();const auto samples=all.value(device->currentData().toString()).toObject().value("samples").toArray();
        const int days=period->currentData().toInt();const auto cutoff=QDate::currentDate().addDays(-days);
        for(qsizetype i=samples.size();i>0;--i) {
            const auto sample=samples.at(i-1).toObject();const auto date=QDate::fromString(sample.value("d").toString(),Qt::ISODate);
            if(!date.isValid()||(days>0&&date<cutoff)) continue;
            QStringList cells{date.toString(Qt::ISODate)};
            for(const char *key:{"temp_c","life_used_pct","spare_pct","spare_threshold_pct","media_errors","unsafe_shutdowns","written_tb","power_on_hours","critical_warning"}) {
                const auto value=sample.value(QLatin1String(key));
                // Qt6 preserves signed 64-bit integers in parsed JSON. Do not round counters through double display.
                cells<< (value.isDouble()?QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact)).mid(1).chopped(1):QStringLiteral("—"));
            }
            auto *item=new QTreeWidgetItem(table,cells);item->setToolTip(0,sample.value("captured_utc").toString());
        }
        summary->setText(SmartPage::tr("%1 recorded days. No entry means no retained measurement.").arg(table->topLevelItemCount()));
    };
    QObject::connect(device,&QComboBox::currentIndexChanged,&dlg,[&](int){refresh();});QObject::connect(period,&QComboBox::currentIndexChanged,&dlg,[&](int){refresh();});refresh();
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Close);QObject::connect(buttons,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);layout->addWidget(buttons);dlg.exec();
}

} // namespace

SmartPage::SmartPage(QWidget *parent) : QWidget(parent) {
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(8, 6, 8, 6);
    auto *top = new QHBoxLayout;
    auto *about = new QLabel(tr("Storage health · Device status, temperature and drive wear. Run a check to update the readings."));
    about->setWordWrap(true);
    about->setProperty("role", "muted");
    top->addWidget(about, 1);
    auto *run = new QPushButton(tr("Check drives"));
    top->addWidget(run);
    auto *history=new QPushButton(tr("Recorded History…"));top->addWidget(history);
    connect(history,&QPushButton::clicked,this,[this]{historyWorkspace(this);});
    v->addLayout(top);
    status_ = new QLabel;
    status_->setProperty("role", "muted");
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    v->addWidget(status_);
    alert_ = new QLabel;
    alert_->setWordWrap(true);
    alert_->setVisible(false);
    v->addWidget(alert_);

    detailsButton_ = new QPushButton(tr("Show technical details"));
    detailsButton_->setCheckable(true);
    detailsButton_->setChecked(false);
    detailsButton_->setToolTip(tr("Show the full SMART values and 30-day media-error trends."));
    v->addWidget(detailsButton_, 0, Qt::AlignLeft);
    auto *scroll = new QScrollArea;
    summaryView_ = scroll;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    cardsWidget_ = new QWidget;
    cardsLayout_ = new QVBoxLayout(cardsWidget_);
    cardsLayout_->setContentsMargins(0, 0, 5, 0);
    cardsLayout_->setSpacing(10);
    auto *placeholder = new QLabel(tr("Drive status will appear here after a check."));
    placeholder->setProperty("role", "muted");
    cardsLayout_->addWidget(placeholder);
    cardsLayout_->addStretch();
    scroll->setWidget(cardsWidget_);
    v->addWidget(summaryView_, 1);

    table_ = new QTreeWidget;
    table_->setHeaderLabels({QStringLiteral("Disk"), QStringLiteral("Model"), QStringLiteral("Temp"), QStringLiteral("Life Used"),
                             QStringLiteral("Spare"), QStringLiteral("Media errors"), QStringLiteral("Media, 30 days"),
                             QStringLiteral("Unsafe shutdowns"), QStringLiteral("Written"), QStringLiteral("Critical"),
                             QStringLiteral("Self-test")});
    table_->setRootIsDecorated(false);
    table_->setAlternatingRowColors(true);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setVisible(false);
    v->addWidget(table_, 1);
    connect(detailsButton_, &QPushButton::toggled, this, [this](bool checked) {
        summaryView_->setVisible(!checked);
        table_->setVisible(checked);
        detailsButton_->setText(checked ? tr("Show simple overview") : tr("Show technical details"));
    });
    connect(run, &QPushButton::clicked, this, [this] { start(); });
}

void SmartPage::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    if (!ran_) start();  // lazy: nothing runs until the page is opened
}

void SmartPage::start() {
    if (busy_) return;
    ran_ = busy_ = true;
    status_->setText(tr("Reading SMART information…") + (lastChecked_.isValid() ? tr(" · Displaying previous readings from %1").arg(lastChecked_.toString(Qt::ISODate)) : QString()));
    privileged::run(privileged::helperPath(QStringLiteral("tune-helper")),
                    QJsonObject{{"op", "tool"}, {"tool", "smart_json"}}, this, [this](const privileged::Result &r) {
        busy_ = false;
        auto failure = [this](const QString &why) {
            status_->setText(why + (lastChecked_.isValid() ? tr(" · Displaying previous readings from %1").arg(lastChecked_.toString(Qt::ISODate)) : QString()));
        };
        if (!r.ok()) { failure(tr("Check failed: %1").arg(r.message())); return; }
        const QString missing = r.json.value(QStringLiteral("missing")).toString();
        if (!missing.isEmpty()) { failure(QCoreApplication::translate("systools", "(not installed — %1)").arg(missing)); return; }
        const QJsonArray disks = r.json.value(QStringLiteral("disks")).toArray();
        if (disks.isEmpty()) { failure(tr("No disks found.")); return; }
        render(disks, smart::record(disks));
        lastChecked_ = QDateTime::currentDateTime();
        status_->setText(tr("Last checked: %1 · More fields are available under technical details.")
                             .arg(lastChecked_.toString(Qt::ISODate)));
    }, CALL_TIMEOUT_MS);
}

void SmartPage::render(const QJsonArray &disks, const QStringList &alerts) {
    const QJsonObject all = smart::loadHistory().value(QStringLiteral("disks")).toObject();
    const QString cutoff = QDate::currentDate().addDays(-TREND_DAYS).toString(Qt::ISODate);
    alert_->setText(alerts.join(QLatin1Char('\n')));
    theme::setSheet(alert_, QStringLiteral("color: %1").arg(QString::fromLatin1(theme::DANGER)));
    alert_->setVisible(!alerts.isEmpty());

    const int tableScroll = table_->verticalScrollBar()->value();
    table_->clear();
    while (QLayoutItem *item = cardsLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    QSet<QString> ids,duplicates;
    for(const auto &v:disks) {const auto id=v.toObject().value("id").toString();if(ids.contains(id)) duplicates.insert(id);ids.insert(id);}
    for (const QJsonValue &v : disks) {
        const QJsonObject d = v.toObject();
        const QString dev = d.value(QStringLiteral("dev")).toString();
        if (d.contains(QStringLiteral("error"))) {
            table_->addTopLevelItem(new QTreeWidgetItem({dev, d.value(QStringLiteral("error")).toString()}));
            auto *failure = new QLabel(tr("%1 — Couldn't read SMART data: %2")
                                       .arg(dev, d.value(QStringLiteral("error")).toString()));
            failure->setWordWrap(true);
            failure->setProperty("tone", "warn");
            cardsLayout_->addWidget(failure);
            continue;
        }
        // Trend: media errors against the first sample from the last 30 days.
        const auto id=d.value(QStringLiteral("id")).toString();
        const QJsonArray samples = smart::usableIdentity(id)&&!duplicates.contains(id)?all.value(id).toObject().value(QStringLiteral("samples")).toArray():QJsonArray{};
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

        const bool faulty = (crit && *crit != 0) || (passedV.isBool() && !passedV.toBool()) ||
                            (spare && threshold && *spare < *threshold);
        const bool caution = !faulty && ((delta && *delta > 0) || (life && *life >= 80));
        const bool insufficient = !crit && !passedV.isBool() && !temp && !life && !media;
        auto *card = new QFrame;
        card->setObjectName(QStringLiteral("driveCard"));
        card->setStyleSheet(QStringLiteral("QFrame#driveCard{background:%1;border:1px solid %2;border-radius:11px}"
                                           "QFrame#driveCard QLabel{background:transparent;border:none}")
                            .arg(theme::BG2, theme::BORDER));
        auto *col = new QVBoxLayout(card);
        col->setContentsMargins(14, 12, 14, 12);
        col->setSpacing(9);
        auto *heading = new QHBoxLayout;
        auto *model = new QLabel(d.value(QStringLiteral("model")).toString().isEmpty()
                                     ? dev : d.value(QStringLiteral("model")).toString());
        model->setTextFormat(Qt::PlainText);
        QFont modelFont = model->font();
        modelFont.setBold(true);
        model->setFont(modelFont);
        heading->addWidget(model, 1);
        auto *verdict = new QLabel(faulty ? tr("Needs attention") : caution ? tr("Review") :
                                  insufficient ? tr("Not checked") : tr("No alerts found"));
        verdict->setStyleSheet(QStringLiteral("color:%1;font-weight:600")
                               .arg(faulty ? theme::DANGER : caution ? theme::WARN :
                                    insufficient ? theme::MUTED : theme::OK));
        heading->addWidget(verdict);
        col->addLayout(heading);
        auto *location = new QLabel(dev);
        location->setProperty("tone", "muted");
        col->addWidget(location);
        auto *metrics = new QLabel(tr("Temperature: %1    ·    Life used: %2    ·    Media errors: %3")
                                    .arg(num(temp, tr(" °C")), num(life, tr(" %")), num(media)));
        metrics->setWordWrap(true);
        col->addWidget(metrics);
        QString note;
        if (faulty) note = tr("A critical SMART indicator was reported. Back up important files and inspect the detailed values.");
        else if (caution) note = tr("Drive wear or media errors deserve a closer look. Compare with your previous measurements.");
        else if (insufficient)
            note = tr("The drive did not provide enough SMART indicators to assess its condition.");
        else note = tr("No warning indicators in the available SMART data. This is not a guarantee against drive failure.");
        auto *description = new QLabel(note);
        description->setWordWrap(true);
        description->setProperty("tone", "dim");
        col->addWidget(description);
        // What the counters add up to: age, write rate, a wear projection, temperature.
        QStringList insights;
        const auto hours = smart::field(d, "power_on_hours"), written = smart::field(d, "written_tb");
        if (hours && *hours > 0) {
            const double days = *hours / 24.0;
            insights << (days >= 365 ? tr("Powered on for %1 years (%L2 h).").arg(days / 365.0, 0, 'f', 1).arg(qint64(*hours))
                                     : tr("Powered on for %1 days (%L2 h).").arg(qint64(days)).arg(qint64(*hours)));
            if (written && *written > 0 && days >= 1) {
                const double perDay = *written * 1000.0 / days;   // GB per powered-on day
                insights << tr("Writes about %1 GB per powered-on day.").arg(perDay, 0, 'f', perDay < 10 ? 1 : 0);
                // "Life used" is a whole percentage: below a few percent the projection is mostly rounding.
                if (life && *life >= 3 && *life < 100 && perDay > 0.01) {
                    const double leftTb = *written / *life * (100.0 - *life);
                    const double leftYears = leftTb * 1000.0 / perDay / 365.0;
                    insights << tr("At this pace the rated wear lasts for roughly %1 TB more (≈ %2 years of powered-on use). "
                                   "Drives usually keep working past 100 %; it is the manufacturer's endurance rating.")
                                    .arg(leftTb, 0, 'f', leftTb < 10 ? 1 : 0).arg(leftYears > 50 ? QStringLiteral("50+") : QString::number(leftYears, 'f', 0));
                }
            }
        }
        if (temp && *temp >= 70 && *temp < 150)   // above that a sensor reads garbage, not heat
            insights << tr("Running hot (%1 °C): NVMe drives throttle around 70–80 °C; a heatsink or airflow helps.").arg(*temp);
        if (const auto unsafe = smart::field(d, "unsafe_shutdowns"); unsafe && hours && *hours > 0 && *unsafe > 0 && *unsafe / (*hours / 24.0) > 0.2)
            insights << tr("%1 unsafe shutdowns: power was cut without the drive being told (forced power-offs, crashes). "
                           "Each one risks recently written data.").arg(*unsafe);
        if (delta && *delta > 0) insights << tr("%1 new media error(s) in the last %2 days: back up and run a long self-test (smartctl -t long).").arg(*delta).arg(TREND_DAYS);
        if (!insights.isEmpty()) {
            auto *more = new QLabel(QStringLiteral("• ") + insights.join(QStringLiteral("\n• ")));
            more->setWordWrap(true);
            more->setTextFormat(Qt::PlainText);
            col->addWidget(more);
        }
        cardsLayout_->addWidget(card);
    }
    cardsLayout_->addStretch();
    table_->verticalScrollBar()->setValue(tableScroll);
}
